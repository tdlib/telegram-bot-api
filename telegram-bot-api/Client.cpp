//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2021
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/Client.h"

#include "telegram-bot-api/ClientParameters.h"

#include "td/db/TQueue.h"

#include "td/actor/PromiseFuture.h"
#include "td/actor/SleepActor.h"

#include "td/utils/algorithm.h"
#include "td/utils/base64.h"
#include "td/utils/filesystem.h"
#include "td/utils/HttpUrl.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/PathView.h"
#include "td/utils/port/path.h"
#include "td/utils/port/Stat.h"
#include "td/utils/Slice.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/Span.h"
#include "td/utils/StackAllocator.h"
#include "td/utils/Status.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/Time.h"
#include "td/utils/utf8.h"

#include <cstdlib>

namespace telegram_bot_api {

using td::Jsonable;
using td::JsonValueScope;
using td::JsonValue;

using td_api::make_object;
using td_api::move_object_as;

void Client::fail_query_with_error(PromisedQueryPtr query, int32 error_code, Slice error_message,
                                   Slice default_message) {
  if (error_code == 429) {
    Slice prefix = "Too Many Requests: retry after ";
    if (begins_with(error_message, prefix)) {
      auto r_retry_after = td::to_integer_safe<int>(error_message.substr(prefix.size()));
      if (r_retry_after.is_ok() && r_retry_after.ok() > 0) {
        return query->set_retry_after_error(r_retry_after.ok());
      }
    }
    LOG(ERROR) << "Wrong error message: " << error_message << " from " << *query;
    return fail_query(500, error_message, std::move(query));
  }
  int32 real_error_code = error_code;
  Slice real_error_message = error_message;
  if (error_code < 400 || error_code == 404) {
    if (error_code < 200) {
      LOG(ERROR) << "Receive error \"" << real_error_message << "\" with code " << error_code << " from " << *query;
    }

    error_code = 400;
  } else if (error_code == 403) {
    bool is_server_error = true;
    for (auto c : error_message) {
      if (c == '_' || ('A' <= c && c <= 'Z') || td::is_digit(c)) {
        continue;
      }

      is_server_error = false;
      break;
    }
    if (is_server_error) {
      error_code = 400;
    }
  }
  if (error_code == 400) {
    if (!default_message.empty()) {
      error_message = default_message;
    }
    if (error_message == "MESSAGE_NOT_MODIFIED") {
      error_message = Slice(
          "message is not modified: specified new message content and reply markup are exactly the same as a current "
          "content and reply markup of the message");
    } else if (error_message == "WC_CONVERT_URL_INVALID" || error_message == "EXTERNAL_URL_INVALID") {
      error_message = "Wrong HTTP URL specified";
    } else if (error_message == "WEBPAGE_CURL_FAILED") {
      error_message = "Failed to get HTTP URL content";
    } else if (error_message == "WEBPAGE_MEDIA_EMPTY") {
      error_message = "Wrong type of the web page content";
    } else if (error_message == "MEDIA_GROUPED_INVALID") {
      error_message = "Can't use the media of the specified type in the album";
    } else if (error_message == "REPLY_MARKUP_TOO_LONG") {
      error_message = Slice("reply markup is too long");
    } else if (error_message == "INPUT_USER_DEACTIVATED") {
      error_code = 403;
      error_message = Slice("Forbidden: user is deactivated");
    } else if (error_message == "USER_IS_BLOCKED") {
      error_code = 403;
      error_message = Slice("bot was blocked by the user");
    } else if (error_message == "USER_ADMIN_INVALID") {
      error_code = 400;
      error_message = Slice("user is an administrator of the chat");
    } else if (error_message == "File generation failed") {
      error_code = 400;
      error_message = Slice("can't upload file by URL");
    } else if (error_message == "CHAT_ABOUT_NOT_MODIFIED") {
      error_code = 400;
      error_message = Slice("chat description is not modified");
    } else if (error_message == "PACK_SHORT_NAME_INVALID") {
      error_code = 400;
      error_message = Slice("invalid sticker set name is specified");
    } else if (error_message == "PACK_SHORT_NAME_OCCUPIED") {
      error_code = 400;
      error_message = Slice("sticker set name is already occupied");
    } else if (error_message == "STICKER_EMOJI_INVALID") {
      error_code = 400;
      error_message = Slice("invalid sticker emojis");
    } else if (error_message == "QUERY_ID_INVALID") {
      error_code = 400;
      error_message = Slice("query is too old and response timeout expired or query ID is invalid");
    } else if (error_message == "MESSAGE_DELETE_FORBIDDEN") {
      error_code = 400;
      error_message = Slice("message can't be deleted");
    }
  }
  Slice prefix;
  switch (error_code) {
    case 400:
      prefix = Slice("Bad Request");
      break;
    case 401:
      prefix = Slice("Unauthorized");
      break;
    case 403:
      prefix = Slice("Forbidden");
      break;
    case 500:
      prefix = Slice("Internal Server Error");
      if (real_error_message != Slice("Request aborted")) {
        LOG(ERROR) << "Receive Internal Server Error \"" << real_error_message << "\" from " << *query;
      }
      break;
    default:
      LOG(ERROR) << "Unsupported error " << real_error_code << ": " << real_error_message << " from " << *query;
      return fail_query(400, PSLICE() << "Bad Request: " << error_message, std::move(query));
  }

  if (begins_with(error_message, prefix)) {
    return fail_query(error_code, error_message, std::move(query));
  } else {
    td::string error_str = prefix.str();
    if (error_message.empty()) {
      LOG(ERROR) << "Empty error message with code " << real_error_code << " from " << *query;
    } else {
      error_str += ": ";
      if (error_message.size() >= 2u &&
          (error_message[1] == '_' || ('A' <= error_message[1] && error_message[1] <= 'Z'))) {
        error_str += error_message.str();
      } else {
        error_str += td::to_lower(error_message[0]);
        error_str += error_message.substr(1).str();
      }
    }
    return fail_query(error_code, error_str, std::move(query));
  }
}

void Client::fail_query_with_error(PromisedQueryPtr &&query, object_ptr<td_api::error> error, Slice default_message) {
  fail_query_with_error(std::move(query), error->code_, error->message_, default_message);
}

Client::Client(td::ActorShared<> parent, const td::string &bot_token, bool is_test_dc, int64 tqueue_id,
               std::shared_ptr<const ClientParameters> parameters, td::ActorId<BotStatActor> stat_actor)
    : parent_(std::move(parent))
    , bot_token_(bot_token)
    , bot_token_id_("<unknown>")
    , is_test_dc_(is_test_dc)
    , tqueue_id_(tqueue_id)
    , parameters_(std::move(parameters))
    , stat_actor_(std::move(stat_actor)) {
  static auto is_inited = init_methods();
  CHECK(is_inited);
}

bool Client::init_methods() {
  methods_.emplace("getme", &Client::process_get_me_query);
  methods_.emplace("getmycommands", &Client::process_get_my_commands_query);
  methods_.emplace("setmycommands", &Client::process_set_my_commands_query);
  methods_.emplace("deletemycommands", &Client::process_delete_my_commands_query);
  methods_.emplace("getmydefaultadministratorrights", &Client::process_get_my_default_administrator_rights_query);
  methods_.emplace("setmydefaultadministratorrights", &Client::process_set_my_default_administrator_rights_query);
  methods_.emplace("getchatmenubutton", &Client::process_get_chat_menu_button_query);
  methods_.emplace("setchatmenubutton", &Client::process_set_chat_menu_button_query);
  methods_.emplace("getuserprofilephotos", &Client::process_get_user_profile_photos_query);
  methods_.emplace("sendmessage", &Client::process_send_message_query);
  methods_.emplace("sendanimation", &Client::process_send_animation_query);
  methods_.emplace("sendaudio", &Client::process_send_audio_query);
  methods_.emplace("senddice", &Client::process_send_dice_query);
  methods_.emplace("senddocument", &Client::process_send_document_query);
  methods_.emplace("sendphoto", &Client::process_send_photo_query);
  methods_.emplace("sendsticker", &Client::process_send_sticker_query);
  methods_.emplace("sendvideo", &Client::process_send_video_query);
  methods_.emplace("sendvideonote", &Client::process_send_video_note_query);
  methods_.emplace("sendvoice", &Client::process_send_voice_query);
  methods_.emplace("sendgame", &Client::process_send_game_query);
  methods_.emplace("sendinvoice", &Client::process_send_invoice_query);
  methods_.emplace("sendlocation", &Client::process_send_location_query);
  methods_.emplace("sendvenue", &Client::process_send_venue_query);
  methods_.emplace("sendcontact", &Client::process_send_contact_query);
  methods_.emplace("sendpoll", &Client::process_send_poll_query);
  methods_.emplace("stoppoll", &Client::process_stop_poll_query);
  methods_.emplace("copymessage", &Client::process_copy_message_query);
  methods_.emplace("forwardmessage", &Client::process_forward_message_query);
  methods_.emplace("sendmediagroup", &Client::process_send_media_group_query);
  methods_.emplace("sendchataction", &Client::process_send_chat_action_query);
  methods_.emplace("editmessagetext", &Client::process_edit_message_text_query);
  methods_.emplace("editmessagelivelocation", &Client::process_edit_message_live_location_query);
  methods_.emplace("stopmessagelivelocation", &Client::process_edit_message_live_location_query);
  methods_.emplace("editmessagemedia", &Client::process_edit_message_media_query);
  methods_.emplace("editmessagecaption", &Client::process_edit_message_caption_query);
  methods_.emplace("editmessagereplymarkup", &Client::process_edit_message_reply_markup_query);
  methods_.emplace("deletemessage", &Client::process_delete_message_query);
  methods_.emplace("createinvoicelink", &Client::process_create_invoice_link_query);
  methods_.emplace("setgamescore", &Client::process_set_game_score_query);
  methods_.emplace("getgamehighscores", &Client::process_get_game_high_scores_query);
  methods_.emplace("answerwebappquery", &Client::process_answer_web_app_query_query);
  methods_.emplace("answerinlinequery", &Client::process_answer_inline_query_query);
  methods_.emplace("answercallbackquery", &Client::process_answer_callback_query_query);
  methods_.emplace("answershippingquery", &Client::process_answer_shipping_query_query);
  methods_.emplace("answerprecheckoutquery", &Client::process_answer_pre_checkout_query_query);
  methods_.emplace("exportchatinvitelink", &Client::process_export_chat_invite_link_query);
  methods_.emplace("createchatinvitelink", &Client::process_create_chat_invite_link_query);
  methods_.emplace("editchatinvitelink", &Client::process_edit_chat_invite_link_query);
  methods_.emplace("revokechatinvitelink", &Client::process_revoke_chat_invite_link_query);
  methods_.emplace("getchat", &Client::process_get_chat_query);
  methods_.emplace("setchatphoto", &Client::process_set_chat_photo_query);
  methods_.emplace("deletechatphoto", &Client::process_delete_chat_photo_query);
  methods_.emplace("setchattitle", &Client::process_set_chat_title_query);
  methods_.emplace("setchatpermissions", &Client::process_set_chat_permissions_query);
  methods_.emplace("setchatdescription", &Client::process_set_chat_description_query);
  methods_.emplace("pinchatmessage", &Client::process_pin_chat_message_query);
  methods_.emplace("unpinchatmessage", &Client::process_unpin_chat_message_query);
  methods_.emplace("unpinallchatmessages", &Client::process_unpin_all_chat_messages_query);
  methods_.emplace("setchatstickerset", &Client::process_set_chat_sticker_set_query);
  methods_.emplace("deletechatstickerset", &Client::process_delete_chat_sticker_set_query);
  methods_.emplace("getchatmember", &Client::process_get_chat_member_query);
  methods_.emplace("getchatadministrators", &Client::process_get_chat_administrators_query);
  methods_.emplace("getchatmembercount", &Client::process_get_chat_member_count_query);
  methods_.emplace("getchatmemberscount", &Client::process_get_chat_member_count_query);
  methods_.emplace("leavechat", &Client::process_leave_chat_query);
  methods_.emplace("promotechatmember", &Client::process_promote_chat_member_query);
  methods_.emplace("setchatadministratorcustomtitle", &Client::process_set_chat_administrator_custom_title_query);
  methods_.emplace("banchatmember", &Client::process_ban_chat_member_query);
  methods_.emplace("kickchatmember", &Client::process_ban_chat_member_query);
  methods_.emplace("restrictchatmember", &Client::process_restrict_chat_member_query);
  methods_.emplace("unbanchatmember", &Client::process_unban_chat_member_query);
  methods_.emplace("banchatsenderchat", &Client::process_ban_chat_sender_chat_query);
  methods_.emplace("unbanchatsenderchat", &Client::process_unban_chat_sender_chat_query);
  methods_.emplace("approvechatjoinrequest", &Client::process_approve_chat_join_request_query);
  methods_.emplace("declinechatjoinrequest", &Client::process_decline_chat_join_request_query);
  methods_.emplace("getstickerset", &Client::process_get_sticker_set_query);
  methods_.emplace("uploadstickerfile", &Client::process_upload_sticker_file_query);
  methods_.emplace("createnewstickerset", &Client::process_create_new_sticker_set_query);
  methods_.emplace("addstickertoset", &Client::process_add_sticker_to_set_query);
  methods_.emplace("setstickersetthumb", &Client::process_set_sticker_set_thumb_query);
  methods_.emplace("setstickerpositioninset", &Client::process_set_sticker_position_in_set_query);
  methods_.emplace("deletestickerfromset", &Client::process_delete_sticker_from_set_query);
  methods_.emplace("setpassportdataerrors", &Client::process_set_passport_data_errors_query);
  methods_.emplace("sendcustomrequest", &Client::process_send_custom_request_query);
  methods_.emplace("answercustomquery", &Client::process_answer_custom_query_query);
  methods_.emplace("getupdates", &Client::process_get_updates_query);
  methods_.emplace("setwebhook", &Client::process_set_webhook_query);
  methods_.emplace("deletewebhook", &Client::process_set_webhook_query);
  methods_.emplace("getwebhookinfo", &Client::process_get_webhook_info_query);
  methods_.emplace("getfile", &Client::process_get_file_query);
  return true;
}

class Client::JsonFile final : public Jsonable {
 public:
  JsonFile(const td_api::file *file, const Client *client, bool with_path)
      : file_(file), client_(client), with_path_(with_path) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    client_->json_store_file(object, file_, with_path_);
  }

 private:
  const td_api::file *file_;
  const Client *client_;
  bool with_path_;
};

class Client::JsonDatedFile final : public Jsonable {
 public:
  JsonDatedFile(const td_api::datedFile *file, const Client *client) : file_(file), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    client_->json_store_file(object, file_->file_.get());
    object("file_date", file_->date_);
  }

 private:
  const td_api::datedFile *file_;
  const Client *client_;
};

class Client::JsonDatedFiles final : public Jsonable {
 public:
  JsonDatedFiles(const td::vector<object_ptr<td_api::datedFile>> &files, const Client *client)
      : files_(files), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (auto &file : files_) {
      array << JsonDatedFile(file.get(), client_);
    }
  }

 private:
  const td::vector<object_ptr<td_api::datedFile>> &files_;
  const Client *client_;
};

class Client::JsonUser final : public Jsonable {
 public:
  JsonUser(int64 user_id, const Client *client, bool full_bot_info = false)
      : user_id_(user_id), client_(client), full_bot_info_(full_bot_info) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    auto user_info = client_->get_user_info(user_id_);
    object("id", user_id_);
    bool is_bot = user_info != nullptr && user_info->type == UserInfo::Type::Bot;
    object("is_bot", td::JsonBool(is_bot));
    object("first_name", user_info == nullptr ? "" : user_info->first_name);
    if (user_info != nullptr && !user_info->last_name.empty()) {
      object("last_name", user_info->last_name);
    }
    if (user_info != nullptr && !user_info->username.empty()) {
      object("username", user_info->username);
    }
    if (user_info != nullptr && !user_info->language_code.empty()) {
      object("language_code", user_info->language_code);
    }
    if (user_info != nullptr && user_info->is_premium) {
      object("is_premium", td::JsonTrue());
    }
    if (user_info != nullptr && user_info->added_to_attachment_menu) {
      object("added_to_attachment_menu", td::JsonTrue());
    }
    if (is_bot && full_bot_info_) {
      object("can_join_groups", td::JsonBool(user_info->can_join_groups));
      object("can_read_all_group_messages", td::JsonBool(user_info->can_read_all_group_messages));
      object("supports_inline_queries", td::JsonBool(user_info->is_inline_bot));
    }
  }

 private:
  int64 user_id_;
  const Client *client_;
  bool full_bot_info_;
};

class Client::JsonUsers final : public Jsonable {
 public:
  JsonUsers(const td::vector<int64> &user_ids, const Client *client) : user_ids_(user_ids), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (auto &user_id : user_ids_) {
      array << JsonUser(user_id, client_);
    }
  }

 private:
  const td::vector<int64> &user_ids_;
  const Client *client_;
};

class Client::JsonEntity final : public Jsonable {
 public:
  JsonEntity(const td_api::textEntity *entity, const Client *client) : entity_(entity), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("offset", entity_->offset_);
    object("length", entity_->length_);
    switch (entity_->type_->get_id()) {
      case td_api::textEntityTypeMention::ID:
        object("type", "mention");
        break;
      case td_api::textEntityTypeHashtag::ID:
        object("type", "hashtag");
        break;
      case td_api::textEntityTypeCashtag::ID:
        object("type", "cashtag");
        break;
      case td_api::textEntityTypeBotCommand::ID:
        object("type", "bot_command");
        break;
      case td_api::textEntityTypeUrl::ID:
        object("type", "url");
        break;
      case td_api::textEntityTypeEmailAddress::ID:
        object("type", "email");
        break;
      case td_api::textEntityTypePhoneNumber::ID:
        object("type", "phone_number");
        break;
      case td_api::textEntityTypeBankCardNumber::ID:
        object("type", "bank_card_number");
        break;
      case td_api::textEntityTypeBold::ID:
        object("type", "bold");
        break;
      case td_api::textEntityTypeItalic::ID:
        object("type", "italic");
        break;
      case td_api::textEntityTypeUnderline::ID:
        object("type", "underline");
        break;
      case td_api::textEntityTypeStrikethrough::ID:
        object("type", "strikethrough");
        break;
      case td_api::textEntityTypeSpoiler::ID:
        object("type", "spoiler");
        break;
      case td_api::textEntityTypeCode::ID:
        object("type", "code");
        break;
      case td_api::textEntityTypePre::ID:
        object("type", "pre");
        break;
      case td_api::textEntityTypePreCode::ID: {
        auto entity = static_cast<const td_api::textEntityTypePreCode *>(entity_->type_.get());
        object("type", "pre");
        object("language", entity->language_);
        break;
      }
      case td_api::textEntityTypeTextUrl::ID: {
        auto entity = static_cast<const td_api::textEntityTypeTextUrl *>(entity_->type_.get());
        object("type", "text_link");
        object("url", entity->url_);
        break;
      }
      case td_api::textEntityTypeMentionName::ID: {
        auto entity = static_cast<const td_api::textEntityTypeMentionName *>(entity_->type_.get());
        object("type", "text_mention");
        object("user", JsonUser(entity->user_id_, client_));
        break;
      }
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::textEntity *entity_;
  const Client *client_;
};

class Client::JsonVectorEntities final : public Jsonable {
 public:
  JsonVectorEntities(const td::vector<object_ptr<td_api::textEntity>> &entities, const Client *client)
      : entities_(entities), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (auto &entity : entities_) {
      auto entity_type = entity->type_->get_id();
      if (entity_type != td_api::textEntityTypeBankCardNumber::ID &&
          entity_type != td_api::textEntityTypeMediaTimestamp::ID) {
        array << JsonEntity(entity.get(), client_);
      }
    }
  }

 private:
  const td::vector<object_ptr<td_api::textEntity>> &entities_;
  const Client *client_;
};

class Client::JsonLocation final : public Jsonable {
 public:
  explicit JsonLocation(const td_api::location *location, double expires_in = 0.0, int32 live_period = 0,
                        int32 heading = 0, int32 proximity_alert_radius = 0)
      : location_(location)
      , expires_in_(expires_in)
      , live_period_(live_period)
      , heading_(heading)
      , proximity_alert_radius_(proximity_alert_radius) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("latitude", location_->latitude_);
    object("longitude", location_->longitude_);
    if (expires_in_ > 0.0) {
      object("live_period", live_period_);
      if (heading_ > 0) {
        object("heading", heading_);
      }
      if (proximity_alert_radius_ > 0) {
        object("proximity_alert_radius", proximity_alert_radius_);
      }
    }
    if (location_->horizontal_accuracy_ > 0) {
      object("horizontal_accuracy", location_->horizontal_accuracy_);
    }
  }

 private:
  const td_api::location *location_;
  double expires_in_;
  int32 live_period_;
  int32 heading_;
  int32 proximity_alert_radius_;
};

class Client::JsonChatPermissions final : public Jsonable {
 public:
  explicit JsonChatPermissions(const td_api::chatPermissions *chat_permissions) : chat_permissions_(chat_permissions) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    Client::json_store_permissions(object, chat_permissions_);
  }

 private:
  const td_api::chatPermissions *chat_permissions_;
};

class Client::JsonChatPhotoInfo final : public Jsonable {
 public:
  explicit JsonChatPhotoInfo(const td_api::chatPhotoInfo *chat_photo) : chat_photo_(chat_photo) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("small_file_id", chat_photo_->small_->remote_->id_);
    object("small_file_unique_id", chat_photo_->small_->remote_->unique_id_);
    object("big_file_id", chat_photo_->big_->remote_->id_);
    object("big_file_unique_id", chat_photo_->big_->remote_->unique_id_);
  }

 private:
  const td_api::chatPhotoInfo *chat_photo_;
};

class Client::JsonChatLocation final : public Jsonable {
 public:
  explicit JsonChatLocation(const td_api::chatLocation *chat_location) : chat_location_(chat_location) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("location", JsonLocation(chat_location_->location_.get()));
    object("address", chat_location_->address_);
  }

 private:
  const td_api::chatLocation *chat_location_;
};

class Client::JsonChatInviteLink final : public Jsonable {
 public:
  JsonChatInviteLink(const td_api::chatInviteLink *chat_invite_link, const Client *client)
      : chat_invite_link_(chat_invite_link), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("invite_link", chat_invite_link_->invite_link_);
    if (!chat_invite_link_->name_.empty()) {
      object("name", chat_invite_link_->name_);
    }
    object("creator", JsonUser(chat_invite_link_->creator_user_id_, client_));
    if (chat_invite_link_->expiration_date_ != 0) {
      object("expire_date", chat_invite_link_->expiration_date_);
    }
    if (chat_invite_link_->member_limit_ != 0) {
      object("member_limit", chat_invite_link_->member_limit_);
    }
    if (chat_invite_link_->pending_join_request_count_ != 0) {
      object("pending_join_request_count", chat_invite_link_->pending_join_request_count_);
    }
    object("creates_join_request", td::JsonBool(chat_invite_link_->creates_join_request_));
    object("is_primary", td::JsonBool(chat_invite_link_->is_primary_));
    object("is_revoked", td::JsonBool(chat_invite_link_->is_revoked_));
  }

 private:
  const td_api::chatInviteLink *chat_invite_link_;
  const Client *client_;
};

class Client::JsonMessage final : public Jsonable {
 public:
  JsonMessage(const MessageInfo *message, bool need_reply, const td::string &source, const Client *client)
      : message_(message), need_reply_(need_reply), source_(source), client_(client) {
  }
  void store(JsonValueScope *scope) const;

 private:
  const MessageInfo *message_;
  bool need_reply_;
  const td::string &source_;
  const Client *client_;

  void add_caption(td::JsonObjectScope &object, const object_ptr<td_api::formattedText> &caption) const {
    CHECK(caption != nullptr);
    if (!caption->text_.empty()) {
      object("caption", caption->text_);

      if (!caption->entities_.empty()) {
        object("caption_entities", JsonVectorEntities(caption->entities_, client_));
      }
    }
  }
};

class Client::JsonChat final : public Jsonable {
 public:
  JsonChat(int64 chat_id, bool is_full, const Client *client, int64 pinned_message_id = -1)
      : chat_id_(chat_id), is_full_(is_full), client_(client), pinned_message_id_(pinned_message_id) {
  }
  void store(JsonValueScope *scope) const {
    auto chat_info = client_->get_chat(chat_id_);
    CHECK(chat_info != nullptr);
    auto object = scope->enter_object();
    object("id", chat_id_);
    const td_api::chatPhoto *photo = nullptr;
    switch (chat_info->type) {
      case ChatInfo::Type::Private: {
        auto user_info = client_->get_user_info(chat_info->user_id);
        CHECK(user_info != nullptr);
        object("first_name", user_info->first_name);
        if (!user_info->last_name.empty()) {
          object("last_name", user_info->last_name);
        }
        if (!user_info->username.empty()) {
          object("username", user_info->username);
        }
        object("type", "private");
        if (is_full_) {
          if (!user_info->bio.empty()) {
            object("bio", user_info->bio);
          }
          if (user_info->has_private_forwards) {
            object("has_private_forwards", td::JsonTrue());
          }
        }
        photo = user_info->photo.get();
        break;
      }
      case ChatInfo::Type::Group: {
        object("title", chat_info->title);
        object("type", "group");

        const auto *permissions = chat_info->permissions.get();

        auto group_info = client_->get_group_info(chat_info->group_id);
        CHECK(group_info != nullptr);
        if (is_full_) {
          if (!group_info->description.empty()) {
            object("description", group_info->description);
          }
          if (!group_info->invite_link.empty()) {
            object("invite_link", group_info->invite_link);
          }
          object("permissions", JsonChatPermissions(permissions));
        }
        auto everyone_is_administrator = permissions->can_send_messages_ && permissions->can_send_media_messages_ &&
                                         permissions->can_send_polls_ && permissions->can_send_other_messages_ &&
                                         permissions->can_add_web_page_previews_ && permissions->can_change_info_ &&
                                         permissions->can_invite_users_ && permissions->can_pin_messages_;
        object("all_members_are_administrators", td::JsonBool(everyone_is_administrator));
        photo = group_info->photo.get();
        break;
      }
      case ChatInfo::Type::Supergroup: {
        object("title", chat_info->title);

        auto supergroup_info = client_->get_supergroup_info(chat_info->supergroup_id);
        CHECK(supergroup_info != nullptr);
        if (!supergroup_info->username.empty()) {
          object("username", supergroup_info->username);
        }

        if (supergroup_info->is_supergroup) {
          object("type", "supergroup");
        } else {
          object("type", "channel");
        }
        if (is_full_) {
          if (!supergroup_info->description.empty()) {
            object("description", supergroup_info->description);
          }
          if (!supergroup_info->invite_link.empty()) {
            object("invite_link", supergroup_info->invite_link);
          }
          if (supergroup_info->sticker_set_id != 0) {
            auto sticker_set_name = client_->get_sticker_set_name(supergroup_info->sticker_set_id);
            if (!sticker_set_name.empty()) {
              object("sticker_set_name", sticker_set_name);
            } else {
              LOG(ERROR) << "Not found chat sticker set " << supergroup_info->sticker_set_id;
            }
          }
          if (supergroup_info->can_set_sticker_set) {
            object("can_set_sticker_set", td::JsonTrue());
          }
          if (supergroup_info->is_supergroup) {
            object("permissions", JsonChatPermissions(chat_info->permissions.get()));
          }
          if (supergroup_info->is_supergroup && supergroup_info->join_to_send_messages) {
            object("join_to_send_messages", td::JsonTrue());
          }
          if (supergroup_info->is_supergroup && supergroup_info->join_by_request) {
            object("join_by_request", td::JsonTrue());
          }
          if (supergroup_info->slow_mode_delay != 0) {
            object("slow_mode_delay", supergroup_info->slow_mode_delay);
          }
          if (supergroup_info->linked_chat_id != 0) {
            object("linked_chat_id", supergroup_info->linked_chat_id);
          }
          if (supergroup_info->location != nullptr) {
            object("location", JsonChatLocation(supergroup_info->location.get()));
          }
        }
        photo = supergroup_info->photo.get();
        break;
      }
      case ChatInfo::Type::Unknown:
      default:
        UNREACHABLE();
    }
    if (is_full_) {
      if (photo != nullptr) {
        const td_api::file *small_file = nullptr;
        const td_api::file *big_file = nullptr;
        for (auto &size : photo->sizes_) {
          if (size->type_ == "a") {
            small_file = size->photo_.get();
          } else if (size->type_ == "c") {
            big_file = size->photo_.get();
          }
        }
        if (small_file == nullptr || big_file == nullptr) {
          LOG(ERROR) << "Failed to convert chatPhoto to chatPhotoInfo for " << chat_id_ << ": " << to_string(*photo);
        } else {
          if (chat_info->photo_info == nullptr) {
            LOG(ERROR) << "Have chatPhoto without chatPhotoInfo for " << chat_id_;
          } else {
            if (small_file->remote_->unique_id_ != chat_info->photo_info->small_->remote_->unique_id_ ||
                big_file->remote_->unique_id_ != chat_info->photo_info->big_->remote_->unique_id_) {
              LOG(ERROR) << "Have different chatPhoto and chatPhotoInfo for " << chat_id_ << ": " << to_string(*photo)
                         << ' ' << to_string(chat_info->photo_info);
            }
          }
        }
      } else if (chat_info->photo_info != nullptr) {
        LOG(ERROR) << "Have chatPhotoInfo without chatPhoto for " << chat_id_;
      }
      if (chat_info->photo_info != nullptr) {
        object("photo", JsonChatPhotoInfo(chat_info->photo_info.get()));
      }
      if (pinned_message_id_ != 0) {
        CHECK(pinned_message_id_ != -1);
        const MessageInfo *pinned_message = client_->get_message(chat_id_, pinned_message_id_);
        if (pinned_message != nullptr) {
          object("pinned_message", JsonMessage(pinned_message, false, "pin in JsonChat", client_));
        } else {
          LOG(ERROR) << "Pinned unknown, inaccessible or deleted message " << pinned_message_id_;
        }
      }
      if (chat_info->message_auto_delete_time != 0) {
        object("message_auto_delete_time", chat_info->message_auto_delete_time);
      }
      if (chat_info->has_protected_content) {
        object("has_protected_content", td::JsonTrue());
      }
    }
  }

 private:
  int64 chat_id_;
  bool is_full_;
  const Client *client_;
  int64 pinned_message_id_;
};

class Client::JsonMessageSender final : public Jsonable {
 public:
  JsonMessageSender(const td_api::MessageSender *sender_id, const Client *client)
      : sender_id_(sender_id), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    CHECK(sender_id_ != nullptr);
    switch (sender_id_->get_id()) {
      case td_api::messageSenderUser::ID: {
        auto sender_user_id = static_cast<const td_api::messageSenderUser *>(sender_id_)->user_id_;
        JsonUser(sender_user_id, client_).store(scope);
        break;
      }
      case td_api::messageSenderChat::ID: {
        auto sender_chat_id = static_cast<const td_api::messageSenderChat *>(sender_id_)->chat_id_;
        JsonChat(sender_chat_id, false, client_).store(scope);
        break;
      }
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::MessageSender *sender_id_;
  const Client *client_;
};

class Client::JsonMessages final : public Jsonable {
 public:
  explicit JsonMessages(const td::vector<td::string> &messages) : messages_(messages) {
  }
  void store(JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (auto &message : messages_) {
      array << td::JsonRaw(message);
    }
  }

 private:
  const td::vector<td::string> &messages_;
};

class Client::JsonAnimation final : public Jsonable {
 public:
  JsonAnimation(const td_api::animation *animation, bool as_document, const Client *client)
      : animation_(animation), as_document_(as_document), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (!animation_->file_name_.empty()) {
      object("file_name", animation_->file_name_);
    }
    if (!animation_->mime_type_.empty()) {
      object("mime_type", animation_->mime_type_);
    }
    if (!as_document_) {
      object("duration", animation_->duration_);
      object("width", animation_->width_);
      object("height", animation_->height_);
    }
    client_->json_store_thumbnail(object, animation_->thumbnail_.get());
    client_->json_store_file(object, animation_->animation_.get());
  }

 private:
  const td_api::animation *animation_;
  bool as_document_;
  const Client *client_;
};

class Client::JsonAudio final : public Jsonable {
 public:
  JsonAudio(const td_api::audio *audio, const Client *client) : audio_(audio), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("duration", audio_->duration_);
    if (!audio_->file_name_.empty()) {
      object("file_name", audio_->file_name_);
    }
    if (!audio_->mime_type_.empty()) {
      object("mime_type", audio_->mime_type_);
    }
    if (!audio_->title_.empty()) {
      object("title", audio_->title_);
    }
    if (!audio_->performer_.empty()) {
      object("performer", audio_->performer_);
    }
    client_->json_store_thumbnail(object, audio_->album_cover_thumbnail_.get());
    client_->json_store_file(object, audio_->audio_.get());
  }

 private:
  const td_api::audio *audio_;
  const Client *client_;
};

class Client::JsonDocument final : public Jsonable {
 public:
  JsonDocument(const td_api::document *document, const Client *client) : document_(document), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (!document_->file_name_.empty()) {
      object("file_name", document_->file_name_);
    }
    if (!document_->mime_type_.empty()) {
      object("mime_type", document_->mime_type_);
    }
    client_->json_store_thumbnail(object, document_->thumbnail_.get());
    client_->json_store_file(object, document_->document_.get());
  }

 private:
  const td_api::document *document_;
  const Client *client_;
};

class Client::JsonPhotoSize final : public Jsonable {
 public:
  JsonPhotoSize(const td_api::photoSize *photo_size, const Client *client) : photo_size_(photo_size), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    client_->json_store_file(object, photo_size_->photo_.get());
    object("width", photo_size_->width_);
    object("height", photo_size_->height_);
  }

 private:
  const td_api::photoSize *photo_size_;
  const Client *client_;
};

class Client::JsonThumbnail final : public Jsonable {
 public:
  JsonThumbnail(const td_api::thumbnail *thumbnail, const Client *client) : thumbnail_(thumbnail), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    client_->json_store_file(object, thumbnail_->file_.get());
    object("width", thumbnail_->width_);
    object("height", thumbnail_->height_);
  }

 private:
  const td_api::thumbnail *thumbnail_;
  const Client *client_;
};

class Client::JsonPhoto final : public Jsonable {
 public:
  JsonPhoto(const td_api::photo *photo, const Client *client) : photo_(photo), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (auto &photo_size : photo_->sizes_) {
      if (photo_size->type_ != "i" && photo_size->type_ != "t" && !photo_size->photo_->remote_->id_.empty()) {
        array << JsonPhotoSize(photo_size.get(), client_);
      }
    }
  }

 private:
  const td_api::photo *photo_;
  const Client *client_;
};

class Client::JsonChatPhoto final : public Jsonable {
 public:
  JsonChatPhoto(const td_api::chatPhoto *photo, const Client *client) : photo_(photo), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (auto &photo_size : photo_->sizes_) {
      if (photo_size->type_ != "i" && photo_size->type_ != "t" && !photo_size->photo_->remote_->id_.empty()) {
        array << JsonPhotoSize(photo_size.get(), client_);
      }
    }
  }

 private:
  const td_api::chatPhoto *photo_;
  const Client *client_;
};

class Client::JsonMaskPosition final : public Jsonable {
 public:
  explicit JsonMaskPosition(const td_api::maskPosition *mask_position) : mask_position_(mask_position) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("point", Client::MASK_POINTS[Client::mask_point_to_index(mask_position_->point_)]);
    object("x_shift", mask_position_->x_shift_);
    object("y_shift", mask_position_->y_shift_);
    object("scale", mask_position_->scale_);
  }

 private:
  const td_api::maskPosition *mask_position_;
};

class Client::JsonSticker final : public Jsonable {
 public:
  JsonSticker(const td_api::sticker *sticker, const Client *client) : sticker_(sticker), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("width", sticker_->width_);
    object("height", sticker_->height_);
    if (!sticker_->emoji_.empty()) {
      object("emoji", sticker_->emoji_);
    }
    auto set_name = client_->get_sticker_set_name(sticker_->set_id_);
    if (!set_name.empty()) {
      object("set_name", set_name);
    }
    auto type = sticker_->type_->get_id();
    object("is_animated", td::JsonBool(type == td_api::stickerTypeAnimated::ID));
    object("is_video", td::JsonBool(type == td_api::stickerTypeVideo::ID));
    if (type == td_api::stickerTypeMask::ID) {
      const auto &mask_position = static_cast<const td_api::stickerTypeMask *>(sticker_->type_.get())->mask_position_;
      if (mask_position != nullptr) {
        object("mask_position", JsonMaskPosition(mask_position.get()));
      }
    }
    if (sticker_->premium_animation_ != nullptr) {
      object("premium_animation", JsonFile(sticker_->premium_animation_.get(), client_, false));
    }
    client_->json_store_thumbnail(object, sticker_->thumbnail_.get());
    client_->json_store_file(object, sticker_->sticker_.get());
  }

 private:
  const td_api::sticker *sticker_;
  const Client *client_;
};

class Client::JsonStickers final : public Jsonable {
 public:
  JsonStickers(const td::vector<object_ptr<td_api::sticker>> &stickers, const Client *client)
      : stickers_(stickers), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (auto &sticker : stickers_) {
      array << JsonSticker(sticker.get(), client_);
    }
  }

 private:
  const td::vector<object_ptr<td_api::sticker>> &stickers_;
  const Client *client_;
};

class Client::JsonVideo final : public Jsonable {
 public:
  JsonVideo(const td_api::video *video, const Client *client) : video_(video), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("duration", video_->duration_);
    object("width", video_->width_);
    object("height", video_->height_);
    if (!video_->file_name_.empty()) {
      object("file_name", video_->file_name_);
    }
    if (!video_->mime_type_.empty()) {
      object("mime_type", video_->mime_type_);
    }
    client_->json_store_thumbnail(object, video_->thumbnail_.get());
    client_->json_store_file(object, video_->video_.get());
  }

 private:
  const td_api::video *video_;
  const Client *client_;
};

class Client::JsonVideoNote final : public Jsonable {
 public:
  JsonVideoNote(const td_api::videoNote *video_note, const Client *client) : video_note_(video_note), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("duration", video_note_->duration_);
    object("length", video_note_->length_);
    client_->json_store_thumbnail(object, video_note_->thumbnail_.get());
    client_->json_store_file(object, video_note_->video_.get());
  }

 private:
  const td_api::videoNote *video_note_;
  const Client *client_;
};

class Client::JsonVoiceNote final : public Jsonable {
 public:
  JsonVoiceNote(const td_api::voiceNote *voice_note, const Client *client) : voice_note_(voice_note), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("duration", voice_note_->duration_);
    if (!voice_note_->mime_type_.empty()) {
      object("mime_type", voice_note_->mime_type_);
    }
    client_->json_store_file(object, voice_note_->voice_.get());
  }

 private:
  const td_api::voiceNote *voice_note_;
  const Client *client_;
};

class Client::JsonVenue final : public Jsonable {
 public:
  explicit JsonVenue(const td_api::venue *venue) : venue_(venue) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("location", JsonLocation(venue_->location_.get()));
    object("title", venue_->title_);
    object("address", venue_->address_);
    if (venue_->provider_ == "foursquare") {
      if (!venue_->id_.empty()) {
        object("foursquare_id", venue_->id_);
      }
      if (!venue_->type_.empty()) {
        object("foursquare_type", venue_->type_);
      }
    }
    if (venue_->provider_ == "gplaces") {
      if (!venue_->id_.empty()) {
        object("google_place_id", venue_->id_);
      }
      if (!venue_->type_.empty()) {
        object("google_place_type", venue_->type_);
      }
    }
  }

 private:
  const td_api::venue *venue_;
};

class Client::JsonContact final : public Jsonable {
 public:
  explicit JsonContact(const td_api::contact *contact) : contact_(contact) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("phone_number", contact_->phone_number_);
    object("first_name", contact_->first_name_);
    if (!contact_->last_name_.empty()) {
      object("last_name", contact_->last_name_);
    }
    if (!contact_->vcard_.empty()) {
      object("vcard", contact_->vcard_);
    }
    if (contact_->user_id_) {
      object("user_id", contact_->user_id_);
    }
  }

 private:
  const td_api::contact *contact_;
};

class Client::JsonDice final : public Jsonable {
 public:
  JsonDice(const td::string &emoji, int32 value) : emoji_(emoji), value_(value) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("emoji", emoji_);
    object("value", value_);
  }

 private:
  const td::string &emoji_;
  int32 value_;
};

class Client::JsonGame final : public Jsonable {
 public:
  JsonGame(const td_api::game *game, const Client *client) : game_(game), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("title", game_->title_);
    if (!game_->text_->text_.empty()) {
      object("text", game_->text_->text_);
    }
    if (!game_->text_->entities_.empty()) {
      object("text_entities", JsonVectorEntities(game_->text_->entities_, client_));
    }
    object("description", game_->description_);
    CHECK(game_->photo_ != nullptr);
    object("photo", JsonPhoto(game_->photo_.get(), client_));
    if (game_->animation_ != nullptr) {
      object("animation", JsonAnimation(game_->animation_.get(), false, client_));
    }
  }

 private:
  const td_api::game *game_;
  const Client *client_;
};

class Client::JsonInvoice final : public Jsonable {
 public:
  explicit JsonInvoice(const td_api::messageInvoice *invoice) : invoice_(invoice) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("title", invoice_->title_);
    object("description", invoice_->description_->text_);
    object("start_parameter", invoice_->start_parameter_);
    object("currency", invoice_->currency_);
    object("total_amount", invoice_->total_amount_);
    // skip photo
    // skip is_test
    // skip need_shipping_address
    // skip receipt_message_id
  }

 private:
  const td_api::messageInvoice *invoice_;
};

class Client::JsonPollOption final : public Jsonable {
 public:
  explicit JsonPollOption(const td_api::pollOption *option) : option_(option) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("text", option_->text_);
    object("voter_count", option_->voter_count_);
    // ignore is_chosen
  }

 private:
  const td_api::pollOption *option_;
};

class Client::JsonPoll final : public Jsonable {
 public:
  JsonPoll(const td_api::poll *poll, const Client *client) : poll_(poll), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", td::to_string(poll_->id_));
    object("question", poll_->question_);
    object("options", td::json_array(poll_->options_, [](auto &option) { return JsonPollOption(option.get()); }));
    object("total_voter_count", poll_->total_voter_count_);
    if (poll_->open_period_ != 0 && poll_->close_date_ != 0) {
      object("open_period", poll_->open_period_);
      object("close_date", poll_->close_date_);
    }
    object("is_closed", td::JsonBool(poll_->is_closed_));
    object("is_anonymous", td::JsonBool(poll_->is_anonymous_));
    switch (poll_->type_->get_id()) {
      case td_api::pollTypeQuiz::ID: {
        object("type", "quiz");
        object("allows_multiple_answers", td::JsonFalse());
        auto quiz = static_cast<const td_api::pollTypeQuiz *>(poll_->type_.get());
        int32 correct_option_id = quiz->correct_option_id_;
        if (correct_option_id != -1) {
          object("correct_option_id", correct_option_id);
        }
        auto *explanation = quiz->explanation_.get();
        if (!explanation->text_.empty()) {
          object("explanation", explanation->text_);
          object("explanation_entities", JsonVectorEntities(explanation->entities_, client_));
        }
        break;
      }
      case td_api::pollTypeRegular::ID:
        object("type", "regular");
        object("allows_multiple_answers",
               td::JsonBool(static_cast<const td_api::pollTypeRegular *>(poll_->type_.get())->allow_multiple_answers_));
        break;
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::poll *poll_;
  const Client *client_;
};

class Client::JsonPollAnswer final : public Jsonable {
 public:
  JsonPollAnswer(const td_api::updatePollAnswer *poll_answer, const Client *client)
      : poll_answer_(poll_answer), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("poll_id", td::to_string(poll_answer_->poll_id_));
    object("user", JsonUser(poll_answer_->user_id_, client_));
    object("option_ids", td::json_array(poll_answer_->option_ids_, [](int32 option_id) { return option_id; }));
  }

 private:
  const td_api::updatePollAnswer *poll_answer_;
  const Client *client_;
};

class Client::JsonAddress final : public Jsonable {
 public:
  explicit JsonAddress(const td_api::address *address) : address_(address) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("country_code", address_->country_code_);
    object("state", address_->state_);
    object("city", address_->city_);
    object("street_line1", address_->street_line1_);
    object("street_line2", address_->street_line2_);
    object("post_code", address_->postal_code_);
  }

 private:
  const td_api::address *address_;
};

class Client::JsonOrderInfo final : public Jsonable {
 public:
  explicit JsonOrderInfo(const td_api::orderInfo *order_info) : order_info_(order_info) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (!order_info_->name_.empty()) {
      object("name", order_info_->name_);
    }
    if (!order_info_->phone_number_.empty()) {
      object("phone_number", order_info_->phone_number_);
    }
    if (!order_info_->email_address_.empty()) {
      object("email", order_info_->email_address_);
    }
    if (order_info_->shipping_address_ != nullptr) {
      object("shipping_address", JsonAddress(order_info_->shipping_address_.get()));
    }
  }

 private:
  const td_api::orderInfo *order_info_;
};

class Client::JsonSuccessfulPaymentBot final : public Jsonable {
 public:
  explicit JsonSuccessfulPaymentBot(const td_api::messagePaymentSuccessfulBot *successful_payment)
      : successful_payment_(successful_payment) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("currency", successful_payment_->currency_);
    object("total_amount", successful_payment_->total_amount_);
    if (!td::check_utf8(successful_payment_->invoice_payload_)) {
      LOG(WARNING) << "Receive non-UTF-8 invoice payload";
      object("invoice_payload", td::JsonRawString(successful_payment_->invoice_payload_));
    } else {
      object("invoice_payload", successful_payment_->invoice_payload_);
    }
    if (!successful_payment_->shipping_option_id_.empty()) {
      object("shipping_option_id", successful_payment_->shipping_option_id_);
    }
    if (successful_payment_->order_info_ != nullptr) {
      object("order_info", JsonOrderInfo(successful_payment_->order_info_.get()));
    }

    object("telegram_payment_charge_id", successful_payment_->telegram_payment_charge_id_);
    object("provider_payment_charge_id", successful_payment_->provider_payment_charge_id_);
  }

 private:
  const td_api::messagePaymentSuccessfulBot *successful_payment_;
};

class Client::JsonEncryptedPassportElement final : public Jsonable {
 public:
  JsonEncryptedPassportElement(const td_api::encryptedPassportElement *element, const Client *client)
      : element_(element), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    auto id = element_->type_->get_id();
    object("type", Client::get_passport_element_type(id));
    switch (id) {
      case td_api::passportElementTypePhoneNumber::ID:
        object("phone_number", element_->value_);
        break;
      case td_api::passportElementTypeEmailAddress::ID:
        object("email", element_->value_);
        break;
      case td_api::passportElementTypePersonalDetails::ID:
      case td_api::passportElementTypePassport::ID:
      case td_api::passportElementTypeDriverLicense::ID:
      case td_api::passportElementTypeIdentityCard::ID:
      case td_api::passportElementTypeInternalPassport::ID:
      case td_api::passportElementTypeAddress::ID:
        object("data", td::base64_encode(element_->data_));
        break;
    }
    switch (id) {
      case td_api::passportElementTypeUtilityBill::ID:
      case td_api::passportElementTypeBankStatement::ID:
      case td_api::passportElementTypeRentalAgreement::ID:
      case td_api::passportElementTypePassportRegistration::ID:
      case td_api::passportElementTypeTemporaryRegistration::ID:
        object("files", JsonDatedFiles(element_->files_, client_));
        if (!element_->translation_.empty()) {
          object("translation", JsonDatedFiles(element_->translation_, client_));
        }
        break;
    }
    switch (id) {
      case td_api::passportElementTypePassport::ID:
      case td_api::passportElementTypeDriverLicense::ID:
      case td_api::passportElementTypeIdentityCard::ID:
      case td_api::passportElementTypeInternalPassport::ID:
        CHECK(element_->front_side_ != nullptr);
        object("front_side", JsonDatedFile(element_->front_side_.get(), client_));
        if (element_->reverse_side_ != nullptr) {
          CHECK(id == td_api::passportElementTypeIdentityCard::ID ||
                id == td_api::passportElementTypeDriverLicense::ID);
          object("reverse_side", JsonDatedFile(element_->reverse_side_.get(), client_));
        } else {
          CHECK(id == td_api::passportElementTypePassport::ID || id == td_api::passportElementTypeInternalPassport::ID);
        }
        if (element_->selfie_ != nullptr) {
          object("selfie", JsonDatedFile(element_->selfie_.get(), client_));
        }
        if (!element_->translation_.empty()) {
          object("translation", JsonDatedFiles(element_->translation_, client_));
        }
        break;
    }
    object("hash", td::base64_encode(element_->hash_));
  }

 private:
  const td_api::encryptedPassportElement *element_;
  const Client *client_;
};

class Client::JsonEncryptedCredentials final : public Jsonable {
 public:
  explicit JsonEncryptedCredentials(const td_api::encryptedCredentials *credentials) : credentials_(credentials) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("data", td::base64_encode(credentials_->data_));
    object("hash", td::base64_encode(credentials_->hash_));
    object("secret", td::base64_encode(credentials_->secret_));
  }

 private:
  const td_api::encryptedCredentials *credentials_;
};

class Client::JsonPassportData final : public Jsonable {
 public:
  JsonPassportData(const td_api::messagePassportDataReceived *passport_data, const Client *client)
      : passport_data_(passport_data), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("data", td::json_array(passport_data_->elements_, [client = client_](auto &element) {
             return JsonEncryptedPassportElement(element.get(), client);
           }));
    object("credentials", JsonEncryptedCredentials(passport_data_->credentials_.get()));
  }

 private:
  const td_api::messagePassportDataReceived *passport_data_;
  const Client *client_;
};

class Client::JsonWebAppData final : public Jsonable {
 public:
  explicit JsonWebAppData(const td_api::messageWebAppDataReceived *web_app_data) : web_app_data_(web_app_data) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("button_text", web_app_data_->button_text_);
    object("data", web_app_data_->data_);
  }

 private:
  const td_api::messageWebAppDataReceived *web_app_data_;
};

class Client::JsonProximityAlertTriggered final : public Jsonable {
 public:
  JsonProximityAlertTriggered(const td_api::messageProximityAlertTriggered *proximity_alert_triggered,
                              const Client *client)
      : proximity_alert_triggered_(proximity_alert_triggered), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("traveler", JsonMessageSender(proximity_alert_triggered_->traveler_id_.get(), client_));
    object("watcher", JsonMessageSender(proximity_alert_triggered_->watcher_id_.get(), client_));
    object("distance", proximity_alert_triggered_->distance_);
  }

 private:
  const td_api::messageProximityAlertTriggered *proximity_alert_triggered_;
  const Client *client_;
};

class Client::JsonVideoChatScheduled final : public Jsonable {
 public:
  explicit JsonVideoChatScheduled(const td_api::messageVideoChatScheduled *video_chat_scheduled)
      : video_chat_scheduled_(video_chat_scheduled) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("start_date", video_chat_scheduled_->start_date_);
  }

 private:
  const td_api::messageVideoChatScheduled *video_chat_scheduled_;
};

class Client::JsonVideoChatStarted final : public Jsonable {
 public:
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
  }
};

class Client::JsonVideoChatEnded final : public Jsonable {
 public:
  explicit JsonVideoChatEnded(const td_api::messageVideoChatEnded *video_chat_ended)
      : video_chat_ended_(video_chat_ended) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("duration", video_chat_ended_->duration_);
  }

 private:
  const td_api::messageVideoChatEnded *video_chat_ended_;
};

class Client::JsonInviteVideoChatParticipants final : public Jsonable {
 public:
  JsonInviteVideoChatParticipants(const td_api::messageInviteVideoChatParticipants *invite_video_chat_participants,
                                  const Client *client)
      : invite_video_chat_participants_(invite_video_chat_participants), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("users", JsonUsers(invite_video_chat_participants_->user_ids_, client_));
  }

 private:
  const td_api::messageInviteVideoChatParticipants *invite_video_chat_participants_;
  const Client *client_;
};

class Client::JsonChatSetTtl final : public Jsonable {
 public:
  explicit JsonChatSetTtl(const td_api::messageChatSetTtl *chat_set_ttl) : chat_set_ttl_(chat_set_ttl) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("message_auto_delete_time", chat_set_ttl_->ttl_);
  }

 private:
  const td_api::messageChatSetTtl *chat_set_ttl_;
};

class Client::JsonCallbackGame final : public Jsonable {
 public:
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
  }
};

class Client::JsonWebAppInfo final : public Jsonable {
 public:
  explicit JsonWebAppInfo(const td::string &url) : url_(url) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("url", url_);
  }

 private:
  const td::string &url_;
};

class Client::JsonInlineKeyboardButton final : public Jsonable {
 public:
  explicit JsonInlineKeyboardButton(const td_api::inlineKeyboardButton *button) : button_(button) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("text", button_->text_);
    switch (button_->type_->get_id()) {
      case td_api::inlineKeyboardButtonTypeUrl::ID: {
        auto type = static_cast<const td_api::inlineKeyboardButtonTypeUrl *>(button_->type_.get());
        object("url", type->url_);
        break;
      }
      case td_api::inlineKeyboardButtonTypeLoginUrl::ID: {
        auto type = static_cast<const td_api::inlineKeyboardButtonTypeLoginUrl *>(button_->type_.get());
        object("url", type->url_);
        break;
      }
      case td_api::inlineKeyboardButtonTypeCallback::ID:
      case td_api::inlineKeyboardButtonTypeCallbackWithPassword::ID: {
        auto data = get_callback_data(button_->type_);
        if (!td::check_utf8(data)) {
          object("callback_data", "INVALID");
        } else {
          object("callback_data", data);
        }
        break;
      }
      case td_api::inlineKeyboardButtonTypeCallbackGame::ID:
        object("callback_game", JsonCallbackGame());
        break;
      case td_api::inlineKeyboardButtonTypeSwitchInline::ID: {
        auto type = static_cast<const td_api::inlineKeyboardButtonTypeSwitchInline *>(button_->type_.get());
        if (type->in_current_chat_) {
          object("switch_inline_query_current_chat", type->query_);
        } else {
          object("switch_inline_query", type->query_);
        }
        break;
      }
      case td_api::inlineKeyboardButtonTypeBuy::ID:
        object("pay", td::JsonTrue());
        break;
      case td_api::inlineKeyboardButtonTypeUser::ID: {
        auto type = static_cast<const td_api::inlineKeyboardButtonTypeUser *>(button_->type_.get());
        object("url", PSLICE() << "tg://user?id=" << type->user_id_);
        break;
      }
      case td_api::inlineKeyboardButtonTypeWebApp::ID: {
        auto type = static_cast<const td_api::inlineKeyboardButtonTypeWebApp *>(button_->type_.get());
        object("web_app", JsonWebAppInfo(type->url_));
        break;
      }
      default:
        UNREACHABLE();
        break;
    }
  }

 private:
  const td_api::inlineKeyboardButton *button_;
};

class Client::JsonInlineKeyboard final : public Jsonable {
 public:
  explicit JsonInlineKeyboard(const td_api::replyMarkupInlineKeyboard *inline_keyboard)
      : inline_keyboard_(inline_keyboard) {
  }
  void store(JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (auto &row : inline_keyboard_->rows_) {
      array << td::json_array(row, [](auto &button) { return JsonInlineKeyboardButton(button.get()); });
    }
  }

 private:
  const td_api::replyMarkupInlineKeyboard *inline_keyboard_;
};

class Client::JsonReplyMarkup final : public Jsonable {
 public:
  explicit JsonReplyMarkup(const td_api::ReplyMarkup *reply_markup) : reply_markup_(reply_markup) {
  }
  void store(JsonValueScope *scope) const {
    CHECK(reply_markup_->get_id() == td_api::replyMarkupInlineKeyboard::ID);
    auto object = scope->enter_object();
    object("inline_keyboard",
           JsonInlineKeyboard(static_cast<const td_api::replyMarkupInlineKeyboard *>(reply_markup_)));
  }

 private:
  const td_api::ReplyMarkup *reply_markup_;
};

void Client::JsonMessage::store(JsonValueScope *scope) const {
  CHECK(message_ != nullptr);
  auto object = scope->enter_object();
  object("message_id", as_client_message_id(message_->id));
  if (message_->sender_user_id != 0) {
    object("from", JsonUser(message_->sender_user_id, client_));
  }
  if (!message_->author_signature.empty()) {
    object("author_signature", message_->author_signature);
  }
  if (message_->sender_chat_id != 0) {
    object("sender_chat", JsonChat(message_->sender_chat_id, false, client_));
  }
  object("chat", JsonChat(message_->chat_id, false, client_));
  object("date", message_->date);
  if (message_->edit_date > 0) {
    object("edit_date", message_->edit_date);
  }
  if (message_->initial_send_date > 0) {
    if (message_->initial_sender_user_id != 0) {
      object("forward_from", JsonUser(message_->initial_sender_user_id, client_));
    }
    if (message_->initial_sender_chat_id != 0) {
      object("forward_from_chat", JsonChat(message_->initial_sender_chat_id, false, client_));
    }
    if (message_->initial_chat_id != 0) {
      object("forward_from_chat", JsonChat(message_->initial_chat_id, false, client_));
      if (message_->initial_message_id != 0) {
        object("forward_from_message_id", as_client_message_id(message_->initial_message_id));
      }
    }
    if (!message_->initial_author_signature.empty()) {
      object("forward_signature", message_->initial_author_signature);
    }
    if (!message_->initial_sender_name.empty()) {
      object("forward_sender_name", message_->initial_sender_name);
    }
    if (message_->is_automatic_forward) {
      object("is_automatic_forward", td::JsonTrue());
    }
    object("forward_date", message_->initial_send_date);
  }
  if (message_->reply_to_message_id > 0 && need_reply_ && !message_->is_reply_to_message_deleted) {
    const MessageInfo *reply_to_message = client_->get_message(message_->chat_id, message_->reply_to_message_id);
    if (reply_to_message != nullptr) {
      object("reply_to_message", JsonMessage(reply_to_message, false, "reply in " + source_, client_));
    } else {
      LOG(WARNING) << "Replied to unknown or deleted message " << message_->reply_to_message_id << " in chat "
                   << message_->chat_id << " while storing " << source_ << " " << message_->id;
    }
  }
  if (message_->media_album_id != 0) {
    object("media_group_id", td::to_string(message_->media_album_id));
  }
  switch (message_->content->get_id()) {
    case td_api::messageText::ID: {
      auto content = static_cast<const td_api::messageText *>(message_->content.get());
      object("text", content->text_->text_);
      if (!content->text_->entities_.empty()) {
        object("entities", JsonVectorEntities(content->text_->entities_, client_));
      }
      break;
    }
    case td_api::messageAnimation::ID: {
      auto content = static_cast<const td_api::messageAnimation *>(message_->content.get());
      object("animation", JsonAnimation(content->animation_.get(), false, client_));
      object("document", JsonAnimation(content->animation_.get(), true, client_));
      add_caption(object, content->caption_);
      break;
    }
    case td_api::messageAudio::ID: {
      auto content = static_cast<const td_api::messageAudio *>(message_->content.get());
      object("audio", JsonAudio(content->audio_.get(), client_));
      add_caption(object, content->caption_);
      break;
    }
    case td_api::messageDocument::ID: {
      auto content = static_cast<const td_api::messageDocument *>(message_->content.get());
      object("document", JsonDocument(content->document_.get(), client_));
      add_caption(object, content->caption_);
      break;
    }
    case td_api::messagePhoto::ID: {
      auto content = static_cast<const td_api::messagePhoto *>(message_->content.get());
      CHECK(content->photo_ != nullptr);
      object("photo", JsonPhoto(content->photo_.get(), client_));
      add_caption(object, content->caption_);
      break;
    }
    case td_api::messageSticker::ID: {
      auto content = static_cast<const td_api::messageSticker *>(message_->content.get());
      object("sticker", JsonSticker(content->sticker_.get(), client_));
      break;
    }
    case td_api::messageVideo::ID: {
      auto content = static_cast<const td_api::messageVideo *>(message_->content.get());
      object("video", JsonVideo(content->video_.get(), client_));
      add_caption(object, content->caption_);
      break;
    }
    case td_api::messageVideoNote::ID: {
      auto content = static_cast<const td_api::messageVideoNote *>(message_->content.get());
      object("video_note", JsonVideoNote(content->video_note_.get(), client_));
      break;
    }
    case td_api::messageVoiceNote::ID: {
      auto content = static_cast<const td_api::messageVoiceNote *>(message_->content.get());
      object("voice", JsonVoiceNote(content->voice_note_.get(), client_));
      add_caption(object, content->caption_);
      break;
    }
    case td_api::messageContact::ID: {
      auto content = static_cast<const td_api::messageContact *>(message_->content.get());
      object("contact", JsonContact(content->contact_.get()));
      break;
    }
    case td_api::messageDice::ID: {
      auto content = static_cast<const td_api::messageDice *>(message_->content.get());
      object("dice", JsonDice(content->emoji_, content->value_));
      break;
    }
    case td_api::messageGame::ID: {
      auto content = static_cast<const td_api::messageGame *>(message_->content.get());
      object("game", JsonGame(content->game_.get(), client_));
      break;
    }
    case td_api::messageInvoice::ID: {
      auto content = static_cast<const td_api::messageInvoice *>(message_->content.get());
      object("invoice", JsonInvoice(content));
      break;
    }
    case td_api::messageLocation::ID: {
      auto content = static_cast<const td_api::messageLocation *>(message_->content.get());
      object("location", JsonLocation(content->location_.get(), content->expires_in_, content->live_period_,
                                      content->heading_, content->proximity_alert_radius_));
      break;
    }
    case td_api::messageVenue::ID: {
      auto content = static_cast<const td_api::messageVenue *>(message_->content.get());
      object("location", JsonLocation(content->venue_->location_.get()));
      object("venue", JsonVenue(content->venue_.get()));
      break;
    }
    case td_api::messagePoll::ID: {
      auto content = static_cast<const td_api::messagePoll *>(message_->content.get());
      object("poll", JsonPoll(content->poll_.get(), client_));
      break;
    }
    case td_api::messageChatAddMembers::ID: {
      auto content = static_cast<const td_api::messageChatAddMembers *>(message_->content.get());
      int64 user_id = client_->choose_added_member_id(content);
      if (user_id > 0) {
        object("new_chat_participant", JsonUser(user_id, client_));
        object("new_chat_member", JsonUser(user_id, client_));
        object("new_chat_members", JsonUsers(content->member_user_ids_, client_));
      } else {
        LOG(ERROR) << "Can't choose added member for new_chat_member field";
      }
      break;
    }
    case td_api::messageChatJoinByLink::ID: {
      if (message_->sender_user_id > 0) {
        object("new_chat_participant", JsonUser(message_->sender_user_id, client_));
        object("new_chat_member", JsonUser(message_->sender_user_id, client_));
        object("new_chat_members", JsonUsers({message_->sender_user_id}, client_));
      }
      break;
    }
    case td_api::messageChatJoinByRequest::ID: {
      if (message_->sender_user_id > 0) {
        object("new_chat_participant", JsonUser(message_->sender_user_id, client_));
        object("new_chat_member", JsonUser(message_->sender_user_id, client_));
        object("new_chat_members", JsonUsers({message_->sender_user_id}, client_));
      }
      break;
    }
    case td_api::messageChatDeleteMember::ID: {
      auto content = static_cast<const td_api::messageChatDeleteMember *>(message_->content.get());
      int64 user_id = content->user_id_;
      object("left_chat_participant", JsonUser(user_id, client_));
      object("left_chat_member", JsonUser(user_id, client_));
      break;
    }
    case td_api::messageChatChangeTitle::ID: {
      auto content = static_cast<const td_api::messageChatChangeTitle *>(message_->content.get());
      object("new_chat_title", content->title_);
      break;
    }
    case td_api::messageChatChangePhoto::ID: {
      auto content = static_cast<const td_api::messageChatChangePhoto *>(message_->content.get());
      CHECK(content->photo_ != nullptr);
      object("new_chat_photo", JsonChatPhoto(content->photo_.get(), client_));
      break;
    }
    case td_api::messageChatDeletePhoto::ID:
      object("delete_chat_photo", td::JsonTrue());
      break;
    case td_api::messageBasicGroupChatCreate::ID:
      object("group_chat_created", td::JsonTrue());
      break;
    case td_api::messageSupergroupChatCreate::ID: {
      auto chat = client_->get_chat(message_->chat_id);
      if (chat->type != ChatInfo::Type::Supergroup) {
        LOG(ERROR) << "Receive messageSupergroupChatCreate in the non-supergroup chat " << message_->chat_id;
        break;
      }
      auto supergroup_info = client_->get_supergroup_info(chat->supergroup_id);
      CHECK(supergroup_info != nullptr);
      if (supergroup_info->is_supergroup) {
        object("supergroup_chat_created", td::JsonTrue());
      } else {
        object("channel_chat_created", td::JsonTrue());
      }
      break;
    }
    case td_api::messageChatUpgradeTo::ID: {
      auto content = static_cast<const td_api::messageChatUpgradeTo *>(message_->content.get());
      auto chat_id = get_supergroup_chat_id(content->supergroup_id_);
      object("migrate_to_chat_id", td::JsonLong(chat_id));
      break;
    }
    case td_api::messageChatUpgradeFrom::ID: {
      auto content = static_cast<const td_api::messageChatUpgradeFrom *>(message_->content.get());
      auto chat_id = get_basic_group_chat_id(content->basic_group_id_);
      object("migrate_from_chat_id", td::JsonLong(chat_id));
      break;
    }
    case td_api::messagePinMessage::ID: {
      auto content = static_cast<const td_api::messagePinMessage *>(message_->content.get());
      auto message_id = content->message_id_;
      if (message_id > 0) {
        const MessageInfo *pinned_message = client_->get_message(message_->chat_id, message_id);
        if (pinned_message != nullptr) {
          object("pinned_message", JsonMessage(pinned_message, false, "pin in " + source_, client_));
        } else {
          LOG_IF(ERROR, need_reply_) << "Pinned unknown, inaccessible or deleted message " << message_id;
        }
      }
      break;
    }
    case td_api::messageGameScore::ID:
      break;
    case td_api::messagePaymentSuccessful::ID:
      break;
    case td_api::messagePaymentSuccessfulBot::ID: {
      auto content = static_cast<const td_api::messagePaymentSuccessfulBot *>(message_->content.get());
      object("successful_payment", JsonSuccessfulPaymentBot(content));
      break;
    }
    case td_api::messageCall::ID:
      break;
    case td_api::messageScreenshotTaken::ID:
      break;
    case td_api::messageChatSetTtl::ID: {
      auto content = static_cast<const td_api::messageChatSetTtl *>(message_->content.get());
      object("message_auto_delete_timer_changed", JsonChatSetTtl(content));
      break;
    }
    case td_api::messageUnsupported::ID:
      break;
    case td_api::messageContactRegistered::ID:
      break;
    case td_api::messageExpiredPhoto::ID:
      break;
    case td_api::messageExpiredVideo::ID:
      break;
    case td_api::messageCustomServiceAction::ID:
      break;
    case td_api::messageChatSetTheme::ID:
      break;
    case td_api::messageAnimatedEmoji::ID:
      UNREACHABLE();
      break;
    case td_api::messageWebsiteConnected::ID: {
      auto chat = client_->get_chat(message_->chat_id);
      if (chat->type != ChatInfo::Type::Private) {
        break;
      }

      auto content = static_cast<const td_api::messageWebsiteConnected *>(message_->content.get());
      if (!content->domain_name_.empty()) {
        object("connected_website", content->domain_name_);
      }
      break;
    }
    case td_api::messagePassportDataSent::ID:
      break;
    case td_api::messagePassportDataReceived::ID: {
      auto content = static_cast<const td_api::messagePassportDataReceived *>(message_->content.get());
      object("passport_data", JsonPassportData(content, client_));
      break;
    }
    case td_api::messageProximityAlertTriggered::ID: {
      auto content = static_cast<const td_api::messageProximityAlertTriggered *>(message_->content.get());
      object("proximity_alert_triggered", JsonProximityAlertTriggered(content, client_));
      break;
    }
    case td_api::messageVideoChatScheduled::ID: {
      auto content = static_cast<const td_api::messageVideoChatScheduled *>(message_->content.get());
      object("video_chat_scheduled", JsonVideoChatScheduled(content));
      object("voice_chat_scheduled", JsonVideoChatScheduled(content));
      break;
    }
    case td_api::messageVideoChatStarted::ID:
      object("video_chat_started", JsonVideoChatStarted());
      object("voice_chat_started", JsonVideoChatStarted());
      break;
    case td_api::messageVideoChatEnded::ID: {
      auto content = static_cast<const td_api::messageVideoChatEnded *>(message_->content.get());
      object("video_chat_ended", JsonVideoChatEnded(content));
      object("voice_chat_ended", JsonVideoChatEnded(content));
      break;
    }
    case td_api::messageInviteVideoChatParticipants::ID: {
      auto content = static_cast<const td_api::messageInviteVideoChatParticipants *>(message_->content.get());
      object("video_chat_participants_invited", JsonInviteVideoChatParticipants(content, client_));
      object("voice_chat_participants_invited", JsonInviteVideoChatParticipants(content, client_));
      break;
    }
    case td_api::messageWebAppDataSent::ID:
      break;
    case td_api::messageWebAppDataReceived::ID: {
      auto content = static_cast<const td_api::messageWebAppDataReceived *>(message_->content.get());
      object("web_app_data", JsonWebAppData(content));
      break;
    }
    default:
      UNREACHABLE();
  }
  if (message_->reply_markup != nullptr) {
    object("reply_markup", JsonReplyMarkup(message_->reply_markup.get()));
  }
  if (message_->via_bot_user_id > 0) {
    object("via_bot", JsonUser(message_->via_bot_user_id, client_));
  }
  if (!message_->can_be_saved) {
    object("has_protected_content", td::JsonTrue());
  }
}

class Client::JsonDeletedMessage final : public Jsonable {
 public:
  JsonDeletedMessage(int64 chat_id, int64 message_id, const Client *client)
      : chat_id_(chat_id), message_id_(message_id), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("message_id", as_client_message_id(message_id_));
    object("chat", JsonChat(chat_id_, false, client_));
    object("date", 0);
  }

 private:
  int64 chat_id_;
  int64 message_id_;
  const Client *client_;
};

class Client::JsonMessageId final : public Jsonable {
 public:
  explicit JsonMessageId(int64 message_id) : message_id_(message_id) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("message_id", as_client_message_id(message_id_));
  }

 private:
  int64 message_id_;
};

class Client::JsonInlineQuery final : public Jsonable {
 public:
  JsonInlineQuery(int64 inline_query_id, int64 sender_user_id, const td_api::location *user_location,
                  const td_api::ChatType *chat_type, const td::string &query, const td::string &offset,
                  const Client *client)
      : inline_query_id_(inline_query_id)
      , sender_user_id_(sender_user_id)
      , user_location_(user_location)
      , chat_type_(chat_type)
      , query_(query)
      , offset_(offset)
      , client_(client) {
  }

  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", td::to_string(inline_query_id_));
    object("from", JsonUser(sender_user_id_, client_));
    if (user_location_ != nullptr) {
      object("location", JsonLocation(user_location_));
    }
    if (chat_type_ != nullptr) {
      auto chat_type = [&] {
        switch (chat_type_->get_id()) {
          case td_api::chatTypePrivate::ID: {
            auto type = static_cast<const td_api::chatTypePrivate *>(chat_type_);
            if (type->user_id_ == sender_user_id_) {
              return "sender";
            }
            return "private";
          }
          case td_api::chatTypeBasicGroup::ID:
            return "group";
          case td_api::chatTypeSupergroup::ID: {
            auto type = static_cast<const td_api::chatTypeSupergroup *>(chat_type_);
            if (type->is_channel_) {
              return "channel";
            } else {
              return "supergroup";
            }
          }
          case td_api::chatTypeSecret::ID:
            return "";
          default:
            UNREACHABLE();
            return "";
        }
      }();
      if (chat_type[0] != '\0') {
        object("chat_type", chat_type);
      }
    }
    object("query", query_);
    object("offset", offset_);
  }

 private:
  int64 inline_query_id_;
  int64 sender_user_id_;
  const td_api::location *user_location_;
  const td_api::ChatType *chat_type_;
  const td::string &query_;
  const td::string &offset_;
  const Client *client_;
};

class Client::JsonChosenInlineResult final : public Jsonable {
 public:
  JsonChosenInlineResult(int64 sender_user_id, const td_api::location *user_location, const td::string &query,
                         const td::string &result_id, const td::string &inline_message_id, const Client *client)
      : sender_user_id_(sender_user_id)
      , user_location_(user_location)
      , query_(query)
      , result_id_(result_id)
      , inline_message_id_(inline_message_id)
      , client_(client) {
  }

  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("from", JsonUser(sender_user_id_, client_));
    if (user_location_ != nullptr) {
      object("location", JsonLocation(user_location_));
    }
    if (!inline_message_id_.empty()) {
      object("inline_message_id", inline_message_id_);
    }
    object("query", query_);
    object("result_id", result_id_);
  }

 private:
  int64 sender_user_id_;
  const td_api::location *user_location_;
  const td::string &query_;
  const td::string &result_id_;
  const td::string &inline_message_id_;
  const Client *client_;
};

class Client::JsonCallbackQuery final : public Jsonable {
 public:
  JsonCallbackQuery(int64 callback_query_id, int64 sender_user_id, int64 chat_id, int64 message_id,
                    const MessageInfo *message_info, int64 chat_instance, td_api::CallbackQueryPayload *payload,
                    const Client *client)
      : callback_query_id_(callback_query_id)
      , sender_user_id_(sender_user_id)
      , chat_id_(chat_id)
      , message_id_(message_id)
      , message_info_(message_info)
      , chat_instance_(chat_instance)
      , payload_(payload)
      , client_(client) {
  }

  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", td::to_string(callback_query_id_));
    object("from", JsonUser(sender_user_id_, client_));
    if (message_info_ != nullptr) {
      object("message", JsonMessage(message_info_, true, "callback query", client_));
    } else {
      object("message", JsonDeletedMessage(chat_id_, message_id_, client_));
    }
    object("chat_instance", td::to_string(chat_instance_));
    client_->json_store_callback_query_payload(object, payload_);
  }

 private:
  int64 callback_query_id_;
  int64 sender_user_id_;
  int64 chat_id_;
  int64 message_id_;
  const MessageInfo *message_info_;
  int64 chat_instance_;
  td_api::CallbackQueryPayload *payload_;
  const Client *client_;
};

class Client::JsonInlineCallbackQuery final : public Jsonable {
 public:
  JsonInlineCallbackQuery(int64 callback_query_id, int64 sender_user_id, const td::string &inline_message_id,
                          int64 chat_instance, td_api::CallbackQueryPayload *payload, const Client *client)
      : callback_query_id_(callback_query_id)
      , sender_user_id_(sender_user_id)
      , inline_message_id_(inline_message_id)
      , chat_instance_(chat_instance)
      , payload_(payload)
      , client_(client) {
  }

  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", td::to_string(callback_query_id_));
    object("from", JsonUser(sender_user_id_, client_));
    object("inline_message_id", inline_message_id_);
    object("chat_instance", td::to_string(chat_instance_));
    client_->json_store_callback_query_payload(object, payload_);
  }

 private:
  int64 callback_query_id_;
  int64 sender_user_id_;
  const td::string &inline_message_id_;
  int64 chat_instance_;
  td_api::CallbackQueryPayload *payload_;
  const Client *client_;
};

class Client::JsonShippingQuery final : public Jsonable {
 public:
  JsonShippingQuery(const td_api::updateNewShippingQuery *query, const Client *client)
      : query_(query), client_(client) {
  }

  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", td::to_string(query_->id_));
    object("from", JsonUser(query_->sender_user_id_, client_));
    if (!td::check_utf8(query_->invoice_payload_)) {
      LOG(WARNING) << "Receive non-UTF-8 invoice payload";
      object("invoice_payload", td::JsonRawString(query_->invoice_payload_));
    } else {
      object("invoice_payload", query_->invoice_payload_);
    }
    object("shipping_address", JsonAddress(query_->shipping_address_.get()));
  }

 private:
  const td_api::updateNewShippingQuery *query_;
  const Client *client_;
};

class Client::JsonPreCheckoutQuery final : public Jsonable {
 public:
  JsonPreCheckoutQuery(const td_api::updateNewPreCheckoutQuery *query, const Client *client)
      : query_(query), client_(client) {
  }

  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", td::to_string(query_->id_));
    object("from", JsonUser(query_->sender_user_id_, client_));
    object("currency", query_->currency_);
    object("total_amount", query_->total_amount_);
    if (!td::check_utf8(query_->invoice_payload_)) {
      LOG(WARNING) << "Receive non-UTF-8 invoice payload";
      object("invoice_payload", td::JsonRawString(query_->invoice_payload_));
    } else {
      object("invoice_payload", query_->invoice_payload_);
    }
    if (!query_->shipping_option_id_.empty()) {
      object("shipping_option_id", query_->shipping_option_id_);
    }
    if (query_->order_info_ != nullptr) {
      object("order_info", JsonOrderInfo(query_->order_info_.get()));
    }
  }

 private:
  const td_api::updateNewPreCheckoutQuery *query_;
  const Client *client_;
};

class Client::JsonCustomJson final : public Jsonable {
 public:
  explicit JsonCustomJson(const td::string &json) : json_(json) {
  }

  void store(JsonValueScope *scope) const {
    *scope << td::JsonRaw(json_);
  }

 private:
  const td::string &json_;
};

class Client::JsonBotCommand final : public Jsonable {
 public:
  explicit JsonBotCommand(const td_api::botCommand *command) : command_(command) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("command", command_->command_);
    object("description", command_->description_);
  }

 private:
  const td_api::botCommand *command_;
};

class Client::JsonBotMenuButton final : public Jsonable {
 public:
  explicit JsonBotMenuButton(const td_api::botMenuButton *menu_button) : menu_button_(menu_button) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (menu_button_->text_.empty()) {
      object("type", menu_button_->url_.empty() ? "commands" : "default");
    } else {
      object("type", "web_app");
      object("text", menu_button_->text_);
      object("web_app", JsonWebAppInfo(menu_button_->url_));
    }
  }

 private:
  const td_api::botMenuButton *menu_button_;
};

class Client::JsonChatAdministratorRights final : public Jsonable {
 public:
  JsonChatAdministratorRights(const td_api::chatAdministratorRights *rights, Client::ChatType chat_type)
      : rights_(rights), chat_type_(chat_type) {
  }

  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    td_api::chatAdministratorRights empty_rights;
    Client::json_store_administrator_rights(object, rights_ == nullptr ? &empty_rights : rights_, chat_type_);
  }

 private:
  const td_api::chatAdministratorRights *rights_;
  Client::ChatType chat_type_;
};

class Client::JsonChatPhotos final : public Jsonable {
 public:
  JsonChatPhotos(const td_api::chatPhotos *photos, const Client *client) : photos_(photos), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("total_count", photos_->total_count_);
    object("photos", td::json_array(photos_->photos_,
                                    [client = client_](auto &photo) { return JsonChatPhoto(photo.get(), client); }));
  }

 private:
  const td_api::chatPhotos *photos_;
  const Client *client_;
};

class Client::JsonChatMember final : public Jsonable {
 public:
  JsonChatMember(const td_api::chatMember *member, Client::ChatType chat_type, const Client *client)
      : member_(member), chat_type_(chat_type), client_(client) {
  }

  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    CHECK(member_->member_id_ != nullptr);
    switch (member_->member_id_->get_id()) {
      case td_api::messageSenderUser::ID: {
        auto user_id = static_cast<const td_api::messageSenderUser *>(member_->member_id_.get())->user_id_;
        object("user", JsonUser(user_id, client_));
        break;
      }
      case td_api::messageSenderChat::ID:
        break;
      default:
        UNREACHABLE();
    }
    object("status", Client::get_chat_member_status(member_->status_));
    switch (member_->status_->get_id()) {
      case td_api::chatMemberStatusCreator::ID: {
        auto creator = static_cast<const td_api::chatMemberStatusCreator *>(member_->status_.get());
        if (!creator->custom_title_.empty()) {
          object("custom_title", creator->custom_title_);
        }
        object("is_anonymous", td::JsonBool(creator->is_anonymous_));
        // object("is_member", creator->is_member_); only creator itself knows that he is a left creator
        break;
      }
      case td_api::chatMemberStatusAdministrator::ID: {
        auto administrator = static_cast<const td_api::chatMemberStatusAdministrator *>(member_->status_.get());
        object("can_be_edited", td::JsonBool(administrator->can_be_edited_));
        Client::json_store_administrator_rights(object, administrator->rights_.get(), chat_type_);
        object("can_manage_voice_chats", td::JsonBool(administrator->rights_->can_manage_video_chats_));
        if (!administrator->custom_title_.empty()) {
          object("custom_title", administrator->custom_title_);
        }
        break;
      }
      case td_api::chatMemberStatusMember::ID:
        break;
      case td_api::chatMemberStatusRestricted::ID:
        if (chat_type_ == Client::ChatType::Supergroup) {
          auto restricted = static_cast<const td_api::chatMemberStatusRestricted *>(member_->status_.get());
          object("until_date", restricted->restricted_until_date_);
          Client::json_store_permissions(object, restricted->permissions_.get());
          object("is_member", td::JsonBool(restricted->is_member_));
        }
        break;
      case td_api::chatMemberStatusLeft::ID:
        break;
      case td_api::chatMemberStatusBanned::ID: {
        auto banned = static_cast<const td_api::chatMemberStatusBanned *>(member_->status_.get());
        object("until_date", banned->banned_until_date_);
        break;
      }
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::chatMember *member_;
  Client::ChatType chat_type_;
  const Client *client_;
};

class Client::JsonChatMembers final : public Jsonable {
 public:
  JsonChatMembers(const td::vector<object_ptr<td_api::chatMember>> &members, Client::ChatType chat_type,
                  bool administrators_only, const Client *client)
      : members_(members), chat_type_(chat_type), administrators_only_(administrators_only), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (auto &member : members_) {
      CHECK(member != nullptr);
      CHECK(member->member_id_ != nullptr);
      if (member->member_id_->get_id() != td_api::messageSenderUser::ID) {
        continue;
      }
      auto user_id = static_cast<const td_api::messageSenderUser *>(member->member_id_.get())->user_id_;
      auto user_info = client_->get_user_info(user_id);
      bool is_member_bot = user_info != nullptr && user_info->type == UserInfo::Type::Bot;
      if (is_member_bot && user_id != client_->my_id_) {
        continue;
      }
      if (administrators_only_) {
        auto status = Client::get_chat_member_status(member->status_);
        if (status != "creator" && status != "administrator") {
          continue;
        }
      }
      array << JsonChatMember(member.get(), chat_type_, client_);
    }
  }

 private:
  const td::vector<object_ptr<td_api::chatMember>> &members_;
  Client::ChatType chat_type_;
  bool administrators_only_;
  const Client *client_;
};

class Client::JsonChatMemberUpdated final : public Jsonable {
 public:
  JsonChatMemberUpdated(const td_api::updateChatMember *update, const Client *client)
      : update_(update), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("chat", JsonChat(update_->chat_id_, false, client_));
    object("from", JsonUser(update_->actor_user_id_, client_));
    object("date", update_->date_);
    auto chat_type = client_->get_chat_type(update_->chat_id_);
    object("old_chat_member", JsonChatMember(update_->old_chat_member_.get(), chat_type, client_));
    object("new_chat_member", JsonChatMember(update_->new_chat_member_.get(), chat_type, client_));
    if (update_->invite_link_ != nullptr) {
      object("invite_link", JsonChatInviteLink(update_->invite_link_.get(), client_));
    }
  }

 private:
  const td_api::updateChatMember *update_;
  const Client *client_;
};

class Client::JsonChatJoinRequest final : public Jsonable {
 public:
  JsonChatJoinRequest(const td_api::updateNewChatJoinRequest *update, const Client *client)
      : update_(update), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("chat", JsonChat(update_->chat_id_, false, client_));
    object("from", JsonUser(update_->request_->user_id_, client_));
    object("date", update_->request_->date_);
    if (!update_->request_->bio_.empty()) {
      object("bio", update_->request_->bio_);
    }
    if (update_->invite_link_ != nullptr) {
      object("invite_link", JsonChatInviteLink(update_->invite_link_.get(), client_));
    }
  }

 private:
  const td_api::updateNewChatJoinRequest *update_;
  const Client *client_;
};

class Client::JsonGameHighScore final : public Jsonable {
 public:
  JsonGameHighScore(const td_api::gameHighScore *score, const Client *client) : score_(score), client_(client) {
  }

  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("position", score_->position_);
    object("user", JsonUser(score_->user_id_, client_));
    object("score", score_->score_);
  }

 private:
  const td_api::gameHighScore *score_;
  const Client *client_;
};

class Client::JsonUpdateTypes final : public Jsonable {
 public:
  explicit JsonUpdateTypes(td::uint32 update_types) : update_types_(update_types) {
  }
  void store(JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (int32 i = 0; i < static_cast<int32>(UpdateType::Size); i++) {
      if (((update_types_ >> i) & 1) != 0) {
        auto update_type = static_cast<UpdateType>(i);
        if (update_type != UpdateType::CustomEvent && update_type != UpdateType::CustomQuery) {
          array << get_update_type_name(update_type);
        }
      }
    }
  }

 private:
  td::uint32 update_types_;
};

class Client::JsonWebhookInfo final : public Jsonable {
 public:
  explicit JsonWebhookInfo(const Client *client) : client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    td::CSlice url = client_->webhook_url_;
    if (td::check_utf8(url)) {
      object("url", url);
    } else {
      object("url", td::JsonRawString(url));
    }
    object("has_custom_certificate", td::JsonBool(client_->has_webhook_certificate_));
    object("pending_update_count", td::narrow_cast<int32>(client_->get_pending_update_count()));
    if (client_->last_webhook_error_date_ > 0) {
      object("last_error_date", client_->last_webhook_error_date_);
      td::CSlice error_message = client_->last_webhook_error_.message();
      if (td::check_utf8(error_message)) {
        object("last_error_message", error_message);
      } else {
        object("last_error_message", td::JsonRawString(error_message));
      }
    }
    if (client_->webhook_max_connections_ > 0) {
      object("max_connections", client_->webhook_max_connections_);
    }
    if (!url.empty()) {
      object("ip_address", client_->webhook_ip_address_.empty() ? "<unknown>" : client_->webhook_ip_address_);
    }
    if (client_->allowed_update_types_ != DEFAULT_ALLOWED_UPDATE_TYPES) {
      object("allowed_updates", JsonUpdateTypes(client_->allowed_update_types_));
    }
    if (client_->last_synchronization_error_date_ > 0) {
      object("last_synchronization_error_date", client_->last_synchronization_error_date_);
    }
  }

 private:
  const Client *client_;
};

class Client::JsonStickerSet final : public Jsonable {
 public:
  JsonStickerSet(const td_api::stickerSet *sticker_set, const Client *client)
      : sticker_set_(sticker_set), client_(client) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (sticker_set_->id_ == Client::GREAT_MINDS_SET_ID) {
      object("name", GREAT_MINDS_SET_NAME);
    } else {
      object("name", sticker_set_->name_);
    }
    object("title", sticker_set_->title_);
    if (sticker_set_->thumbnail_ != nullptr) {
      client_->json_store_thumbnail(object, sticker_set_->thumbnail_.get());
    }
    auto type = sticker_set_->sticker_type_->get_id();
    object("is_animated", td::JsonBool(type == td_api::stickerTypeAnimated::ID));
    object("is_video", td::JsonBool(type == td_api::stickerTypeVideo::ID));
    object("contains_masks", td::JsonBool(type == td_api::stickerTypeMask::ID));
    object("stickers", JsonStickers(sticker_set_->stickers_, client_));
  }

 private:
  const td_api::stickerSet *sticker_set_;
  const Client *client_;
};

class Client::JsonSentWebAppMessage final : public Jsonable {
 public:
  explicit JsonSentWebAppMessage(const td_api::sentWebAppMessage *message) : message_(message) {
  }
  void store(JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (!message_->inline_message_id_.empty()) {
      object("inline_message_id", message_->inline_message_id_);
    }
  }

 private:
  const td_api::sentWebAppMessage *message_;
};

class Client::TdOnOkCallback final : public TdQueryCallback {
 public:
  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (error->code_ != 401 && error->code_ != 406 && error->code_ != 500) {
        LOG(ERROR) << "Query has failed: " << td::oneline(to_string(error));
      }
    }
  }
};

class Client::TdOnAuthorizationCallback final : public TdQueryCallback {
 public:
  explicit TdOnAuthorizationCallback(Client *client) : client_(client) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    bool was_ready = client_->authorization_state_->get_id() != td_api::authorizationStateWaitPhoneNumber::ID;
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (error->code_ == 429 || error->code_ >= 500 || (error->code_ != 401 && was_ready)) {
        // try again
        return client_->on_update_authorization_state();
      }

      LOG(WARNING) << "Logging out due to " << td::oneline(to_string(error));
      client_->log_out(error->message_ == "API_ID_INVALID");
    } else if (was_ready) {
      client_->on_update_authorization_state();
    }
  }

 private:
  Client *client_;
};

class Client::TdOnInitCallback final : public TdQueryCallback {
 public:
  explicit TdOnInitCallback(Client *client) : client_(client) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      LOG(WARNING) << "Failed to initialize due to " << td::oneline(to_string(result));
      client_->close();
    }
  }

 private:
  Client *client_;
};

class Client::TdOnGetUserProfilePhotosCallback final : public TdQueryCallback {
 public:
  TdOnGetUserProfilePhotosCallback(const Client *client, PromisedQueryPtr query)
      : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::chatPhotos::ID);
    auto profile_photos = move_object_as<td_api::chatPhotos>(result);
    answer_query(JsonChatPhotos(profile_photos.get(), client_), std::move(query_));
  }

 private:
  const Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnSendMessageCallback final : public TdQueryCallback {
 public:
  TdOnSendMessageCallback(Client *client, PromisedQueryPtr query) : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::message::ID);
    auto query_id = client_->get_send_message_query_id(std::move(query_), false);
    client_->on_sent_message(move_object_as<td_api::message>(result), query_id);
  }

 private:
  Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnSendMessageAlbumCallback final : public TdQueryCallback {
 public:
  TdOnSendMessageAlbumCallback(Client *client, PromisedQueryPtr query) : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::messages::ID);
    auto messages = move_object_as<td_api::messages>(result);
    auto query_id = client_->get_send_message_query_id(std::move(query_), true);
    for (auto &message : messages->messages_) {
      client_->on_sent_message(std::move(message), query_id);
    }
  }

 private:
  Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnDeleteFailedToSendMessageCallback final : public TdQueryCallback {
 public:
  TdOnDeleteFailedToSendMessageCallback(Client *client, int64 chat_id, int64 message_id)
      : client_(client)
      , chat_id_(chat_id)
      , message_id_(message_id)
      , old_chat_description_(client->get_chat_description(chat_id)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (error->code_ != 401 && !client_->need_close_ && !client_->closing_ && !client_->logging_out_) {
        LOG(ERROR) << "Can't delete failed to send message " << message_id_ << " because of "
                   << td::oneline(to_string(error)) << " in " << client_->get_chat_description(chat_id_)
                   << ". Old chat description: " << old_chat_description_;
      }
      return;
    }

    CHECK(result->get_id() == td_api::ok::ID);
    if (client_->get_message(chat_id_, message_id_) != nullptr) {
      LOG(ERROR) << "Have cache for message " << message_id_ << " in the chat " << chat_id_;
      client_->delete_message(chat_id_, message_id_, false);
    }
  }

 private:
  Client *client_;
  int64 chat_id_;
  int64 message_id_;
  td::string old_chat_description_;
};

class Client::TdOnEditMessageCallback final : public TdQueryCallback {
 public:
  TdOnEditMessageCallback(const Client *client, PromisedQueryPtr query) : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::message::ID);
    auto message = move_object_as<td_api::message>(result);
    int64 chat_id = message->chat_id_;
    int64 message_id = message->id_;

    auto message_info = client_->get_message(chat_id, message_id);
    if (message_info == nullptr) {
      return fail_query_with_error(std::move(query_), 400, "message not found");
    }
    message_info->is_content_changed = false;
    answer_query(JsonMessage(message_info, false, "edited message", client_), std::move(query_));
  }

 private:
  const Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnEditInlineMessageCallback final : public TdQueryCallback {
 public:
  explicit TdOnEditInlineMessageCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::ok::ID);
    answer_query(td::JsonTrue(), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnStopPollCallback final : public TdQueryCallback {
 public:
  TdOnStopPollCallback(const Client *client, int64 chat_id, int64 message_id, PromisedQueryPtr query)
      : client_(client), chat_id_(chat_id), message_id_(message_id), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::ok::ID);
    auto message_info = client_->get_message(chat_id_, message_id_);
    if (message_info == nullptr) {
      return fail_query_with_error(std::move(query_), 400, "message not found");
    }
    if (message_info->content->get_id() != td_api::messagePoll::ID) {
      LOG(ERROR) << "Poll not found in " << message_id_ << " in " << chat_id_;
      return fail_query_with_error(std::move(query_), 400, "message poll not found");
    }
    auto message_poll = static_cast<const td_api::messagePoll *>(message_info->content.get());
    answer_query(JsonPoll(message_poll->poll_.get(), client_), std::move(query_));
  }

 private:
  const Client *client_;
  int64 chat_id_;
  int64 message_id_;
  PromisedQueryPtr query_;
};

class Client::TdOnOkQueryCallback final : public TdQueryCallback {
 public:
  explicit TdOnOkQueryCallback(PromisedQueryPtr query) : query_(std::move(query)) {
    CHECK(query_ != nullptr);
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::ok::ID);
    answer_query(td::JsonTrue(), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

template <class OnSuccess>
class Client::TdOnCheckUserCallback final : public TdQueryCallback {
 public:
  TdOnCheckUserCallback(const Client *client, PromisedQueryPtr query, OnSuccess on_success)
      : client_(client), query_(std::move(query)), on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result), "user not found");
    }

    CHECK(result->get_id() == td_api::user::ID);
    auto user = move_object_as<td_api::user>(result);
    auto user_info = client_->get_user_info(user->id_);
    CHECK(user_info != nullptr);  // it must have already been got through updates

    return client_->check_user_read_access(user_info, std::move(query_), std::move(on_success_));
  }

 private:
  const Client *client_;
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

template <class OnSuccess>
class Client::TdOnCheckUserNoFailCallback final : public TdQueryCallback {
 public:
  TdOnCheckUserNoFailCallback(PromisedQueryPtr query, OnSuccess on_success)
      : query_(std::move(query)), on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    on_success_(std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

template <class OnSuccess>
class Client::TdOnCheckChatCallback final : public TdQueryCallback {
 public:
  TdOnCheckChatCallback(const Client *client, bool only_supergroup, AccessRights access_rights, PromisedQueryPtr query,
                        OnSuccess on_success)
      : client_(client)
      , only_supergroup_(only_supergroup)
      , access_rights_(access_rights)
      , query_(std::move(query))
      , on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result), "chat not found");
    }

    CHECK(result->get_id() == td_api::chat::ID);
    auto chat = move_object_as<td_api::chat>(result);
    auto chat_info = client_->get_chat(chat->id_);
    CHECK(chat_info != nullptr);  // it must have already been got through updates
    CHECK(chat_info->title == chat->title_);
    if (only_supergroup_ && chat_info->type != ChatInfo::Type::Supergroup) {
      return fail_query(400, "Bad Request: chat not found", std::move(query_));
    }

    return client_->check_chat_access(chat->id_, access_rights_, chat_info, std::move(query_), std::move(on_success_));
  }

 private:
  const Client *client_;
  bool only_supergroup_;
  AccessRights access_rights_;
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

template <class OnSuccess>
class Client::TdOnCheckChatNoFailCallback final : public TdQueryCallback {
 public:
  TdOnCheckChatNoFailCallback(int64 chat_id, PromisedQueryPtr query, OnSuccess on_success)
      : chat_id_(chat_id), query_(std::move(query)), on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    on_success_(chat_id_, std::move(query_));
  }

 private:
  int64 chat_id_;
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

template <class OnSuccess>
class Client::TdOnSearchStickerSetCallback final : public TdQueryCallback {
 public:
  TdOnSearchStickerSetCallback(PromisedQueryPtr query, OnSuccess on_success)
      : query_(std::move(query)), on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result), "sticker set not found");
    }

    CHECK(result->get_id() == td_api::stickerSet::ID);
    auto sticker_set = move_object_as<td_api::stickerSet>(result);
    on_success_(sticker_set->id_, std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

class Client::TdOnResolveBotUsernameCallback final : public TdQueryCallback {
 public:
  TdOnResolveBotUsernameCallback(Client *client, td::string username)
      : client_(client), username_(std::move(username)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return client_->on_resolve_bot_username(username_, 0);
    }

    CHECK(result->get_id() == td_api::chat::ID);
    auto chat = move_object_as<td_api::chat>(result);
    auto chat_info = client_->get_chat(chat->id_);
    CHECK(chat_info != nullptr);  // it must have already been got through updates
    if (chat_info->type != ChatInfo::Type::Private) {
      return client_->on_resolve_bot_username(username_, 0);
    }
    auto user_info = client_->get_user_info(chat_info->user_id);
    CHECK(user_info != nullptr);
    if (user_info->type != UserInfo::Type::Bot) {
      return client_->on_resolve_bot_username(username_, 0);
    }

    client_->on_resolve_bot_username(username_, chat_info->user_id);
  }

 private:
  Client *client_;
  td::string username_;
};

template <class OnSuccess>
class Client::TdOnCheckMessageCallback final : public TdQueryCallback {
 public:
  TdOnCheckMessageCallback(Client *client, int64 chat_id, int64 message_id, bool allow_empty, Slice message_type,
                           PromisedQueryPtr query, OnSuccess on_success)
      : client_(client)
      , chat_id_(chat_id)
      , message_id_(message_id)
      , allow_empty_(allow_empty)
      , message_type_(message_type)
      , query_(std::move(query))
      , on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (error->code_ == 429) {
        LOG(WARNING) << "Failed to get message " << message_id_ << " in " << chat_id_ << ": " << message_type_;
      }
      if (allow_empty_) {
        return on_success_(chat_id_, 0, std::move(query_));
      }
      return fail_query_with_error(std::move(query_), std::move(error), PSLICE() << message_type_ << " not found");
    }

    CHECK(result->get_id() == td_api::message::ID);
    auto full_message_id = client_->add_message(move_object_as<td_api::message>(result));
    CHECK(full_message_id.chat_id == chat_id_);
    CHECK(full_message_id.message_id == message_id_);
    on_success_(full_message_id.chat_id, full_message_id.message_id, std::move(query_));
  }

 private:
  Client *client_;
  int64 chat_id_;
  int64 message_id_;
  bool allow_empty_;
  Slice message_type_;
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

template <class OnSuccess>
class Client::TdOnCheckRemoteFileIdCallback final : public TdQueryCallback {
 public:
  TdOnCheckRemoteFileIdCallback(PromisedQueryPtr query, OnSuccess on_success)
      : query_(std::move(query)), on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result), "invalid file_id");
    }

    CHECK(result->get_id() == td_api::file::ID);
    on_success_(move_object_as<td_api::file>(result), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

template <class OnSuccess>
class Client::TdOnGetChatMemberCallback final : public TdQueryCallback {
 public:
  TdOnGetChatMemberCallback(PromisedQueryPtr query, OnSuccess on_success)
      : query_(std::move(query)), on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result), "user not found");
    }

    CHECK(result->get_id() == td_api::chatMember::ID);
    on_success_(move_object_as<td_api::chatMember>(result), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

class Client::TdOnDownloadFileCallback final : public TdQueryCallback {
 public:
  TdOnDownloadFileCallback(Client *client, int32 file_id) : client_(client), file_id_(file_id) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      return client_->on_file_download(file_id_, Status::Error(error->code_, error->message_));
    }
    CHECK(result->get_id() == td_api::file::ID);
    if (client_->is_file_being_downloaded(file_id_)) {  // if download is yet not finished
      client_->download_started_file_ids_.insert(file_id_);
    }
    client_->on_update_file(move_object_as<td_api::file>(result));
  }

 private:
  Client *client_;
  int32 file_id_;
};

class Client::TdOnCancelDownloadFileCallback final : public TdQueryCallback {
 public:
  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      LOG(ERROR) << "Failed to cancel download file";
      return;
    }
    CHECK(result->get_id() == td_api::ok::ID);
  }
};

class Client::TdOnGetReplyMessageCallback final : public TdQueryCallback {
 public:
  TdOnGetReplyMessageCallback(Client *client, int64 chat_id) : client_(client), chat_id_(chat_id) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return client_->on_get_reply_message(chat_id_, nullptr);
    }

    CHECK(result->get_id() == td_api::message::ID);
    client_->on_get_reply_message(chat_id_, move_object_as<td_api::message>(result));
  }

 private:
  Client *client_;
  int64 chat_id_;
};

class Client::TdOnGetEditedMessageCallback final : public TdQueryCallback {
 public:
  explicit TdOnGetEditedMessageCallback(Client *client) : client_(client) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (error->code_ == 429) {
        LOG(WARNING) << "Failed to get edited message";
      }
      return client_->on_get_edited_message(nullptr);
    }

    CHECK(result->get_id() == td_api::message::ID);
    client_->on_get_edited_message(move_object_as<td_api::message>(result));
  }

 private:
  Client *client_;
};

class Client::TdOnGetCallbackQueryMessageCallback final : public TdQueryCallback {
 public:
  TdOnGetCallbackQueryMessageCallback(Client *client, int64 user_id, int state)
      : client_(client), user_id_(user_id), state_(state) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (error->code_ == 429) {
        LOG(WARNING) << "Failed to get callback query message";
      }
      return client_->on_get_callback_query_message(nullptr, user_id_, state_);
    }

    CHECK(result->get_id() == td_api::message::ID);
    client_->on_get_callback_query_message(move_object_as<td_api::message>(result), user_id_, state_);
  }

 private:
  Client *client_;
  int64 user_id_;
  int state_;
};

class Client::TdOnGetStickerSetCallback final : public TdQueryCallback {
 public:
  TdOnGetStickerSetCallback(Client *client, int64 set_id, int64 new_callback_query_user_id, int64 new_message_chat_id)
      : client_(client)
      , set_id_(set_id)
      , new_callback_query_user_id_(new_callback_query_user_id)
      , new_message_chat_id_(new_message_chat_id) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (error->message_ != "STICKERSET_INVALID" && error->code_ != 401 && error->code_ != 500) {
        LOG(ERROR) << "Failed to get sticker set " << set_id_ << " from callback query by user "
                   << new_callback_query_user_id_ << "/new message in chat " << new_message_chat_id_ << ": "
                   << td::oneline(to_string(error));
      }
      return client_->on_get_sticker_set(set_id_, new_callback_query_user_id_, new_message_chat_id_, nullptr);
    }

    CHECK(result->get_id() == td_api::stickerSet::ID);
    client_->on_get_sticker_set(set_id_, new_callback_query_user_id_, new_message_chat_id_,
                                move_object_as<td_api::stickerSet>(result));
  }

 private:
  Client *client_;
  int64 set_id_;
  int64 new_callback_query_user_id_;
  int64 new_message_chat_id_;
};

class Client::TdOnGetChatStickerSetCallback final : public TdQueryCallback {
 public:
  TdOnGetChatStickerSetCallback(Client *client, int64 chat_id, int64 pinned_message_id, PromisedQueryPtr query)
      : client_(client), chat_id_(chat_id), pinned_message_id_(pinned_message_id), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      auto chat_info = client_->get_chat(chat_id_);
      CHECK(chat_info != nullptr);
      CHECK(chat_info->type == ChatInfo::Type::Supergroup);
      client_->set_supergroup_sticker_set_id(chat_info->supergroup_id, 0);
    } else {
      CHECK(result->get_id() == td_api::stickerSet::ID);
      auto sticker_set = move_object_as<td_api::stickerSet>(result);
      client_->on_get_sticker_set_name(sticker_set->id_, sticker_set->name_);
    }

    answer_query(JsonChat(chat_id_, true, client_, pinned_message_id_), std::move(query_));
  }

 private:
  Client *client_;
  int64 chat_id_;
  int64 pinned_message_id_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetChatPinnedMessageCallback final : public TdQueryCallback {
 public:
  TdOnGetChatPinnedMessageCallback(Client *client, int64 chat_id, PromisedQueryPtr query)
      : client_(client), chat_id_(chat_id), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    int64 pinned_message_id = 0;
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (error->code_ == 429) {
        return fail_query_with_error(std::move(query_), std::move(error));
      } else if (error->code_ != 404 && error->message_ != "CHANNEL_PRIVATE") {
        LOG(ERROR) << "Failed to get chat pinned message: " << to_string(error);
      }
    } else {
      CHECK(result->get_id() == td_api::message::ID);
      auto full_message_id = client_->add_message(move_object_as<td_api::message>(result));
      pinned_message_id = full_message_id.message_id;
      CHECK(full_message_id.chat_id == chat_id_);
      CHECK(pinned_message_id > 0);
    }

    auto chat_info = client_->get_chat(chat_id_);
    CHECK(chat_info != nullptr);
    if (chat_info->type == ChatInfo::Type::Supergroup) {
      auto supergroup_info = client_->get_supergroup_info(chat_info->supergroup_id);
      CHECK(supergroup_info != nullptr);

      auto sticker_set_id = supergroup_info->sticker_set_id;
      if (sticker_set_id != 0 && client_->get_sticker_set_name(sticker_set_id).empty()) {
        return client_->send_request(
            make_object<td_api::getStickerSet>(sticker_set_id),
            td::make_unique<TdOnGetChatStickerSetCallback>(client_, chat_id_, pinned_message_id, std::move(query_)));
      }
    }

    answer_query(JsonChat(chat_id_, true, client_, pinned_message_id), std::move(query_));
  }

 private:
  Client *client_;
  int64 chat_id_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetChatPinnedMessageToUnpinCallback final : public TdQueryCallback {
 public:
  TdOnGetChatPinnedMessageToUnpinCallback(Client *client, int64 chat_id, PromisedQueryPtr query)
      : client_(client), chat_id_(chat_id), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    int64 pinned_message_id = 0;
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (error->code_ == 429) {
        return fail_query_with_error(std::move(query_), std::move(error));
      } else {
        return fail_query_with_error(std::move(query_), make_object<td_api::error>(400, "Message to unpin not found"));
      }
    }

    CHECK(result->get_id() == td_api::message::ID);
    auto full_message_id = client_->add_message(move_object_as<td_api::message>(result));
    pinned_message_id = full_message_id.message_id;
    CHECK(full_message_id.chat_id == chat_id_);
    CHECK(pinned_message_id > 0);

    client_->send_request(make_object<td_api::unpinChatMessage>(chat_id_, pinned_message_id),
                          td::make_unique<TdOnOkQueryCallback>(std::move(query_)));
  }

 private:
  Client *client_;
  int64 chat_id_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetMyCommandsCallback final : public TdQueryCallback {
 public:
  explicit TdOnGetMyCommandsCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::botCommands::ID);
    auto bot_commands = move_object_as<td_api::botCommands>(result);
    answer_query(td::json_array(bot_commands->commands_, [](auto &command) { return JsonBotCommand(command.get()); }),
                 std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnGetMyDefaultAdministratorRightsCallback final : public TdQueryCallback {
 public:
  TdOnGetMyDefaultAdministratorRightsCallback(bool for_channels, PromisedQueryPtr query)
      : for_channels_(for_channels), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::userFullInfo::ID);
    auto full_info = move_object_as<td_api::userFullInfo>(result);
    if (full_info->bot_info_ == nullptr) {
      LOG(ERROR) << "Have no bot info for self";
      return fail_query_with_error(std::move(query_),
                                   make_object<td_api::error>(500, "Requested data is inaccessible"));
    }
    auto bot_info = std::move(full_info->bot_info_);
    const auto *rights = for_channels_ ? bot_info->default_channel_administrator_rights_.get()
                                       : bot_info->default_group_administrator_rights_.get();
    answer_query(JsonChatAdministratorRights(rights, for_channels_ ? ChatType::Channel : ChatType::Supergroup),
                 std::move(query_));
  }

 private:
  bool for_channels_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetMenuButtonCallback final : public TdQueryCallback {
 public:
  explicit TdOnGetMenuButtonCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::botMenuButton::ID);
    auto menu_button = move_object_as<td_api::botMenuButton>(result);
    answer_query(JsonBotMenuButton(menu_button.get()), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnGetChatFullInfoCallback final : public TdQueryCallback {
 public:
  TdOnGetChatFullInfoCallback(Client *client, int64 chat_id, PromisedQueryPtr query)
      : client_(client), chat_id_(chat_id), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    // we don't need the result, everything is already received through updates

    client_->send_request(make_object<td_api::getChatPinnedMessage>(chat_id_),
                          td::make_unique<TdOnGetChatPinnedMessageCallback>(client_, chat_id_, std::move(query_)));
  }

 private:
  Client *client_;
  int64 chat_id_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetGroupMembersCallback final : public TdQueryCallback {
 public:
  TdOnGetGroupMembersCallback(const Client *client, bool administrators_only, PromisedQueryPtr query)
      : client_(client), administrators_only_(administrators_only), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::basicGroupFullInfo::ID);
    auto group_full_info = move_object_as<td_api::basicGroupFullInfo>(result);
    answer_query(JsonChatMembers(group_full_info->members_, Client::ChatType::Group, administrators_only_, client_),
                 std::move(query_));
  }

 private:
  const Client *client_;
  bool administrators_only_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetSupergroupMembersCallback final : public TdQueryCallback {
 public:
  TdOnGetSupergroupMembersCallback(const Client *client, Client::ChatType chat_type, PromisedQueryPtr query)
      : client_(client), chat_type_(chat_type), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::chatMembers::ID);
    auto chat_members = move_object_as<td_api::chatMembers>(result);
    answer_query(JsonChatMembers(chat_members->members_, chat_type_, false, client_), std::move(query_));
  }

 private:
  const Client *client_;
  Client::ChatType chat_type_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetSupergroupMembersCountCallback final : public TdQueryCallback {
 public:
  explicit TdOnGetSupergroupMembersCountCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::supergroupFullInfo::ID);
    auto supergroup_full_info = move_object_as<td_api::supergroupFullInfo>(result);
    return answer_query(td::VirtuallyJsonableInt(supergroup_full_info->member_count_), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnCreateInvoiceLinkCallback final : public TdQueryCallback {
 public:
  explicit TdOnCreateInvoiceLinkCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::httpUrl::ID);
    auto http_url = move_object_as<td_api::httpUrl>(result);
    return answer_query(td::VirtuallyJsonableString(http_url->url_), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnReplacePrimaryChatInviteLinkCallback final : public TdQueryCallback {
 public:
  explicit TdOnReplacePrimaryChatInviteLinkCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::chatInviteLink::ID);
    auto invite_link = move_object_as<td_api::chatInviteLink>(result);
    return answer_query(td::VirtuallyJsonableString(invite_link->invite_link_), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnGetChatInviteLinkCallback final : public TdQueryCallback {
 public:
  TdOnGetChatInviteLinkCallback(const Client *client, PromisedQueryPtr query)
      : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    if (result->get_id() == td_api::chatInviteLink::ID) {
      auto invite_link = move_object_as<td_api::chatInviteLink>(result);
      return answer_query(JsonChatInviteLink(invite_link.get(), client_), std::move(query_));
    } else {
      CHECK(result->get_id() == td_api::chatInviteLinks::ID);
      auto invite_links = move_object_as<td_api::chatInviteLinks>(result);
      CHECK(!invite_links->invite_links_.empty());
      return answer_query(JsonChatInviteLink(invite_links->invite_links_[0].get(), client_), std::move(query_));
    }
  }

 private:
  const Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetGameHighScoresCallback final : public TdQueryCallback {
 public:
  TdOnGetGameHighScoresCallback(const Client *client, PromisedQueryPtr query)
      : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::gameHighScores::ID);
    auto game_high_scores = move_object_as<td_api::gameHighScores>(result);
    answer_query(td::json_array(game_high_scores->scores_,
                                [client = client_](auto &score) { return JsonGameHighScore(score.get(), client); }),
                 std::move(query_));
  }

 private:
  const Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnAnswerWebAppQueryCallback final : public TdQueryCallback {
 public:
  explicit TdOnAnswerWebAppQueryCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::sentWebAppMessage::ID);
    auto message = move_object_as<td_api::sentWebAppMessage>(result);
    answer_query(JsonSentWebAppMessage(message.get()), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnReturnFileCallback final : public TdQueryCallback {
 public:
  TdOnReturnFileCallback(const Client *client, PromisedQueryPtr query) : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::file::ID);
    auto file = move_object_as<td_api::file>(result);
    answer_query(JsonFile(file.get(), client_, false), std::move(query_));
  }

 private:
  const Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnReturnStickerSetCallback final : public TdQueryCallback {
 public:
  TdOnReturnStickerSetCallback(Client *client, bool return_sticker_set, PromisedQueryPtr query)
      : client_(client), return_sticker_set_(return_sticker_set), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::stickerSet::ID);
    auto sticker_set = move_object_as<td_api::stickerSet>(result);
    client_->on_get_sticker_set_name(sticker_set->id_, sticker_set->name_);
    if (return_sticker_set_) {
      answer_query(JsonStickerSet(sticker_set.get(), client_), std::move(query_));
    } else {
      answer_query(td::JsonTrue(), std::move(query_));
    }
  }

 private:
  Client *client_;
  bool return_sticker_set_;
  PromisedQueryPtr query_;
};

class Client::TdOnSendCustomRequestCallback final : public TdQueryCallback {
 public:
  explicit TdOnSendCustomRequestCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::customRequestResult::ID);
    auto res = move_object_as<td_api::customRequestResult>(result);
    answer_query(JsonCustomJson(res->result_), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

void Client::close() {
  need_close_ = true;
  if (td_client_.empty()) {
    set_timeout_in(0);
  } else if (!closing_) {
    do_send_request(make_object<td_api::close>(), td::make_unique<TdOnOkCallback>());
  }
}

void Client::log_out(bool is_api_id_invalid) {
  is_api_id_invalid_ |= is_api_id_invalid;
  if (!td_client_.empty() && !logging_out_ && !closing_) {
    do_send_request(make_object<td_api::logOut>(), td::make_unique<TdOnOkCallback>());
  }
}

std::size_t Client::get_pending_update_count() const {
  return parameters_->shared_data_->tqueue_->get_size(tqueue_id_);
}

void Client::update_last_synchronization_error_date() {
  if (disconnection_time_ == 0 || !was_authorized_ || logging_out_ || closing_) {
    return;
  }
  auto now = td::Time::now();
  if (last_update_creation_time_ > now - 10 || disconnection_time_ > now - 180) {
    return;
  }

  last_synchronization_error_date_ = get_unix_time();
}

ServerBotInfo Client::get_bot_info() const {
  ServerBotInfo res;
  res.id_ = bot_token_id_;
  res.token_ = bot_token_;
  auto user_info = get_user_info(my_id_);
  if (user_info != nullptr) {
    res.username_ = user_info->username;
  } else if (!was_authorized_) {
    res.username_ = "<unauthorized>";
  } else {
    res.username_ = "<unknown>";
  }
  res.webhook_ = webhook_url_;
  res.has_webhook_certificate_ = has_webhook_certificate_;
  auto &tqueue = parameters_->shared_data_->tqueue_;
  res.head_update_id_ = tqueue->get_head(tqueue_id_).value();
  res.tail_update_id_ = tqueue->get_tail(tqueue_id_).value();
  res.webhook_max_connections_ = webhook_max_connections_;
  res.pending_update_count_ = tqueue->get_size(tqueue_id_);
  res.start_time_ = start_time_;
  return res;
}

void Client::start_up() {
  start_time_ = td::Time::now();
  next_bot_updates_warning_time_ = start_time_ + 600;
  webhook_set_time_ = start_time_;
  next_allowed_set_webhook_time_ = start_time_;
  next_set_webhook_logging_time_ = start_time_;
  next_webhook_is_not_modified_warning_time_ = start_time_;
  previous_get_updates_start_time_ = start_time_ - 100;
  next_get_updates_conflict_time_ = start_time_ - 100;

  sticker_set_names_[GREAT_MINDS_SET_ID] = GREAT_MINDS_SET_NAME.str();

  auto colon_pos = bot_token_.find_first_of(':');
  if (colon_pos == td::string::npos) {
    LOG(WARNING) << "Wrong bot token " << bot_token_;
    logging_out_ = true;
    return finish_closing();
  }
  bot_token_id_ = bot_token_.substr(0, colon_pos);

  auto base64_bot_token = bot_token_.substr(colon_pos + 1);
  if (td::base64url_decode(base64_bot_token).is_error() || base64_bot_token.size() < 24) {
    LOG(WARNING) << "Wrong bot token " << bot_token_;
    logging_out_ = true;
    return finish_closing();
  }

  bot_token_with_dc_ = bot_token_ + (is_test_dc_ ? ":T" : "");

  auto context = std::make_shared<td::ActorContext>();
  set_context(context);
  set_tag(bot_token_id_);

  auto suff = bot_token_with_dc_ + TD_DIR_SLASH;
  if (!parameters_->allow_colon_in_filenames_) {
    for (auto &c : suff) {
      if (c == ':') {
        c = '~';
      }
    }
  }
  dir_ = parameters_->working_directory_ + suff;

  class TdCallback final : public td::TdCallback {
   public:
    explicit TdCallback(td::ActorId<Client> client) : client_(std::move(client)) {
    }
    void on_result(td::uint64 id, object_ptr<td_api::Object> result) final {
      send_closure_later(client_, &Client::on_result, id, std::move(result));
    }
    void on_error(td::uint64 id, object_ptr<td_api::error> result) final {
      send_closure_later(client_, &Client::on_result, id, std::move(result));
    }

   private:
    td::ActorId<Client> client_;
  };
  td::ClientActor::Options options;
  options.net_query_stats = parameters_->net_query_stats_;
  td_client_ = td::create_actor<td::ClientActor>("TdClientActor", td::make_unique<TdCallback>(actor_id(this)),
                                                 std::move(options));
}

void Client::send(PromisedQueryPtr query) {
  if (!query->is_internal()) {
    query->set_stat_actor(stat_actor_);
  }
  cmd_queue_.emplace(std::move(query));
  loop();
}

void Client::raw_event(const td::Event::Raw &event) {
  long_poll_wakeup(true);
}

void Client::loop() {
  if (was_authorized_ || logging_out_ || closing_) {
    while (!cmd_queue_.empty()) {
      auto query = std::move(cmd_queue_.front());
      cmd_queue_.pop();
      on_cmd(std::move(query));
    }
  }
}

void Client::on_get_reply_message(int64 chat_id, object_ptr<td_api::message> reply_to_message) {
  auto &queue = new_message_queues_[chat_id];
  CHECK(queue.has_active_request_);
  queue.has_active_request_ = false;

  CHECK(!queue.queue_.empty());
  object_ptr<td_api::message> &message = queue.queue_.front().message;
  CHECK(chat_id == message->chat_id_);
  int64 &reply_to_message_id = get_reply_to_message_id(message);
  CHECK(reply_to_message_id > 0);
  if (reply_to_message == nullptr) {
    LOG(INFO) << "Can't find message " << reply_to_message_id << " in chat " << chat_id
              << ". It is already deleted or inaccessible because of the chosen privacy mode";
    reply_to_message_id = 0;
  } else {
    CHECK(chat_id == reply_to_message->chat_id_);
    CHECK(reply_to_message_id == reply_to_message->id_);
    LOG(INFO) << "Receive reply to message " << reply_to_message_id << " in chat " << chat_id;
    add_message(std::move(reply_to_message));
  }

  process_new_message_queue(chat_id);
}

void Client::on_get_edited_message(object_ptr<td_api::message> edited_message) {
  if (edited_message == nullptr) {
    LOG(INFO) << "Can't find just edited message. It is already deleted or inaccessible because of chosen privacy mode";
  } else {
    add_new_message(std::move(edited_message), true);
  }
}

void Client::on_get_callback_query_message(object_ptr<td_api::message> message, int64 user_id, int state) {
  CHECK(user_id != 0);
  auto &queue = new_callback_query_queues_[user_id];
  CHECK(queue.has_active_request_);
  queue.has_active_request_ = false;

  CHECK(!queue.queue_.empty());
  int64 chat_id = queue.queue_.front()->chat_id_;
  int64 message_id = queue.queue_.front()->message_id_;
  if (message == nullptr) {
    if (state == 0) {
      LOG(INFO) << "Can't find callback query message " << message_id << " in chat " << chat_id
                << ". It may be already deleted";
    } else {
      CHECK(state == 1);
      auto message_info = get_message_editable(chat_id, message_id);
      if (message_info == nullptr) {
        LOG(INFO) << "Can't find callback query message " << message_id << " in chat " << chat_id
                  << ". It may be already deleted, while searcing for its reply to message";
        process_new_callback_query_queue(user_id, state);
        return;
      }
      LOG(INFO) << "Can't find callback query reply to message " << message_info->reply_to_message_id << " in chat "
                << chat_id << ". It may be already deleted";
      message_info->is_reply_to_message_deleted = true;
    }
  } else {
    LOG(INFO) << "Receive callback query " << (state == 1 ? "reply to " : "") << "message " << message_id << " in chat "
              << chat_id;
    add_message(std::move(message));
  }
  process_new_callback_query_queue(user_id, state + 1);
}

void Client::on_get_sticker_set(int64 set_id, int64 new_callback_query_user_id, int64 new_message_chat_id,
                                object_ptr<td_api::stickerSet> sticker_set) {
  if (new_callback_query_user_id != 0) {
    auto &queue = new_callback_query_queues_[new_callback_query_user_id];
    CHECK(queue.has_active_request_);
    queue.has_active_request_ = false;

    CHECK(!queue.queue_.empty());
  }
  if (new_message_chat_id != 0) {
    auto &queue = new_message_queues_[new_message_chat_id];
    CHECK(queue.has_active_request_);
    queue.has_active_request_ = false;

    CHECK(!queue.queue_.empty());
  }

  CHECK(set_id != 0);
  if (set_id != GREAT_MINDS_SET_ID) {
    td::string &set_name = sticker_set_names_[set_id];
    if (sticker_set != nullptr) {
      set_name = std::move(sticker_set->name_);
    }
  }

  if (new_callback_query_user_id != 0) {
    process_new_callback_query_queue(new_callback_query_user_id, 2);
  }
  if (new_message_chat_id != 0) {
    process_new_message_queue(new_message_chat_id);
  }
}

void Client::on_get_sticker_set_name(int64 set_id, const td::string &name) {
  CHECK(set_id != 0);
  if (set_id != GREAT_MINDS_SET_ID) {
    sticker_set_names_[set_id] = name;
  }
}

template <class OnSuccess>
void Client::check_user_read_access(const UserInfo *user_info, PromisedQueryPtr query, OnSuccess on_success) {
  CHECK(user_info != nullptr);
  if (!user_info->have_access) {
    // return fail_query(400, "Bad Request: have no access to the user", std::move(query));
  }
  on_success(std::move(query));
}

template <class OnSuccess>
void Client::check_user(int64 user_id, PromisedQueryPtr query, OnSuccess on_success) {
  const UserInfo *user_info = get_user_info(user_id);
  if (user_info != nullptr) {
    return check_user_read_access(user_info, std::move(query), std::move(on_success));
  }
  send_request(make_object<td_api::getUser>(user_id),
               td::make_unique<TdOnCheckUserCallback<OnSuccess>>(this, std::move(query), std::move(on_success)));
}

template <class OnSuccess>
void Client::check_user_no_fail(int64 user_id, PromisedQueryPtr query, OnSuccess on_success) {
  const UserInfo *user_info = get_user_info(user_id);
  if (user_info != nullptr) {
    on_success(std::move(query));
    return;
  }
  send_request(make_object<td_api::getUser>(user_id),
               td::make_unique<TdOnCheckUserNoFailCallback<OnSuccess>>(std::move(query), std::move(on_success)));
}

template <class OnSuccess>
void Client::check_chat_access(int64 chat_id, AccessRights access_rights, const ChatInfo *chat_info,
                               PromisedQueryPtr query, OnSuccess on_success) const {
  CHECK(chat_info != nullptr);
  bool need_write_access = access_rights == AccessRights::Write;
  bool need_edit_access = access_rights == AccessRights::Edit || need_write_access;
  bool need_read_access = true;
  switch (chat_info->type) {
    case ChatInfo::Type::Private: {
      auto user_info = get_user_info(chat_info->user_id);
      CHECK(user_info != nullptr);
      if (user_info->type == UserInfo::Type::Deleted && need_edit_access) {
        return fail_query(403, "Forbidden: user is deactivated", std::move(query));
      }
      if (user_info->type == UserInfo::Type::Unknown) {
        return fail_query(400, "Bad Request: private chat not found", std::move(query));
      }
      break;
    }
    case ChatInfo::Type::Group: {
      if (access_rights == AccessRights::ReadMembers) {  // member list is inaccessible in deactivated groups
        need_write_access = true;
        need_edit_access = true;
      }
      auto group_info = get_group_info(chat_info->group_id);
      CHECK(group_info != nullptr);
      if (!group_info->is_active && need_write_access) {
        if (group_info->upgraded_to_supergroup_id != 0) {
          td::FlatHashMap<td::string, td::unique_ptr<td::VirtuallyJsonable>> parameters;
          auto updagraded_to_chat_id = get_supergroup_chat_id(group_info->upgraded_to_supergroup_id);
          parameters.emplace("migrate_to_chat_id", td::make_unique<td::VirtuallyJsonableLong>(updagraded_to_chat_id));
          return fail_query(400, "Bad Request: group chat was upgraded to a supergroup chat", std::move(query),
                            std::move(parameters));
        } else {
          return fail_query(403, "Forbidden: the group chat was deleted", std::move(query));
        }
      }
      if (group_info->is_active && group_info->kicked && need_edit_access) {
        return fail_query(403, "Forbidden: bot was kicked from the group chat", std::move(query));
      }
      if (group_info->is_active && group_info->left && need_edit_access) {
        return fail_query(403, "Forbidden: bot is not a member of the group chat", std::move(query));
      }
      break;
    }
    case ChatInfo::Type::Supergroup: {
      auto supergroup_info = get_supergroup_info(chat_info->supergroup_id);
      CHECK(supergroup_info != nullptr);
      bool is_public = !supergroup_info->username.empty() || supergroup_info->has_location;
      if (supergroup_info->status->get_id() == td_api::chatMemberStatusBanned::ID) {
        if (supergroup_info->is_supergroup) {
          return fail_query(403, "Forbidden: bot was kicked from the supergroup chat", std::move(query));
        } else {
          return fail_query(403, "Forbidden: bot was kicked from the channel chat", std::move(query));
        }
      }
      bool need_more_access_rights = is_public ? need_edit_access : need_read_access;
      if (supergroup_info->status->get_id() == td_api::chatMemberStatusLeft::ID && need_more_access_rights) {
        if (supergroup_info->is_supergroup) {
          return fail_query(403, "Forbidden: bot is not a member of the supergroup chat", std::move(query));
        } else {
          return fail_query(403, "Forbidden: bot is not a member of the channel chat", std::move(query));
        }
      }
      break;
    }
    case ChatInfo::Type::Unknown:
    default:
      UNREACHABLE();
  }
  on_success(chat_id, std::move(query));
}

template <class OnSuccess>
void Client::check_chat(Slice chat_id_str, AccessRights access_rights, PromisedQueryPtr query, OnSuccess on_success) {
  if (chat_id_str.empty()) {
    return fail_query(400, "Bad Request: chat_id is empty", std::move(query));
  }

  if (chat_id_str[0] == '@') {
    return send_request(make_object<td_api::searchPublicChat>(chat_id_str.str()),
                        td::make_unique<TdOnCheckChatCallback<OnSuccess>>(this, true, access_rights, std::move(query),
                                                                          std::move(on_success)));
  }

  auto chat_id = td::to_integer<int64>(chat_id_str);
  auto chat_info = get_chat(chat_id);
  if (chat_info != nullptr) {
    return check_chat_access(chat_id, access_rights, chat_info, std::move(query), std::move(on_success));
  }
  send_request(make_object<td_api::getChat>(chat_id),
               td::make_unique<TdOnCheckChatCallback<OnSuccess>>(this, false, access_rights, std::move(query),
                                                                 std::move(on_success)));
}

template <class OnSuccess>
void Client::check_chat_no_fail(Slice chat_id_str, PromisedQueryPtr query, OnSuccess on_success) {
  if (chat_id_str.empty()) {
    return fail_query(400, "Bad Request: sender_chat_id is empty", std::move(query));
  }

  auto r_chat_id = td::to_integer_safe<int64>(chat_id_str);
  if (r_chat_id.is_error()) {
    return fail_query(400, "Bad Request: sender_chat_id is not a valid Integer", std::move(query));
  }
  auto chat_id = r_chat_id.move_as_ok();

  auto chat_info = get_chat(chat_id);
  if (chat_info != nullptr) {
    return on_success(chat_id, std::move(query));
  }
  send_request(make_object<td_api::getChat>(chat_id), td::make_unique<TdOnCheckChatNoFailCallback<OnSuccess>>(
                                                          chat_id, std::move(query), std::move(on_success)));
}

template <class OnSuccess>
void Client::check_bot_command_scope(BotCommandScope &&scope, PromisedQueryPtr query, OnSuccess on_success) {
  CHECK(scope.scope_ != nullptr);
  if (scope.chat_id_.empty()) {
    on_success(std::move(scope.scope_), std::move(query));
    return;
  }
  check_chat(scope.chat_id_, AccessRights::ReadMembers, std::move(query),
             [this, user_id = scope.user_id_, scope_id = scope.scope_->get_id(), on_success = std::move(on_success)](
                 int64 chat_id, PromisedQueryPtr query) mutable {
               switch (scope_id) {
                 case td_api::botCommandScopeChat::ID:
                   on_success(make_object<td_api::botCommandScopeChat>(chat_id), std::move(query));
                   break;
                 case td_api::botCommandScopeChatAdministrators::ID:
                   on_success(make_object<td_api::botCommandScopeChatAdministrators>(chat_id), std::move(query));
                   break;
                 case td_api::botCommandScopeChatMember::ID:
                   check_user_no_fail(
                       user_id, std::move(query),
                       [chat_id, user_id, on_success = std::move(on_success)](PromisedQueryPtr query) mutable {
                         on_success(make_object<td_api::botCommandScopeChatMember>(chat_id, user_id), std::move(query));
                       });
                   break;
                 default:
                   UNREACHABLE();
               }
             });
}

template <class OnSuccess>
void Client::check_remote_file_id(td::string file_id, PromisedQueryPtr query, OnSuccess on_success) {
  if (file_id.empty()) {
    return fail_query(400, "Bad Request: file_id not specified", std::move(query));
  }

  send_request(make_object<td_api::getRemoteFile>(std::move(file_id), nullptr),
               td::make_unique<TdOnCheckRemoteFileIdCallback<OnSuccess>>(std::move(query), std::move(on_success)));
}

bool Client::is_chat_member(const object_ptr<td_api::ChatMemberStatus> &status) {
  switch (status->get_id()) {
    case td_api::chatMemberStatusBanned::ID:
    case td_api::chatMemberStatusLeft::ID:
      return false;
    case td_api::chatMemberStatusRestricted::ID:
      return static_cast<const td_api::chatMemberStatusRestricted *>(status.get())->is_member_;
    default:
      // ignore Creator.is_member_
      return true;
  }
}

bool Client::have_message_access(int64 chat_id) const {
  auto chat_info = get_chat(chat_id);
  CHECK(chat_info != nullptr);
  switch (chat_info->type) {
    case ChatInfo::Type::Private:
    case ChatInfo::Type::Group:
      return true;
    case ChatInfo::Type::Supergroup: {
      auto supergroup_info = get_supergroup_info(chat_info->supergroup_id);
      CHECK(supergroup_info != nullptr);
      return is_chat_member(supergroup_info->status);
    }
    case ChatInfo::Type::Unknown:
    default:
      UNREACHABLE();
      return false;
  }
}

template <class OnSuccess>
void Client::check_message(Slice chat_id_str, int64 message_id, bool allow_empty, AccessRights access_rights,
                           Slice message_type, PromisedQueryPtr query, OnSuccess on_success) {
  check_chat(chat_id_str, access_rights, std::move(query),
             [this, message_id, allow_empty, message_type, on_success = std::move(on_success)](
                 int64 chat_id, PromisedQueryPtr query) mutable {
               if ((message_id <= 0 && !allow_empty) || !have_message_access(chat_id)) {
                 return fail_query_with_error(std::move(query), 400, "MESSAGE_NOT_FOUND",
                                              PSLICE() << message_type << " not found");
               }

               if (message_id <= 0) {
                 CHECK(allow_empty);
                 return on_success(chat_id, 0, std::move(query));
               }

               send_request(
                   make_object<td_api::getMessage>(chat_id, message_id),
                   td::make_unique<TdOnCheckMessageCallback<OnSuccess>>(
                       this, chat_id, message_id, allow_empty, message_type, std::move(query), std::move(on_success)));
             });
}

template <class OnSuccess>
void Client::resolve_sticker_set(const td::string &sticker_set_name, PromisedQueryPtr query, OnSuccess on_success) {
  if (sticker_set_name.empty()) {
    return fail_query(400, "Bad Request: sticker_set_name is empty", std::move(query));
  }

  send_request(make_object<td_api::searchStickerSet>(sticker_set_name),
               td::make_unique<TdOnSearchStickerSetCallback<OnSuccess>>(std::move(query), std::move(on_success)));
}

void Client::fix_reply_markup_bot_user_ids(object_ptr<td_api::ReplyMarkup> &reply_markup) const {
  if (reply_markup == nullptr || reply_markup->get_id() != td_api::replyMarkupInlineKeyboard::ID) {
    return;
  }
  auto inline_keyboard = static_cast<td_api::replyMarkupInlineKeyboard *>(reply_markup.get());
  for (auto &row : inline_keyboard->rows_) {
    for (auto &button : row) {
      CHECK(button != nullptr);
      CHECK(button->type_ != nullptr);
      if (button->type_->get_id() != td_api::inlineKeyboardButtonTypeLoginUrl::ID) {
        continue;
      }
      auto login_url_button = static_cast<td_api::inlineKeyboardButtonTypeLoginUrl *>(button->type_.get());
      if (login_url_button->id_ % 1000 != 0) {
        continue;
      }
      auto it = temp_to_real_bot_user_id_.find(std::abs(login_url_button->id_));
      CHECK(it != temp_to_real_bot_user_id_.end());
      auto bot_user_id = it->second;
      CHECK(bot_user_id != 0);
      if (login_url_button->id_ < 0) {
        login_url_button->id_ = -bot_user_id;
      } else {
        login_url_button->id_ = bot_user_id;
      }
    }
  }
}

void Client::fix_inline_query_results_bot_user_ids(
    td::vector<object_ptr<td_api::InputInlineQueryResult>> &results) const {
  for (auto &result : results) {
    td_api::downcast_call(
        *result, [this](auto &result_type) { this->fix_reply_markup_bot_user_ids(result_type.reply_markup_); });
  }
}

void Client::resolve_bot_usernames(PromisedQueryPtr query, td::Promise<PromisedQueryPtr> on_success) {
  CHECK(!unresolved_bot_usernames_.empty());
  auto query_id = current_bot_resolve_query_id_++;
  auto &pending_query = pending_bot_resolve_queries_[query_id];
  pending_query.pending_resolve_count = unresolved_bot_usernames_.size();
  pending_query.query = std::move(query);
  pending_query.on_success = std::move(on_success);
  for (auto &username : unresolved_bot_usernames_) {
    auto &query_ids = awaiting_bot_resolve_queries_[username];
    query_ids.push_back(query_id);
    if (query_ids.size() == 1) {
      send_request(make_object<td_api::searchPublicChat>(username),
                   td::make_unique<TdOnResolveBotUsernameCallback>(this, username));
    }
  }
  unresolved_bot_usernames_.clear();
}

template <class OnSuccess>
void Client::resolve_reply_markup_bot_usernames(object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query,
                                                OnSuccess on_success) {
  if (!unresolved_bot_usernames_.empty()) {
    CHECK(reply_markup != nullptr);
    CHECK(reply_markup->get_id() == td_api::replyMarkupInlineKeyboard::ID);
    return resolve_bot_usernames(
        std::move(query),
        td::PromiseCreator::lambda([this, reply_markup = std::move(reply_markup),
                                    on_success = std::move(on_success)](td::Result<PromisedQueryPtr> result) mutable {
          if (result.is_ok()) {
            fix_reply_markup_bot_user_ids(reply_markup);
            on_success(std::move(reply_markup), result.move_as_ok());
          }
        }));
  }
  on_success(std::move(reply_markup), std::move(query));
}

template <class OnSuccess>
void Client::resolve_inline_query_results_bot_usernames(td::vector<object_ptr<td_api::InputInlineQueryResult>> results,
                                                        PromisedQueryPtr query, OnSuccess on_success) {
  if (!unresolved_bot_usernames_.empty()) {
    return resolve_bot_usernames(
        std::move(query),
        td::PromiseCreator::lambda([this, results = std::move(results),
                                    on_success = std::move(on_success)](td::Result<PromisedQueryPtr> result) mutable {
          if (result.is_ok()) {
            fix_inline_query_results_bot_user_ids(results);
            on_success(std::move(results), result.move_as_ok());
          }
        }));
  }
  on_success(std::move(results), std::move(query));
}

void Client::on_resolve_bot_username(const td::string &username, int64 user_id) {
  auto query_ids_it = awaiting_bot_resolve_queries_.find(username);
  CHECK(query_ids_it != awaiting_bot_resolve_queries_.end());
  CHECK(!query_ids_it->second.empty());
  auto query_ids = std::move(query_ids_it->second);
  awaiting_bot_resolve_queries_.erase(query_ids_it);

  if (user_id == 0) {
    bot_user_ids_.erase(username);
  } else {
    auto &temp_bot_user_id = bot_user_ids_[username];
    temp_to_real_bot_user_id_[temp_bot_user_id] = user_id;
    temp_bot_user_id = user_id;
  }

  for (auto query_id : query_ids) {
    auto it = pending_bot_resolve_queries_.find(query_id);
    if (it == pending_bot_resolve_queries_.end()) {
      // the query has already failed
      continue;
    }
    CHECK(it->second.pending_resolve_count > 0);
    it->second.pending_resolve_count--;
    if (it->second.pending_resolve_count == 0 || user_id == 0) {
      if (user_id == 0) {
        fail_query(400, PSTRING() << "Bad Request: bot \"" << username << "\" not found", std::move(it->second.query));
      } else {
        it->second.on_success.set_value(std::move(it->second.query));
      }
      pending_bot_resolve_queries_.erase(it);
    }
  }
}

template <class OnSuccess>
void Client::get_chat_member(int64 chat_id, int64 user_id, PromisedQueryPtr query, OnSuccess on_success) {
  check_user_no_fail(
      user_id, std::move(query),
      [this, chat_id, user_id, on_success = std::move(on_success)](PromisedQueryPtr query) mutable {
        send_request(make_object<td_api::getChatMember>(chat_id, make_object<td_api::messageSenderUser>(user_id)),
                     td::make_unique<TdOnGetChatMemberCallback<OnSuccess>>(std::move(query), std::move(on_success)));
      });
}

void Client::send_request(object_ptr<td_api::Function> &&f, td::unique_ptr<TdQueryCallback> handler) {
  if (logging_out_) {
    return handler->on_result(
        make_object<td_api::error>(LOGGING_OUT_ERROR_CODE, get_logging_out_error_description().str()));
  }
  if (closing_) {
    return handler->on_result(make_object<td_api::error>(CLOSING_ERROR_CODE, CLOSING_ERROR_DESCRIPTION.str()));
  }

  do_send_request(std::move(f), std::move(handler));
}

void Client::do_send_request(object_ptr<td_api::Function> &&f, td::unique_ptr<TdQueryCallback> handler) {
  CHECK(!td_client_.empty());
  auto id = handlers_.create(std::move(handler));
  send_closure(td_client_, &td::ClientActor::request, id, std::move(f));
}

td_api::object_ptr<td_api::Object> Client::execute(object_ptr<td_api::Function> &&f) {
  return td::ClientActor::execute(std::move(f));
}

void Client::on_update_file(object_ptr<td_api::file> file) {
  auto file_id = file->id_;
  if (!is_file_being_downloaded(file_id)) {
    return;
  }
  if (!parameters_->local_mode_ && file->local_->downloaded_size_ > MAX_DOWNLOAD_FILE_SIZE) {
    if (file->local_->is_downloading_active_) {
      send_request(make_object<td_api::cancelDownloadFile>(file_id, false),
                   td::make_unique<TdOnCancelDownloadFileCallback>());
    }
    return on_file_download(file_id, Status::Error(400, "Bad Request: file is too big"));
  }
  if (file->local_->is_downloading_completed_) {
    return on_file_download(file_id, std::move(file));
  }
  if (!file->local_->is_downloading_active_ && download_started_file_ids_.count(file_id)) {
    // also includes all 5xx and 429 errors
    auto error = Status::Error(400, "Bad Request: wrong file_id or the file is temporarily unavailable");
    if (logging_out_) {
      error = Status::Error(LOGGING_OUT_ERROR_CODE, get_logging_out_error_description());
    }
    if (closing_) {
      error = Status::Error(CLOSING_ERROR_CODE, CLOSING_ERROR_DESCRIPTION);
    }
    return on_file_download(file_id, std::move(error));
  }
}

void Client::on_update_authorization_state() {
  CHECK(authorization_state_ != nullptr);
  switch (authorization_state_->get_id()) {
    case td_api::authorizationStateWaitTdlibParameters::ID: {
      send_request(
          make_object<td_api::setOption>("ignore_inline_thumbnails", make_object<td_api::optionValueBoolean>(true)),
          td::make_unique<TdOnOkCallback>());
      send_request(make_object<td_api::setOption>("reuse_uploaded_photos_by_hash",
                                                  make_object<td_api::optionValueBoolean>(true)),
                   td::make_unique<TdOnOkCallback>());
      send_request(make_object<td_api::setOption>("disable_persistent_network_statistics",
                                                  make_object<td_api::optionValueBoolean>(true)),
                   td::make_unique<TdOnOkCallback>());
      send_request(make_object<td_api::setOption>("disable_time_adjustment_protection",
                                                  make_object<td_api::optionValueBoolean>(true)),
                   td::make_unique<TdOnOkCallback>());

      auto parameters = make_object<td_api::tdlibParameters>();

      parameters->use_test_dc_ = is_test_dc_;
      parameters->database_directory_ = dir_;
      //parameters->use_file_database_ = false;
      //parameters->use_chat_info_database_ = false;
      //parameters->use_secret_chats_ = false;
      parameters->use_message_database_ = USE_MESSAGE_DATABASE;
      parameters->api_id_ = parameters_->api_id_;
      parameters->api_hash_ = parameters_->api_hash_;
      parameters->system_language_code_ = "en";
      parameters->device_model_ = "server";
      parameters->application_version_ = parameters_->version_;
      parameters->enable_storage_optimizer_ = true;
      parameters->ignore_file_names_ = true;

      return send_request(make_object<td_api::setTdlibParameters>(std::move(parameters)),
                          td::make_unique<TdOnInitCallback>(this));
    }
    case td_api::authorizationStateWaitEncryptionKey::ID:
      return send_request(make_object<td_api::checkDatabaseEncryptionKey>(), td::make_unique<TdOnInitCallback>(this));
    case td_api::authorizationStateWaitPhoneNumber::ID:
      send_request(make_object<td_api::setOption>("online", make_object<td_api::optionValueBoolean>(true)),
                   td::make_unique<TdOnOkCallback>());
      return send_request(make_object<td_api::checkAuthenticationBotToken>(bot_token_),
                          td::make_unique<TdOnAuthorizationCallback>(this));
    case td_api::authorizationStateReady::ID: {
      auto user_info = get_user_info(my_id_);
      if (my_id_ <= 0 || user_info == nullptr) {
        LOG(INFO) << "Send getMe request for " << my_id_;
        return send_request(make_object<td_api::getMe>(), td::make_unique<TdOnAuthorizationCallback>(this));
      }

      if (!was_authorized_) {
        LOG(WARNING) << "Logged in as @" << user_info->username;
        was_authorized_ = true;
        td::send_event(parent_, td::Event::raw(static_cast<void *>(this)));
        update_shared_unix_time_difference();
        if (!pending_updates_.empty()) {
          LOG(INFO) << "Process " << pending_updates_.size() << " pending updates";
          for (auto &update : pending_updates_) {
            on_update(std::move(update));
          }
          td::reset_to_empty(pending_updates_);
        }
        last_update_creation_time_ = td::Time::now();
      }
      return loop();
    }
    case td_api::authorizationStateLoggingOut::ID:
      if (!logging_out_) {
        LOG(WARNING) << "Logging out";
        update_last_synchronization_error_date();
        logging_out_ = true;
        if (was_authorized_ && !closing_) {
          td::send_event(parent_, td::Event::raw(nullptr));
        }
      }
      return loop();
    case td_api::authorizationStateClosing::ID:
      if (!closing_) {
        LOG(WARNING) << "Closing";
        update_last_synchronization_error_date();
        closing_ = true;
        if (was_authorized_ && !logging_out_) {
          td::send_event(parent_, td::Event::raw(nullptr));
        }
      }
      return loop();
    case td_api::authorizationStateClosed::ID:
      return on_closed();
    default:
      return log_out(false);  // just in case
  }
}

bool Client::allow_update_before_authorization(const td_api::Object *update) const {
  auto update_id = update->get_id();
  if (update_id == td_api::updateAuthorizationState::ID) {
    return true;
  }
  if (update_id == td_api::updateOption::ID) {
    const auto &name = static_cast<const td_api::updateOption *>(update)->name_;
    return name == "my_id" || name == "unix_time";
  }
  if (update_id == td_api::updateUser::ID) {
    return true;
  }
  return false;
}

void Client::update_shared_unix_time_difference() {
  CHECK(was_authorized_);
  LOG_IF(ERROR, local_unix_time_difference_ == 0) << "Unix time difference was not updated";
  auto data = parameters_->shared_data_.get();
  if (local_unix_time_difference_ > data->unix_time_difference_) {
    data->unix_time_difference_ = local_unix_time_difference_;
  }
}

void Client::on_update(object_ptr<td_api::Object> result) {
  if (!was_authorized_ && !allow_update_before_authorization(result.get())) {
    pending_updates_.push_back(std::move(result));
    return;
  }
  switch (result->get_id()) {
    case td_api::updateAuthorizationState::ID: {
      auto update = move_object_as<td_api::updateAuthorizationState>(result);
      authorization_state_ = std::move(update->authorization_state_);
      on_update_authorization_state();
      break;
    }
    case td_api::updateNewMessage::ID: {
      auto update = move_object_as<td_api::updateNewMessage>(result);
      add_new_message(std::move(update->message_), false);
      break;
    }
    case td_api::updateMessageSendSucceeded::ID: {
      auto update = move_object_as<td_api::updateMessageSendSucceeded>(result);
      on_message_send_succeeded(std::move(update->message_), update->old_message_id_);
      break;
    }
    case td_api::updateMessageSendFailed::ID: {
      auto update = move_object_as<td_api::updateMessageSendFailed>(result);
      on_message_send_failed(update->message_->chat_id_, update->old_message_id_, update->message_->id_,
                             Status::Error(update->error_code_, update->error_message_));
      break;
    }
    case td_api::updateMessageContent::ID: {
      auto update = move_object_as<td_api::updateMessageContent>(result);
      update_message_content(update->chat_id_, update->message_id_, std::move(update->new_content_));
      break;
    }
    case td_api::updateMessageEdited::ID: {
      auto update = move_object_as<td_api::updateMessageEdited>(result);
      auto chat_id = update->chat_id_;
      auto message_id = update->message_id_;
      on_update_message_edited(chat_id, message_id, update->edit_date_, std::move(update->reply_markup_));
      send_request(make_object<td_api::getMessage>(chat_id, message_id),
                   td::make_unique<TdOnGetEditedMessageCallback>(this));
      break;
    }
    case td_api::updateDeleteMessages::ID: {
      auto update = move_object_as<td_api::updateDeleteMessages>(result);
      for (auto message_id : update->message_ids_) {
        delete_message(update->chat_id_, message_id, update->from_cache_);
      }
      break;
    }
    case td_api::updateFile::ID: {
      auto update = move_object_as<td_api::updateFile>(result);
      on_update_file(std::move(update->file_));
      break;
    }
    case td_api::updateFileGenerationStart::ID: {
      auto update = move_object_as<td_api::updateFileGenerationStart>(result);
      auto generation_id = update->generation_id_;
      send_request(
          make_object<td_api::finishFileGeneration>(generation_id, make_object<td_api::error>(400, "Wrong file_id")),
          td::make_unique<TdOnOkCallback>());
      break;
    }
    case td_api::updateNewChat::ID: {
      auto update = move_object_as<td_api::updateNewChat>(result);
      auto chat = std::move(update->chat_);
      auto chat_info = add_chat(chat->id_);
      bool need_warning = false;
      switch (chat->type_->get_id()) {
        case td_api::chatTypePrivate::ID: {
          auto type = move_object_as<td_api::chatTypePrivate>(chat->type_);
          chat_info->type = ChatInfo::Type::Private;
          auto user_id = type->user_id_;
          chat_info->user_id = user_id;
          need_warning = get_user_info(user_id) == nullptr;
          break;
        }
        case td_api::chatTypeBasicGroup::ID: {
          auto type = move_object_as<td_api::chatTypeBasicGroup>(chat->type_);
          chat_info->type = ChatInfo::Type::Group;
          auto group_id = type->basic_group_id_;
          chat_info->group_id = group_id;
          need_warning = get_group_info(group_id) == nullptr;
          break;
        }
        case td_api::chatTypeSupergroup::ID: {
          auto type = move_object_as<td_api::chatTypeSupergroup>(chat->type_);
          chat_info->type = ChatInfo::Type::Supergroup;
          auto supergroup_id = type->supergroup_id_;
          chat_info->supergroup_id = supergroup_id;
          need_warning = get_supergroup_info(supergroup_id) == nullptr;
          break;
        }
        case td_api::chatTypeSecret::ID:
          // unsupported
          break;
        default:
          UNREACHABLE();
      }
      if (need_warning) {
        LOG(ERROR) << "Received updateNewChat about chat " << chat->id_ << ", but hadn't received corresponding info";
      }

      chat_info->title = std::move(chat->title_);
      chat_info->photo_info = std::move(chat->photo_);
      chat_info->permissions = std::move(chat->permissions_);
      chat_info->message_auto_delete_time = chat->message_ttl_;
      chat_info->has_protected_content = chat->has_protected_content_;
      break;
    }
    case td_api::updateChatTitle::ID: {
      auto update = move_object_as<td_api::updateChatTitle>(result);
      auto chat_info = add_chat(update->chat_id_);
      CHECK(chat_info->type != ChatInfo::Type::Unknown);
      chat_info->title = std::move(update->title_);
      break;
    }
    case td_api::updateChatPhoto::ID: {
      auto update = move_object_as<td_api::updateChatPhoto>(result);
      auto chat_info = add_chat(update->chat_id_);
      CHECK(chat_info->type != ChatInfo::Type::Unknown);
      chat_info->photo_info = std::move(update->photo_);
      break;
    }
    case td_api::updateChatPermissions::ID: {
      auto update = move_object_as<td_api::updateChatPermissions>(result);
      auto chat_info = add_chat(update->chat_id_);
      CHECK(chat_info->type != ChatInfo::Type::Unknown);
      chat_info->permissions = std::move(update->permissions_);
      break;
    }
    case td_api::updateChatMessageTtl::ID: {
      auto update = move_object_as<td_api::updateChatMessageTtl>(result);
      auto chat_info = add_chat(update->chat_id_);
      CHECK(chat_info->type != ChatInfo::Type::Unknown);
      chat_info->message_auto_delete_time = update->message_ttl_;
      break;
    }
    case td_api::updateChatHasProtectedContent::ID: {
      auto update = move_object_as<td_api::updateChatHasProtectedContent>(result);
      auto chat_info = add_chat(update->chat_id_);
      CHECK(chat_info->type != ChatInfo::Type::Unknown);
      chat_info->has_protected_content = update->has_protected_content_;
      break;
    }
    case td_api::updateUser::ID: {
      auto update = move_object_as<td_api::updateUser>(result);
      auto *user_info = add_user_info(update->user_->id_);
      add_user(user_info, std::move(update->user_));
      break;
    }
    case td_api::updateUserFullInfo::ID: {
      auto update = move_object_as<td_api::updateUserFullInfo>(result);
      auto user_id = update->user_id_;
      auto full_info = update->user_full_info_.get();
      set_user_photo(user_id, std::move(full_info->photo_));
      if (full_info->bio_ != nullptr) {
        set_user_bio(user_id, std::move(full_info->bio_->text_));
      }
      set_user_has_private_forwards(user_id, full_info->has_private_forwards_);
      break;
    }
    case td_api::updateBasicGroup::ID: {
      auto update = move_object_as<td_api::updateBasicGroup>(result);
      auto *group_info = add_group_info(update->basic_group_->id_);
      add_group(group_info, std::move(update->basic_group_));
      break;
    }
    case td_api::updateBasicGroupFullInfo::ID: {
      auto update = move_object_as<td_api::updateBasicGroupFullInfo>(result);
      auto group_id = update->basic_group_id_;
      auto full_info = update->basic_group_full_info_.get();
      set_group_photo(group_id, std::move(full_info->photo_));
      set_group_description(group_id, std::move(full_info->description_));
      set_group_invite_link(group_id, full_info->invite_link_ != nullptr
                                          ? std::move(full_info->invite_link_->invite_link_)
                                          : td::string());
      break;
    }
    case td_api::updateSupergroup::ID: {
      auto update = move_object_as<td_api::updateSupergroup>(result);
      auto *supergroup_info = add_supergroup_info(update->supergroup_->id_);
      add_supergroup(supergroup_info, std::move(update->supergroup_));
      break;
    }
    case td_api::updateSupergroupFullInfo::ID: {
      auto update = move_object_as<td_api::updateSupergroupFullInfo>(result);
      auto supergroup_id = update->supergroup_id_;
      auto full_info = update->supergroup_full_info_.get();
      set_supergroup_photo(supergroup_id, std::move(full_info->photo_));
      set_supergroup_description(supergroup_id, std::move(full_info->description_));
      set_supergroup_invite_link(supergroup_id, full_info->invite_link_ != nullptr
                                                    ? std::move(full_info->invite_link_->invite_link_)
                                                    : td::string());
      set_supergroup_sticker_set_id(supergroup_id, full_info->sticker_set_id_);
      set_supergroup_can_set_sticker_set(supergroup_id, full_info->can_set_sticker_set_);
      set_supergroup_slow_mode_delay(supergroup_id, full_info->slow_mode_delay_);
      set_supergroup_linked_chat_id(supergroup_id, full_info->linked_chat_id_);
      set_supergroup_location(supergroup_id, std::move(full_info->location_));
      break;
    }
    case td_api::updateOption::ID: {
      auto update = move_object_as<td_api::updateOption>(result);
      const td::string &name = update->name_;
      if (name == "my_id") {
        if (update->value_->get_id() == td_api::optionValueEmpty::ID) {
          CHECK(logging_out_);
          my_id_ = -1;
        } else {
          CHECK(update->value_->get_id() == td_api::optionValueInteger::ID);
          my_id_ = move_object_as<td_api::optionValueInteger>(update->value_)->value_;
        }
      }
      if (name == "group_anonymous_bot_user_id" && update->value_->get_id() == td_api::optionValueInteger::ID) {
        group_anonymous_bot_user_id_ = move_object_as<td_api::optionValueInteger>(update->value_)->value_;
      }
      if (name == "channel_bot_user_id" && update->value_->get_id() == td_api::optionValueInteger::ID) {
        channel_bot_user_id_ = move_object_as<td_api::optionValueInteger>(update->value_)->value_;
      }
      if (name == "telegram_service_notifications_chat_id" &&
          update->value_->get_id() == td_api::optionValueInteger::ID) {
        service_notifications_user_id_ = move_object_as<td_api::optionValueInteger>(update->value_)->value_;
      }
      if (name == "authorization_date") {
        if (update->value_->get_id() == td_api::optionValueEmpty::ID) {
          authorization_date_ = -1;
        } else {
          CHECK(update->value_->get_id() == td_api::optionValueInteger::ID);
          authorization_date_ = static_cast<int32>(move_object_as<td_api::optionValueInteger>(update->value_)->value_);
        }
      }
      if (name == "xallowed_update_types") {
        if (update->value_->get_id() == td_api::optionValueEmpty::ID) {
          allowed_update_types_ = DEFAULT_ALLOWED_UPDATE_TYPES;
        } else {
          CHECK(update->value_->get_id() == td_api::optionValueInteger::ID);
          allowed_update_types_ =
              static_cast<td::uint32>(move_object_as<td_api::optionValueInteger>(update->value_)->value_);
        }
      }
      if (name == "unix_time" && update->value_->get_id() != td_api::optionValueEmpty::ID) {
        CHECK(update->value_->get_id() == td_api::optionValueInteger::ID);
        local_unix_time_difference_ =
            static_cast<double>(move_object_as<td_api::optionValueInteger>(update->value_)->value_) - td::Time::now();
        if (was_authorized_) {
          update_shared_unix_time_difference();
        }
      }
      break;
    }
    case td_api::updatePoll::ID:
      add_update_poll(move_object_as<td_api::updatePoll>(result));
      break;
    case td_api::updatePollAnswer::ID:
      add_update_poll_answer(move_object_as<td_api::updatePollAnswer>(result));
      break;
    case td_api::updateNewInlineQuery::ID: {
      auto update = move_object_as<td_api::updateNewInlineQuery>(result);
      add_new_inline_query(update->id_, update->sender_user_id_, std::move(update->user_location_),
                           std::move(update->chat_type_), update->query_, update->offset_);
      break;
    }
    case td_api::updateNewChosenInlineResult::ID: {
      auto update = move_object_as<td_api::updateNewChosenInlineResult>(result);
      add_new_chosen_inline_result(update->sender_user_id_, std::move(update->user_location_), update->query_,
                                   update->result_id_, update->inline_message_id_);
      break;
    }
    case td_api::updateNewCallbackQuery::ID:
      add_new_callback_query(move_object_as<td_api::updateNewCallbackQuery>(result));
      break;
    case td_api::updateNewInlineCallbackQuery::ID:
      add_new_inline_callback_query(move_object_as<td_api::updateNewInlineCallbackQuery>(result));
      break;
    case td_api::updateNewShippingQuery::ID:
      add_new_shipping_query(move_object_as<td_api::updateNewShippingQuery>(result));
      break;
    case td_api::updateNewPreCheckoutQuery::ID:
      add_new_pre_checkout_query(move_object_as<td_api::updateNewPreCheckoutQuery>(result));
      break;
    case td_api::updateNewCustomEvent::ID:
      add_new_custom_event(move_object_as<td_api::updateNewCustomEvent>(result));
      break;
    case td_api::updateNewCustomQuery::ID:
      add_new_custom_query(move_object_as<td_api::updateNewCustomQuery>(result));
      break;
    case td_api::updateChatMember::ID:
      add_update_chat_member(move_object_as<td_api::updateChatMember>(result));
      break;
    case td_api::updateNewChatJoinRequest::ID:
      add_update_chat_join_request(move_object_as<td_api::updateNewChatJoinRequest>(result));
      break;
    case td_api::updateConnectionState::ID: {
      auto update = move_object_as<td_api::updateConnectionState>(result);
      if (update->state_->get_id() == td_api::connectionStateReady::ID) {
        update_last_synchronization_error_date();
        disconnection_time_ = 0;
      } else if (disconnection_time_ == 0) {
        disconnection_time_ = td::Time::now();
      }
      break;
    }
    default:
      // we are not interested in this update
      break;
  }
}

void Client::on_result(td::uint64 id, object_ptr<td_api::Object> result) {
  LOG(DEBUG) << "Receive from Td: " << id << " " << to_string(result);
  if (id == 0) {
    return on_update(std::move(result));
  }

  auto *handler_ptr = handlers_.get(id);
  CHECK(handler_ptr != nullptr);
  auto handler = std::move(*handler_ptr);
  handler->on_result(std::move(result));
  handlers_.erase(id);
}

td::Slice Client::get_logging_out_error_description() const {
  return is_api_id_invalid_ ? API_ID_INVALID_ERROR_DESCRIPTION : LOGGING_OUT_ERROR_DESCRIPTION;
}

void Client::on_closed() {
  LOG(WARNING) << "Closed";
  CHECK(logging_out_ || closing_);
  CHECK(!td_client_.empty());
  td_client_.reset();

  int http_status_code = logging_out_ ? LOGGING_OUT_ERROR_CODE : CLOSING_ERROR_CODE;
  Slice description = logging_out_ ? get_logging_out_error_description() : CLOSING_ERROR_DESCRIPTION;
  if (webhook_set_query_) {
    fail_query(http_status_code, description, std::move(webhook_set_query_));
  }
  if (!webhook_url_.empty()) {
    webhook_id_.reset();
  }
  if (long_poll_query_) {
    long_poll_wakeup(true);
    CHECK(!long_poll_query_);
  }

  while (!cmd_queue_.empty()) {
    auto query = std::move(cmd_queue_.front());
    cmd_queue_.pop();
    fail_query(http_status_code, description, std::move(query));
  }

  while (!yet_unsent_messages_.empty()) {
    auto it = yet_unsent_messages_.begin();
    auto chat_id = it->first.chat_id;
    auto message_id = it->first.message_id;
    if (!USE_MESSAGE_DATABASE) {
      LOG(ERROR) << "Doesn't receive updateMessageSendFailed for message " << message_id << " in chat " << chat_id;
    }
    on_message_send_failed(chat_id, message_id, 0, Status::Error(http_status_code, description));
  }
  CHECK(yet_unsent_message_count_.empty());

  while (!pending_bot_resolve_queries_.empty()) {
    auto it = pending_bot_resolve_queries_.begin();
    fail_query(http_status_code, description, std::move(it->second.query));
    pending_bot_resolve_queries_.erase(it);
  }

  while (!file_download_listeners_.empty()) {
    auto it = file_download_listeners_.begin();
    auto file_id = it->first;
    LOG(ERROR) << "Doesn't receive updateFile for file " << file_id;
    on_file_download(file_id, Status::Error(http_status_code, description));
  }

  if (logging_out_) {
    parameters_->shared_data_->webhook_db_->erase(bot_token_with_dc_);

    class RmWorker final : public td::Actor {
     public:
      RmWorker(td::string dir, td::ActorId<Client> parent) : dir_(std::move(dir)), parent_(std::move(parent)) {
      }

     private:
      td::string dir_;
      td::ActorId<Client> parent_;

      void start_up() final {
        CHECK(dir_.size() >= 24);
        CHECK(dir_.back() == TD_DIR_SLASH);
        td::rmrf(dir_).ignore();
        stop();
      }
      void tear_down() final {
        send_closure(parent_, &Client::finish_closing);
      }
    };
    // NB: the same scheduler as for database in Td
    auto current_scheduler_id = td::Scheduler::instance()->sched_id();
    auto scheduler_count = td::Scheduler::instance()->sched_count();
    auto scheduler_id = td::min(current_scheduler_id + 1, scheduler_count - 1);
    td::create_actor_on_scheduler<RmWorker>("RmWorker", scheduler_id, dir_, actor_id(this)).release();
    return;
  }

  finish_closing();
}

void Client::finish_closing() {
  if (clear_tqueue_ && logging_out_) {
    clear_tqueue();
  }

  set_timeout_in(need_close_ ? 0 : 600);
}

void Client::timeout_expired() {
  LOG(WARNING) << "Stop client";
  stop();
}

void Client::clear_tqueue() {
  CHECK(webhook_id_.empty());
  auto &tqueue = parameters_->shared_data_->tqueue_;
  auto size = tqueue->get_size(tqueue_id_);
  if (size > 0) {
    LOG(INFO) << "Removing " << size << " tqueue events";
    td::MutableSpan<td::TQueue::Event> span;
    auto r_size = tqueue->get(tqueue_id_, tqueue->get_tail(tqueue_id_), true, 0, span);
    CHECK(r_size.is_ok());
    CHECK(r_size.ok() == 0);
    CHECK(tqueue->get_size(tqueue_id_) == 0);
  }
}

bool Client::to_bool(td::MutableSlice value) {
  td::to_lower_inplace(value);
  value = td::trim(value);
  return value == "true" || value == "yes" || value == "1";
}

td::Result<td_api::object_ptr<td_api::keyboardButton>> Client::get_keyboard_button(JsonValue &button) {
  if (button.type() == JsonValue::Type::Object) {
    auto &object = button.get_object();

    TRY_RESULT(text, get_json_object_string_field(object, "text", false));

    TRY_RESULT(request_phone_number, get_json_object_bool_field(object, "request_phone_number"));
    TRY_RESULT(request_contact, get_json_object_bool_field(object, "request_contact"));
    if (request_phone_number || request_contact) {
      return make_object<td_api::keyboardButton>(text, make_object<td_api::keyboardButtonTypeRequestPhoneNumber>());
    }

    TRY_RESULT(request_location, get_json_object_bool_field(object, "request_location"));
    if (request_location) {
      return make_object<td_api::keyboardButton>(text, make_object<td_api::keyboardButtonTypeRequestLocation>());
    }

    if (has_json_object_field(object, "request_poll")) {
      bool force_regular = false;
      bool force_quiz = false;
      TRY_RESULT(request_poll, get_json_object_field(object, "request_poll", JsonValue::Type::Object, false));
      auto &request_poll_object = request_poll.get_object();
      if (has_json_object_field(request_poll_object, "type")) {
        TRY_RESULT(type, get_json_object_string_field(request_poll_object, "type"));
        if (type == "quiz") {
          force_quiz = true;
        } else if (type == "regular") {
          force_regular = true;
        }
      }
      return make_object<td_api::keyboardButton>(
          text, make_object<td_api::keyboardButtonTypeRequestPoll>(force_regular, force_quiz));
    }

    if (has_json_object_field(object, "web_app")) {
      TRY_RESULT(web_app, get_json_object_field(object, "web_app", JsonValue::Type::Object, false));
      auto &web_app_object = web_app.get_object();
      TRY_RESULT(url, get_json_object_string_field(web_app_object, "url", false));
      return make_object<td_api::keyboardButton>(text, make_object<td_api::keyboardButtonTypeWebApp>(url));
    }

    return make_object<td_api::keyboardButton>(text, nullptr);
  }
  if (button.type() == JsonValue::Type::String) {
    return make_object<td_api::keyboardButton>(button.get_string().str(), nullptr);
  }

  return Status::Error(400, "KeyboardButton must be a String or an Object");
}

td::Result<td_api::object_ptr<td_api::inlineKeyboardButton>> Client::get_inline_keyboard_button(JsonValue &button) {
  if (button.type() != JsonValue::Type::Object) {
    return Status::Error(400, "InlineKeyboardButton must be an Object");
  }

  auto &object = button.get_object();

  TRY_RESULT(text, get_json_object_string_field(object, "text", false));
  {
    TRY_RESULT(url, get_json_object_string_field(object, "url"));
    if (!url.empty()) {
      return make_object<td_api::inlineKeyboardButton>(text, make_object<td_api::inlineKeyboardButtonTypeUrl>(url));
    }
  }

  {
    TRY_RESULT(callback_data, get_json_object_string_field(object, "callback_data"));
    if (!callback_data.empty()) {
      return make_object<td_api::inlineKeyboardButton>(
          text, make_object<td_api::inlineKeyboardButtonTypeCallback>(callback_data));
    }
  }

  if (has_json_object_field(object, "callback_game")) {
    return make_object<td_api::inlineKeyboardButton>(text, make_object<td_api::inlineKeyboardButtonTypeCallbackGame>());
  }

  if (has_json_object_field(object, "pay")) {
    return make_object<td_api::inlineKeyboardButton>(text, make_object<td_api::inlineKeyboardButtonTypeBuy>());
  }

  if (has_json_object_field(object, "switch_inline_query")) {
    TRY_RESULT(switch_inline_query, get_json_object_string_field(object, "switch_inline_query", false));
    return make_object<td_api::inlineKeyboardButton>(
        text, make_object<td_api::inlineKeyboardButtonTypeSwitchInline>(switch_inline_query, false));
  }

  if (has_json_object_field(object, "switch_inline_query_current_chat")) {
    TRY_RESULT(switch_inline_query, get_json_object_string_field(object, "switch_inline_query_current_chat", false));
    return make_object<td_api::inlineKeyboardButton>(
        text, make_object<td_api::inlineKeyboardButtonTypeSwitchInline>(switch_inline_query, true));
  }

  if (has_json_object_field(object, "login_url")) {
    TRY_RESULT(login_url, get_json_object_field(object, "login_url", JsonValue::Type::Object, false));
    CHECK(login_url.type() == JsonValue::Type::Object);
    auto &login_url_object = login_url.get_object();
    TRY_RESULT(url, get_json_object_string_field(login_url_object, "url", false));
    TRY_RESULT(bot_username, get_json_object_string_field(login_url_object, "bot_username"));
    TRY_RESULT(request_write_access, get_json_object_bool_field(login_url_object, "request_write_access"));
    TRY_RESULT(forward_text, get_json_object_string_field(login_url_object, "forward_text"));

    int64 bot_user_id = 0;
    if (bot_username.empty()) {
      bot_user_id = my_id_;
    } else {
      if (bot_username[0] == '@') {
        bot_username = bot_username.substr(1);
      }
      if (bot_username.empty()) {
        return Status::Error(400, "LoginUrl bot username is invalid");
      }
      for (auto c : bot_username) {
        if (c != '_' && !td::is_alnum(c)) {
          return Status::Error(400, "LoginUrl bot username is invalid");
        }
      }
      if (cur_temp_bot_user_id_ >= 100000) {
        return Status::Error(400, "Too much different LoginUrl bot usernames");
      }
      auto &user_id = bot_user_ids_[bot_username];
      if (user_id == 0) {
        user_id = cur_temp_bot_user_id_++;
        user_id *= 1000;
      }
      if (user_id % 1000 == 0) {
        unresolved_bot_usernames_.insert(bot_username);
      }
      bot_user_id = user_id;
    }
    if (!request_write_access) {
      bot_user_id *= -1;
    }
    return make_object<td_api::inlineKeyboardButton>(
        text, make_object<td_api::inlineKeyboardButtonTypeLoginUrl>(url, bot_user_id, forward_text));
  }

  if (has_json_object_field(object, "web_app")) {
    TRY_RESULT(web_app, get_json_object_field(object, "web_app", JsonValue::Type::Object, false));
    auto &web_app_object = web_app.get_object();
    TRY_RESULT(url, get_json_object_string_field(web_app_object, "url", false));
    return make_object<td_api::inlineKeyboardButton>(text, make_object<td_api::inlineKeyboardButtonTypeWebApp>(url));
  }

  return Status::Error(400, "Text buttons are unallowed in the inline keyboard");
}

td::Result<td_api::object_ptr<td_api::ReplyMarkup>> Client::get_reply_markup(const Query *query) {
  auto reply_markup = query->arg("reply_markup");
  if (reply_markup.empty()) {
    return nullptr;
  }

  LOG(INFO) << "Parsing JSON object: " << reply_markup;
  auto r_value = json_decode(reply_markup);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return Status::Error(400, "Can't parse reply keyboard markup JSON object");
  }

  return get_reply_markup(r_value.move_as_ok());
}

td::Result<td_api::object_ptr<td_api::ReplyMarkup>> Client::get_reply_markup(JsonValue &&value) {
  td::vector<td::vector<object_ptr<td_api::keyboardButton>>> rows;
  td::vector<td::vector<object_ptr<td_api::inlineKeyboardButton>>> inline_rows;
  Slice input_field_placeholder;
  bool resize = false;
  bool one_time = false;
  bool remove = false;
  bool is_personal = false;
  bool force_reply = false;

  if (value.type() != JsonValue::Type::Object) {
    return Status::Error(400, "Object expected as reply markup");
  }
  for (auto &field_value : value.get_object()) {
    if (field_value.first == "keyboard") {
      auto keyboard = std::move(field_value.second);
      if (keyboard.type() != JsonValue::Type::Array) {
        return Status::Error(400, "Field \"keyboard\" of the ReplyKeyboardMarkup must be an Array");
      }
      for (auto &row : keyboard.get_array()) {
        td::vector<object_ptr<td_api::keyboardButton>> new_row;
        if (row.type() != JsonValue::Type::Array) {
          return Status::Error(400, "Field \"keyboard\" of the ReplyKeyboardMarkup must be an Array of Arrays");
        }
        for (auto &button : row.get_array()) {
          auto r_button = get_keyboard_button(button);
          if (r_button.is_error()) {
            return Status::Error(400, PSLICE() << "Can't parse keyboard button: " << r_button.error().message());
          }
          new_row.push_back(r_button.move_as_ok());
        }

        rows.push_back(std::move(new_row));
      }
    } else if (field_value.first == "inline_keyboard") {
      auto inline_keyboard = std::move(field_value.second);
      if (inline_keyboard.type() != JsonValue::Type::Array) {
        return Status::Error(400, "Field \"inline_keyboard\" of the InlineKeyboardMarkup must be an Array");
      }
      for (auto &inline_row : inline_keyboard.get_array()) {
        td::vector<object_ptr<td_api::inlineKeyboardButton>> new_inline_row;
        if (inline_row.type() != JsonValue::Type::Array) {
          return Status::Error(400, "Field \"inline_keyboard\" of the InlineKeyboardMarkup must be an Array of Arrays");
        }
        for (auto &button : inline_row.get_array()) {
          auto r_button = get_inline_keyboard_button(button);
          if (r_button.is_error()) {
            return Status::Error(400, PSLICE() << "Can't parse inline keyboard button: " << r_button.error().message());
          }
          new_inline_row.push_back(r_button.move_as_ok());
        }

        inline_rows.push_back(std::move(new_inline_row));
      }
    } else if (field_value.first == "resize_keyboard") {
      if (field_value.second.type() != JsonValue::Type::Boolean) {
        return Status::Error(400, "Field \"resize_keyboard\" of the ReplyKeyboardMarkup must be of the type Boolean");
      }
      resize = field_value.second.get_boolean();
    } else if (field_value.first == "one_time_keyboard") {
      if (field_value.second.type() != JsonValue::Type::Boolean) {
        return Status::Error(400, "Field \"one_time_keyboard\" of the ReplyKeyboardMarkup must be of the type Boolean");
      }
      one_time = field_value.second.get_boolean();
    } else if (field_value.first == "hide_keyboard" || field_value.first == "remove_keyboard") {
      if (field_value.second.type() != JsonValue::Type::Boolean) {
        return Status::Error(400, "Field \"remove_keyboard\" of the ReplyKeyboardRemove must be of the type Boolean");
      }
      remove = field_value.second.get_boolean();
    } else if (field_value.first == "personal_keyboard" || field_value.first == "selective") {
      if (field_value.second.type() != JsonValue::Type::Boolean) {
        return Status::Error(400, "Field \"selective\" of the reply markup must be of the type Boolean");
      }
      is_personal = field_value.second.get_boolean();
    } else if (field_value.first == "force_reply_keyboard" || field_value.first == "force_reply") {
      if (field_value.second.type() != JsonValue::Type::Boolean) {
        return Status::Error(400, "Field \"force_reply\" of the reply markup must be of the type Boolean");
      }
      force_reply = field_value.second.get_boolean();
    } else if (field_value.first == "input_field_placeholder") {
      if (field_value.second.type() != JsonValue::Type::String) {
        return Status::Error(400, "Field \"input_field_placeholder\" of the reply markup must be of the type String");
      }
      input_field_placeholder = field_value.second.get_string();
    }
  }

  object_ptr<td_api::ReplyMarkup> result;
  if (!rows.empty()) {
    result = make_object<td_api::replyMarkupShowKeyboard>(std::move(rows), resize, one_time, is_personal,
                                                          input_field_placeholder.str());
  } else if (!inline_rows.empty()) {
    result = make_object<td_api::replyMarkupInlineKeyboard>(std::move(inline_rows));
  } else if (remove) {
    result = make_object<td_api::replyMarkupRemoveKeyboard>(is_personal);
  } else if (force_reply) {
    result = make_object<td_api::replyMarkupForceReply>(is_personal, input_field_placeholder.str());
  }
  if (result == nullptr || result->get_id() != td_api::replyMarkupInlineKeyboard::ID) {
    unresolved_bot_usernames_.clear();
  }

  return std::move(result);
}

td::Result<td_api::object_ptr<td_api::labeledPricePart>> Client::get_labeled_price_part(JsonValue &value) {
  if (value.type() != JsonValue::Type::Object) {
    return Status::Error(400, "LabeledPrice must be an Object");
  }

  auto &object = value.get_object();

  TRY_RESULT(label, get_json_object_string_field(object, "label", false));
  if (label.empty()) {
    return Status::Error(400, "LabeledPrice label must be non-empty");
  }

  TRY_RESULT(amount, get_json_object_field(object, "amount", JsonValue::Type::Null, false));
  Slice number;
  if (amount.type() == JsonValue::Type::Number) {
    number = amount.get_number();
  } else if (amount.type() == JsonValue::Type::String) {
    number = amount.get_string();
  } else {
    return Status::Error(400, "Field \"amount\" must be of type Number or String");
  }
  auto parsed_amount = td::to_integer_safe<int64>(number);
  if (parsed_amount.is_error()) {
    return Status::Error(400, "Can't parse \"amount\" as Number");
  }
  return make_object<td_api::labeledPricePart>(label, parsed_amount.ok());
}

td::Result<td::vector<td_api::object_ptr<td_api::labeledPricePart>>> Client::get_labeled_price_parts(JsonValue &value) {
  if (value.type() != JsonValue::Type::Array) {
    return Status::Error(400, "Expected an Array of labeled prices");
  }

  td::vector<object_ptr<td_api::labeledPricePart>> prices;
  for (auto &price : value.get_array()) {
    auto r_labeled_price = get_labeled_price_part(price);
    if (r_labeled_price.is_error()) {
      return Status::Error(400, PSLICE() << "Can't parse labeled price: " << r_labeled_price.error().message());
    }
    prices.push_back(r_labeled_price.move_as_ok());
  }
  if (prices.empty()) {
    return Status::Error(400, "There must be at least one price");
  }

  return std::move(prices);
}

td::Result<td::vector<td::int64>> Client::get_suggested_tip_amounts(JsonValue &value) {
  if (value.type() != JsonValue::Type::Array) {
    return Status::Error(400, "Expected an Array of suggested tip amounts");
  }

  td::vector<int64> suggested_tip_amounts;
  for (auto &amount : value.get_array()) {
    Slice number;
    if (amount.type() == JsonValue::Type::Number) {
      number = amount.get_number();
    } else if (amount.type() == JsonValue::Type::String) {
      number = amount.get_string();
    } else {
      return Status::Error(400, "Suggested tip amount must be of type Number or String");
    }
    auto parsed_amount = td::to_integer_safe<int64>(number);
    if (parsed_amount.is_error()) {
      return Status::Error(400, "Can't parse suggested tip amount as Number");
    }
    suggested_tip_amounts.push_back(parsed_amount.ok());
  }
  return std::move(suggested_tip_amounts);
}

td::Result<td_api::object_ptr<td_api::shippingOption>> Client::get_shipping_option(JsonValue &option) {
  if (option.type() != JsonValue::Type::Object) {
    return Status::Error(400, "ShippingOption must be an Object");
  }

  auto &object = option.get_object();

  TRY_RESULT(id, get_json_object_string_field(object, "id", false));
  if (id.empty()) {
    return Status::Error(400, "ShippingOption identifier must be non-empty");
  }

  TRY_RESULT(title, get_json_object_string_field(object, "title", false));
  if (title.empty()) {
    return Status::Error(400, "ShippingOption title must be non-empty");
  }

  TRY_RESULT(prices_json, get_json_object_field(object, "prices", JsonValue::Type::Array, false));

  auto r_prices = get_labeled_price_parts(prices_json);
  if (r_prices.is_error()) {
    return Status::Error(400, PSLICE() << "Can't parse shipping option prices: " << r_prices.error().message());
  }

  return make_object<td_api::shippingOption>(id, title, r_prices.move_as_ok());
}

td::Result<td::vector<td_api::object_ptr<td_api::shippingOption>>> Client::get_shipping_options(const Query *query) {
  TRY_RESULT(shipping_options, get_required_string_arg(query, "shipping_options"));

  LOG(INFO) << "Parsing JSON object: " << shipping_options;
  auto r_value = json_decode(shipping_options);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return Status::Error(400, "Can't parse shipping options JSON object");
  }

  return get_shipping_options(r_value.move_as_ok());
}

td::Result<td::vector<td_api::object_ptr<td_api::shippingOption>>> Client::get_shipping_options(JsonValue &&value) {
  if (value.type() != JsonValue::Type::Array) {
    return Status::Error(400, "Expected an Array of shipping options");
  }

  td::vector<object_ptr<td_api::shippingOption>> options;
  for (auto &option : value.get_array()) {
    auto r_shipping_option = get_shipping_option(option);
    if (r_shipping_option.is_error()) {
      return Status::Error(400, PSLICE() << "Can't parse shipping option: " << r_shipping_option.error().message());
    }
    options.push_back(r_shipping_option.move_as_ok());
  }
  if (options.empty()) {
    return Status::Error(400, "There must be at least one shipping option");
  }

  return std::move(options);
}

td_api::object_ptr<td_api::ChatAction> Client::get_chat_action(const Query *query) {
  auto action = query->arg("action");
  td::to_lower_inplace(action);
  if (action == "cancel") {
    return make_object<td_api::chatActionCancel>();
  }
  if (action == "typing") {
    return make_object<td_api::chatActionTyping>();
  }
  if (action == "record_video") {
    return make_object<td_api::chatActionRecordingVideo>();
  }
  if (action == "upload_video") {
    return make_object<td_api::chatActionUploadingVideo>();
  }
  if (action == "record_audio" || action == "record_voice") {
    return make_object<td_api::chatActionRecordingVoiceNote>();
  }
  if (action == "upload_audio" || action == "upload_voice") {
    return make_object<td_api::chatActionUploadingVoiceNote>();
  }
  if (action == "upload_photo") {
    return make_object<td_api::chatActionUploadingPhoto>();
  }
  if (action == "upload_document") {
    return make_object<td_api::chatActionUploadingDocument>();
  }
  if (action == "choose_sticker") {
    return make_object<td_api::chatActionChoosingSticker>();
  }
  if (action == "pick_up_location" || action == "find_location") {
    return make_object<td_api::chatActionChoosingLocation>();
  }
  if (action == "record_video_note") {
    return make_object<td_api::chatActionRecordingVideoNote>();
  }
  if (action == "upload_video_note") {
    return make_object<td_api::chatActionUploadingVideoNote>();
  }
  return nullptr;
}

td_api::object_ptr<td_api::InputFile> Client::get_input_file(const Query *query, Slice field_name,
                                                             bool force_file) const {
  return get_input_file(query, field_name, query->arg(field_name), force_file);
}

td::string Client::get_local_file_path(Slice file_uri) {
  if (td::begins_with(file_uri, "/")) {
    file_uri.remove_prefix(td::begins_with(file_uri, "/localhost") ? 10 : 1);
  }
#if TD_PORT_WINDOWS
  if (td::begins_with(file_uri, "/")) {
    file_uri.remove_prefix(1);
  }
#endif
  td::string result(file_uri.size(), '\0');
  auto result_len = url_decode(file_uri, result, false);
  result.resize(result_len);
  return result;
}

td_api::object_ptr<td_api::InputFile> Client::get_input_file(const Query *query, Slice field_name, Slice file_id,
                                                             bool force_file) const {
  if (!file_id.empty()) {
    if (parameters_->local_mode_) {
      Slice file_protocol{"file:/"};
      if (td::begins_with(file_id, file_protocol)) {
        return make_object<td_api::inputFileLocal>(get_local_file_path(file_id.substr(file_protocol.size())));
      }
    }
    Slice attach_protocol{"attach://"};
    if (td::begins_with(file_id, attach_protocol)) {
      field_name = file_id.substr(attach_protocol.size());
    } else {
      if (!force_file) {
        return make_object<td_api::inputFileRemote>(file_id.str());
      }
    }
  }
  auto file = query->file(field_name);
  if (file != nullptr) {
    return make_object<td_api::inputFileLocal>(file->temp_file_name);
  }

  return nullptr;
}

td_api::object_ptr<td_api::inputThumbnail> Client::get_input_thumbnail(const Query *query, Slice field_name) const {
  auto input_file = get_input_file(query, field_name, true);
  if (input_file == nullptr) {
    return nullptr;
  }
  return make_object<td_api::inputThumbnail>(std::move(input_file), 0, 0);
}

td::Result<td_api::object_ptr<td_api::InputMessageContent>> Client::get_input_message_content(
    JsonValue &input_message_content, bool is_input_message_content_required) {
  CHECK(input_message_content.type() == JsonValue::Type::Object);
  auto &object = input_message_content.get_object();

  TRY_RESULT(message_text, get_json_object_string_field(object, "message_text"));

  if (!message_text.empty()) {
    TRY_RESULT(disable_web_page_preview, get_json_object_bool_field(object, "disable_web_page_preview"));
    TRY_RESULT(parse_mode, get_json_object_string_field(object, "parse_mode"));
    auto entities = get_json_object_field_force(object, "entities");
    TRY_RESULT(input_message_text, get_input_message_text(std::move(message_text), disable_web_page_preview,
                                                          std::move(parse_mode), std::move(entities)));
    return std::move(input_message_text);
  }

  if (has_json_object_field(object, "latitude") && has_json_object_field(object, "longitude")) {
    TRY_RESULT(latitude, get_json_object_double_field(object, "latitude", false));
    TRY_RESULT(longitude, get_json_object_double_field(object, "longitude", false));
    TRY_RESULT(horizontal_accuracy, get_json_object_double_field(object, "horizontal_accuracy"));
    TRY_RESULT(live_period, get_json_object_int_field(object, "live_period"));
    TRY_RESULT(heading, get_json_object_int_field(object, "heading"));
    TRY_RESULT(proximity_alert_radius, get_json_object_int_field(object, "proximity_alert_radius"));
    auto location = make_object<td_api::location>(latitude, longitude, horizontal_accuracy);

    if (has_json_object_field(object, "title") && has_json_object_field(object, "address")) {
      TRY_RESULT(title, get_json_object_string_field(object, "title", false));
      TRY_RESULT(address, get_json_object_string_field(object, "address", false));
      td::string provider;
      td::string venue_id;
      td::string venue_type;

      TRY_RESULT(google_place_id, get_json_object_string_field(object, "google_place_id"));
      TRY_RESULT(google_place_type, get_json_object_string_field(object, "google_place_type"));
      if (!google_place_id.empty() || !google_place_type.empty()) {
        provider = "gplaces";
        venue_id = std::move(google_place_id);
        venue_type = std::move(google_place_type);
      }
      TRY_RESULT(foursquare_id, get_json_object_string_field(object, "foursquare_id"));
      TRY_RESULT(foursquare_type, get_json_object_string_field(object, "foursquare_type"));
      if (!foursquare_id.empty() || !foursquare_type.empty()) {
        provider = "foursquare";
        venue_id = std::move(foursquare_id);
        venue_type = std::move(foursquare_type);
      }

      return make_object<td_api::inputMessageVenue>(
          make_object<td_api::venue>(std::move(location), title, address, provider, venue_id, venue_type));
    }

    return make_object<td_api::inputMessageLocation>(std::move(location), live_period, heading, proximity_alert_radius);
  }

  if (has_json_object_field(object, "phone_number")) {
    TRY_RESULT(phone_number, get_json_object_string_field(object, "phone_number", false));
    TRY_RESULT(first_name, get_json_object_string_field(object, "first_name", false));
    TRY_RESULT(last_name, get_json_object_string_field(object, "last_name"));
    TRY_RESULT(vcard, get_json_object_string_field(object, "vcard"));

    return make_object<td_api::inputMessageContact>(
        make_object<td_api::contact>(phone_number, first_name, last_name, vcard, 0));
  }

  if (has_json_object_field(object, "payload")) {
    TRY_RESULT(title, get_json_object_string_field(object, "title", false));
    TRY_RESULT(description, get_json_object_string_field(object, "description", false));
    TRY_RESULT(payload, get_json_object_string_field(object, "payload", false));
    if (!td::check_utf8(payload)) {
      return Status::Error(400, "InputInvoiceMessageContent payload must be encoded in UTF-8");
    }
    TRY_RESULT(provider_token, get_json_object_string_field(object, "provider_token", false));
    TRY_RESULT(currency, get_json_object_string_field(object, "currency", false));
    TRY_RESULT(prices_object, get_json_object_field(object, "prices", JsonValue::Type::Array, false));
    TRY_RESULT(prices, get_labeled_price_parts(prices_object));
    TRY_RESULT(provider_data, get_json_object_string_field(object, "provider_data"));
    TRY_RESULT(max_tip_amount, get_json_object_long_field(object, "max_tip_amount"));
    td::vector<int64> suggested_tip_amounts;
    TRY_RESULT(suggested_tip_amounts_array,
               get_json_object_field(object, "suggested_tip_amounts", JsonValue::Type::Array));
    if (suggested_tip_amounts_array.type() == JsonValue::Type::Array) {
      TRY_RESULT_ASSIGN(suggested_tip_amounts, get_suggested_tip_amounts(suggested_tip_amounts_array));
    }
    TRY_RESULT(photo_url, get_json_object_string_field(object, "photo_url"));
    TRY_RESULT(photo_size, get_json_object_int_field(object, "photo_size"));
    TRY_RESULT(photo_width, get_json_object_int_field(object, "photo_width"));
    TRY_RESULT(photo_height, get_json_object_int_field(object, "photo_height"));
    TRY_RESULT(need_name, get_json_object_bool_field(object, "need_name"));
    TRY_RESULT(need_phone_number, get_json_object_bool_field(object, "need_phone_number"));
    TRY_RESULT(need_email_address, get_json_object_bool_field(object, "need_email"));
    TRY_RESULT(need_shipping_address, get_json_object_bool_field(object, "need_shipping_address"));
    TRY_RESULT(send_phone_number_to_provider, get_json_object_bool_field(object, "send_phone_number_to_provider"));
    TRY_RESULT(send_email_address_to_provider, get_json_object_bool_field(object, "send_email_to_provider"));
    TRY_RESULT(is_flexible, get_json_object_bool_field(object, "is_flexible"));

    return make_object<td_api::inputMessageInvoice>(
        make_object<td_api::invoice>(currency, std::move(prices), max_tip_amount, std::move(suggested_tip_amounts),
                                     td::string(), false, need_name, need_phone_number, need_email_address,
                                     need_shipping_address, send_phone_number_to_provider,
                                     send_email_address_to_provider, is_flexible),
        title, description, photo_url, photo_size, photo_width, photo_height, payload, provider_token, provider_data,
        td::string());
  }

  if (is_input_message_content_required) {
    return Status::Error(400, "Input message content is not specified");
  }
  return nullptr;
}

td_api::object_ptr<td_api::messageSendOptions> Client::get_message_send_options(bool disable_notification,
                                                                                bool protect_content) {
  return make_object<td_api::messageSendOptions>(disable_notification, false, protect_content, nullptr);
}

td::Result<td::vector<td_api::object_ptr<td_api::InputInlineQueryResult>>> Client::get_inline_query_results(
    const Query *query) {
  auto results_encoded = query->arg("results");
  if (results_encoded.empty()) {
    return td::vector<object_ptr<td_api::InputInlineQueryResult>>();
  }

  LOG(INFO) << "Parsing JSON object: " << results_encoded;
  auto r_values = json_decode(results_encoded);
  if (r_values.is_error()) {
    return Status::Error(400,
                         PSLICE() << "Can't parse JSON encoded inline query results: " << r_values.error().message());
  }

  return get_inline_query_results(r_values.move_as_ok());
}

td::Result<td::vector<td_api::object_ptr<td_api::InputInlineQueryResult>>> Client::get_inline_query_results(
    td::JsonValue &&values) {
  if (values.type() == JsonValue::Type::Null) {
    return td::vector<object_ptr<td_api::InputInlineQueryResult>>();
  }
  if (values.type() != JsonValue::Type::Array) {
    return Status::Error(400, "Expected an Array of inline query results");
  }

  td::vector<object_ptr<td_api::InputInlineQueryResult>> inline_query_results;
  for (auto &value : values.get_array()) {
    auto r_inline_query_result = get_inline_query_result(std::move(value));
    if (r_inline_query_result.is_error()) {
      return Status::Error(400,
                           PSLICE() << "Can't parse inline query result: " << r_inline_query_result.error().message());
    }
    inline_query_results.push_back(r_inline_query_result.move_as_ok());
  }

  return std::move(inline_query_results);
}

td::Result<td_api::object_ptr<td_api::InputInlineQueryResult>> Client::get_inline_query_result(const Query *query) {
  auto result_encoded = query->arg("result");
  if (result_encoded.empty()) {
    return Status::Error(400, "Result isn't specified");
  }

  LOG(INFO) << "Parsing JSON object: " << result_encoded;
  auto r_value = json_decode(result_encoded);
  if (r_value.is_error()) {
    return Status::Error(400,
                         PSLICE() << "Can't parse JSON encoded web view query results " << r_value.error().message());
  }

  return get_inline_query_result(r_value.move_as_ok());
}

td::Result<td_api::object_ptr<td_api::InputInlineQueryResult>> Client::get_inline_query_result(td::JsonValue &&value) {
  if (value.type() != JsonValue::Type::Object) {
    return Status::Error(400, "Inline query result must be an object");
  }

  auto &object = value.get_object();

  TRY_RESULT(type, get_json_object_string_field(object, "type", false));
  td::to_lower_inplace(type);

  TRY_RESULT(id, get_json_object_string_field(object, "id", false));

  bool is_input_message_content_required = (type == "article");
  object_ptr<td_api::InputMessageContent> input_message_content;

  TRY_RESULT(input_message_content_obj,
             get_json_object_field(object, "input_message_content", JsonValue::Type::Object));
  if (input_message_content_obj.type() == JsonValue::Type::Null) {
    TRY_RESULT(message_text, get_json_object_string_field(object, "message_text", !is_input_message_content_required));
    TRY_RESULT(disable_web_page_preview, get_json_object_bool_field(object, "disable_web_page_preview"));
    TRY_RESULT(parse_mode, get_json_object_string_field(object, "parse_mode"));
    auto entities = get_json_object_field_force(object, "entities");

    if (is_input_message_content_required || !message_text.empty()) {
      TRY_RESULT(input_message_text, get_input_message_text(std::move(message_text), disable_web_page_preview,
                                                            std::move(parse_mode), std::move(entities)));
      input_message_content = std::move(input_message_text);
    }
  } else {
    TRY_RESULT(input_message_content_result,
               get_input_message_content(input_message_content_obj, is_input_message_content_required));
    input_message_content = std::move(input_message_content_result);
  }
  TRY_RESULT(input_caption, get_json_object_string_field(object, "caption"));
  TRY_RESULT(parse_mode, get_json_object_string_field(object, "parse_mode"));
  auto entities = get_json_object_field_force(object, "caption_entities");
  TRY_RESULT(caption, get_formatted_text(std::move(input_caption), std::move(parse_mode), std::move(entities)));

  TRY_RESULT(reply_markup_object, get_json_object_field(object, "reply_markup", JsonValue::Type::Object));
  object_ptr<td_api::ReplyMarkup> reply_markup;
  if (reply_markup_object.type() != JsonValue::Type::Null) {
    TRY_RESULT_ASSIGN(reply_markup, get_reply_markup(std::move(reply_markup_object)));
  }

  object_ptr<td_api::InputInlineQueryResult> result;
  if (type == "article") {
    TRY_RESULT(url, get_json_object_string_field(object, "url"));
    TRY_RESULT(hide_url, get_json_object_bool_field(object, "hide_url"));
    TRY_RESULT(title, get_json_object_string_field(object, "title", false));
    TRY_RESULT(description, get_json_object_string_field(object, "description"));
    TRY_RESULT(thumbnail_url, get_json_object_string_field(object, "thumb_url"));
    TRY_RESULT(thumbnail_width, get_json_object_int_field(object, "thumb_width"));
    TRY_RESULT(thumbnail_height, get_json_object_int_field(object, "thumb_height"));

    CHECK(input_message_content != nullptr);
    return make_object<td_api::inputInlineQueryResultArticle>(
        id, url, hide_url, title, description, thumbnail_url, thumbnail_width, thumbnail_height,
        std::move(reply_markup), std::move(input_message_content));
  }
  if (type == "audio") {
    TRY_RESULT(audio_url, get_json_object_string_field(object, "audio_url"));
    TRY_RESULT(audio_duration, get_json_object_int_field(object, "audio_duration"));
    TRY_RESULT(title, get_json_object_string_field(object, "title", audio_url.empty()));
    TRY_RESULT(performer, get_json_object_string_field(object, "performer"));
    if (audio_url.empty()) {
      TRY_RESULT_ASSIGN(audio_url, get_json_object_string_field(object, "audio_file_id", false));
    }

    if (input_message_content == nullptr) {
      input_message_content = make_object<td_api::inputMessageAudio>(nullptr, nullptr, audio_duration, title, performer,
                                                                     std::move(caption));
    }
    return make_object<td_api::inputInlineQueryResultAudio>(id, title, performer, audio_url, audio_duration,
                                                            std::move(reply_markup), std::move(input_message_content));
  }
  if (type == "contact") {
    TRY_RESULT(phone_number, get_json_object_string_field(object, "phone_number", false));
    TRY_RESULT(first_name, get_json_object_string_field(object, "first_name", false));
    TRY_RESULT(last_name, get_json_object_string_field(object, "last_name"));
    TRY_RESULT(vcard, get_json_object_string_field(object, "vcard"));
    TRY_RESULT(thumbnail_url, get_json_object_string_field(object, "thumb_url"));
    TRY_RESULT(thumbnail_width, get_json_object_int_field(object, "thumb_width"));
    TRY_RESULT(thumbnail_height, get_json_object_int_field(object, "thumb_height"));

    if (input_message_content == nullptr) {
      input_message_content = make_object<td_api::inputMessageContact>(
          make_object<td_api::contact>(phone_number, first_name, last_name, vcard, 0));
    }
    return make_object<td_api::inputInlineQueryResultContact>(
        id, make_object<td_api::contact>(phone_number, first_name, last_name, vcard, 0), thumbnail_url, thumbnail_width,
        thumbnail_height, std::move(reply_markup), std::move(input_message_content));
  }
  if (type == "document") {
    TRY_RESULT(title, get_json_object_string_field(object, "title", false));
    TRY_RESULT(description, get_json_object_string_field(object, "description"));
    TRY_RESULT(thumbnail_url, get_json_object_string_field(object, "thumb_url"));
    TRY_RESULT(document_url, get_json_object_string_field(object, "document_url"));
    TRY_RESULT(mime_type, get_json_object_string_field(object, "mime_type", document_url.empty()));
    TRY_RESULT(thumbnail_width, get_json_object_int_field(object, "thumb_width"));
    TRY_RESULT(thumbnail_height, get_json_object_int_field(object, "thumb_height"));
    if (document_url.empty()) {
      TRY_RESULT_ASSIGN(document_url, get_json_object_string_field(object, "document_file_id", false));
    }

    if (input_message_content == nullptr) {
      input_message_content = make_object<td_api::inputMessageDocument>(nullptr, nullptr, false, std::move(caption));
    }
    return make_object<td_api::inputInlineQueryResultDocument>(
        id, title, description, document_url, mime_type, thumbnail_url, thumbnail_width, thumbnail_height,
        std::move(reply_markup), std::move(input_message_content));
  }
  if (type == "game") {
    TRY_RESULT(game_short_name, get_json_object_string_field(object, "game_short_name", false));
    return make_object<td_api::inputInlineQueryResultGame>(id, game_short_name, std::move(reply_markup));
  }
  if (type == "gif") {
    TRY_RESULT(title, get_json_object_string_field(object, "title"));
    TRY_RESULT(gif_url, get_json_object_string_field(object, "gif_url"));
    TRY_RESULT(thumbnail_url, get_json_object_string_field(object, "thumb_url", gif_url.empty()));
    TRY_RESULT(thumbnail_mime_type, get_json_object_string_field(object, "thumb_mime_type", true));
    TRY_RESULT(gif_duration, get_json_object_int_field(object, "gif_duration"));
    TRY_RESULT(gif_width, get_json_object_int_field(object, "gif_width"));
    TRY_RESULT(gif_height, get_json_object_int_field(object, "gif_height"));
    if (gif_url.empty()) {
      TRY_RESULT_ASSIGN(gif_url, get_json_object_string_field(object, "gif_file_id", false));
    }

    if (input_message_content == nullptr) {
      input_message_content = make_object<td_api::inputMessageAnimation>(
          nullptr, nullptr, td::vector<int32>(), gif_duration, gif_width, gif_height, std::move(caption));
    }
    return make_object<td_api::inputInlineQueryResultAnimation>(
        id, title, thumbnail_url, thumbnail_mime_type, gif_url, "image/gif", gif_duration, gif_width, gif_height,
        std::move(reply_markup), std::move(input_message_content));
  }
  if (type == "location") {
    TRY_RESULT(latitude, get_json_object_double_field(object, "latitude", false));
    TRY_RESULT(longitude, get_json_object_double_field(object, "longitude", false));
    TRY_RESULT(horizontal_accuracy, get_json_object_double_field(object, "horizontal_accuracy"));
    TRY_RESULT(live_period, get_json_object_int_field(object, "live_period"));
    TRY_RESULT(heading, get_json_object_int_field(object, "heading"));
    TRY_RESULT(proximity_alert_radius, get_json_object_int_field(object, "proximity_alert_radius"));
    TRY_RESULT(title, get_json_object_string_field(object, "title", false));
    TRY_RESULT(thumbnail_url, get_json_object_string_field(object, "thumb_url"));
    TRY_RESULT(thumbnail_width, get_json_object_int_field(object, "thumb_width"));
    TRY_RESULT(thumbnail_height, get_json_object_int_field(object, "thumb_height"));

    if (input_message_content == nullptr) {
      auto location = make_object<td_api::location>(latitude, longitude, horizontal_accuracy);
      input_message_content =
          make_object<td_api::inputMessageLocation>(std::move(location), live_period, heading, proximity_alert_radius);
    }
    return make_object<td_api::inputInlineQueryResultLocation>(
        id, make_object<td_api::location>(latitude, longitude, horizontal_accuracy), live_period, title, thumbnail_url,
        thumbnail_width, thumbnail_height, std::move(reply_markup), std::move(input_message_content));
  }
  if (type == "mpeg4_gif") {
    TRY_RESULT(title, get_json_object_string_field(object, "title"));
    TRY_RESULT(mpeg4_url, get_json_object_string_field(object, "mpeg4_url"));
    TRY_RESULT(thumbnail_url, get_json_object_string_field(object, "thumb_url", mpeg4_url.empty()));
    TRY_RESULT(thumbnail_mime_type, get_json_object_string_field(object, "thumb_mime_type", true));
    TRY_RESULT(mpeg4_duration, get_json_object_int_field(object, "mpeg4_duration"));
    TRY_RESULT(mpeg4_width, get_json_object_int_field(object, "mpeg4_width"));
    TRY_RESULT(mpeg4_height, get_json_object_int_field(object, "mpeg4_height"));
    if (mpeg4_url.empty()) {
      TRY_RESULT_ASSIGN(mpeg4_url, get_json_object_string_field(object, "mpeg4_file_id", false));
    }

    if (input_message_content == nullptr) {
      input_message_content = make_object<td_api::inputMessageAnimation>(
          nullptr, nullptr, td::vector<int32>(), mpeg4_duration, mpeg4_width, mpeg4_height, std::move(caption));
    }
    return make_object<td_api::inputInlineQueryResultAnimation>(
        id, title, thumbnail_url, thumbnail_mime_type, mpeg4_url, "video/mp4", mpeg4_duration, mpeg4_width,
        mpeg4_height, std::move(reply_markup), std::move(input_message_content));
  }
  if (type == "photo") {
    TRY_RESULT(title, get_json_object_string_field(object, "title"));
    TRY_RESULT(description, get_json_object_string_field(object, "description"));
    TRY_RESULT(photo_url, get_json_object_string_field(object, "photo_url"));
    TRY_RESULT(thumbnail_url, get_json_object_string_field(object, "thumb_url", photo_url.empty()));
    TRY_RESULT(photo_width, get_json_object_int_field(object, "photo_width"));
    TRY_RESULT(photo_height, get_json_object_int_field(object, "photo_height"));
    if (photo_url.empty()) {
      TRY_RESULT_ASSIGN(photo_url, get_json_object_string_field(object, "photo_file_id", false));
    }

    if (input_message_content == nullptr) {
      input_message_content =
          make_object<td_api::inputMessagePhoto>(nullptr, nullptr, td::vector<int32>(), 0, 0, std::move(caption), 0);
    }
    return make_object<td_api::inputInlineQueryResultPhoto>(id, title, description, thumbnail_url, photo_url,
                                                            photo_width, photo_height, std::move(reply_markup),
                                                            std::move(input_message_content));
  }
  if (type == "sticker") {
    TRY_RESULT(sticker_file_id, get_json_object_string_field(object, "sticker_file_id", false));

    if (input_message_content == nullptr) {
      input_message_content = make_object<td_api::inputMessageSticker>(nullptr, nullptr, 0, 0, td::string());
    }
    return make_object<td_api::inputInlineQueryResultSticker>(id, "", sticker_file_id, 0, 0, std::move(reply_markup),
                                                              std::move(input_message_content));
  }
  if (type == "venue") {
    TRY_RESULT(latitude, get_json_object_double_field(object, "latitude", false));
    TRY_RESULT(longitude, get_json_object_double_field(object, "longitude", false));
    TRY_RESULT(horizontal_accuracy, get_json_object_double_field(object, "horizontal_accuracy"));
    TRY_RESULT(title, get_json_object_string_field(object, "title", false));
    TRY_RESULT(address, get_json_object_string_field(object, "address", false));
    TRY_RESULT(foursquare_id, get_json_object_string_field(object, "foursquare_id"));
    TRY_RESULT(foursquare_type, get_json_object_string_field(object, "foursquare_type"));
    TRY_RESULT(google_place_id, get_json_object_string_field(object, "google_place_id"));
    TRY_RESULT(google_place_type, get_json_object_string_field(object, "google_place_type"));
    TRY_RESULT(thumbnail_url, get_json_object_string_field(object, "thumb_url"));
    TRY_RESULT(thumbnail_width, get_json_object_int_field(object, "thumb_width"));
    TRY_RESULT(thumbnail_height, get_json_object_int_field(object, "thumb_height"));

    td::string provider;
    td::string venue_id;
    td::string venue_type;
    if (!google_place_id.empty() || !google_place_type.empty()) {
      provider = "gplaces";
      venue_id = std::move(google_place_id);
      venue_type = std::move(google_place_type);
    }
    if (!foursquare_id.empty() || !foursquare_type.empty()) {
      provider = "foursquare";
      venue_id = std::move(foursquare_id);
      venue_type = std::move(foursquare_type);
    }

    if (input_message_content == nullptr) {
      input_message_content = make_object<td_api::inputMessageVenue>(
          make_object<td_api::venue>(make_object<td_api::location>(latitude, longitude, horizontal_accuracy), title,
                                     address, provider, venue_id, venue_type));
    }
    return make_object<td_api::inputInlineQueryResultVenue>(
        id,
        make_object<td_api::venue>(make_object<td_api::location>(latitude, longitude, horizontal_accuracy), title,
                                   address, provider, venue_id, venue_type),
        thumbnail_url, thumbnail_width, thumbnail_height, std::move(reply_markup), std::move(input_message_content));
  }
  if (type == "video") {
    TRY_RESULT(title, get_json_object_string_field(object, "title", false));
    TRY_RESULT(description, get_json_object_string_field(object, "description"));
    TRY_RESULT(video_url, get_json_object_string_field(object, "video_url"));
    TRY_RESULT(thumbnail_url, get_json_object_string_field(object, "thumb_url", video_url.empty()));
    TRY_RESULT(mime_type, get_json_object_string_field(object, "mime_type", video_url.empty()));
    TRY_RESULT(video_width, get_json_object_int_field(object, "video_width"));
    TRY_RESULT(video_height, get_json_object_int_field(object, "video_height"));
    TRY_RESULT(video_duration, get_json_object_int_field(object, "video_duration"));
    if (video_url.empty()) {
      TRY_RESULT_ASSIGN(video_url, get_json_object_string_field(object, "video_file_id", false));
    }

    if (input_message_content == nullptr) {
      input_message_content =
          make_object<td_api::inputMessageVideo>(nullptr, nullptr, td::vector<int32>(), video_duration, video_width,
                                                 video_height, false, std::move(caption), 0);
    }
    return make_object<td_api::inputInlineQueryResultVideo>(id, title, description, thumbnail_url, video_url, mime_type,
                                                            video_width, video_height, video_duration,
                                                            std::move(reply_markup), std::move(input_message_content));
  }
  if (type == "voice") {
    TRY_RESULT(title, get_json_object_string_field(object, "title", false));
    TRY_RESULT(voice_note_url, get_json_object_string_field(object, "voice_url"));
    TRY_RESULT(voice_note_duration, get_json_object_int_field(object, "voice_duration"));
    if (voice_note_url.empty()) {
      TRY_RESULT_ASSIGN(voice_note_url, get_json_object_string_field(object, "voice_file_id", false));
    }

    if (input_message_content == nullptr) {
      input_message_content = make_object<td_api::inputMessageVoiceNote>(nullptr, voice_note_duration,
                                                                         "" /* waveform */, std::move(caption));
    }
    return make_object<td_api::inputInlineQueryResultVoiceNote>(
        id, title, voice_note_url, voice_note_duration, std::move(reply_markup), std::move(input_message_content));
  }

  return Status::Error(400, PSLICE() << "type \"" << type << "\" is unsupported for the inline query result");
}

td::Result<Client::BotCommandScope> Client::get_bot_command_scope(JsonValue &&value) {
  if (value.type() != JsonValue::Type::Object) {
    return Status::Error(400, "BotCommandScope must be an Object");
  }

  auto &object = value.get_object();

  TRY_RESULT(type, get_json_object_string_field(object, "type", false));
  if (type == "default") {
    return BotCommandScope(make_object<td_api::botCommandScopeDefault>());
  }
  if (type == "all_private_chats") {
    return BotCommandScope(make_object<td_api::botCommandScopeAllPrivateChats>());
  }
  if (type == "all_group_chats") {
    return BotCommandScope(make_object<td_api::botCommandScopeAllGroupChats>());
  }
  if (type == "all_chat_administrators") {
    return BotCommandScope(make_object<td_api::botCommandScopeAllChatAdministrators>());
  }
  if (type != "chat" && type != "chat_administrators" && type != "chat_member") {
    return Status::Error(400, "Unsupported type specified");
  }

  TRY_RESULT(chat_id, get_json_object_string_field(object, "chat_id", false));
  if (chat_id.empty()) {
    return Status::Error(400, "Empty chat_id specified");
  }
  if (type == "chat") {
    return BotCommandScope(make_object<td_api::botCommandScopeChat>(0), std::move(chat_id));
  }
  if (type == "chat_administrators") {
    return BotCommandScope(make_object<td_api::botCommandScopeChatAdministrators>(0), std::move(chat_id));
  }

  TRY_RESULT(user_id, get_json_object_long_field(object, "user_id", false));
  if (user_id <= 0) {
    return Status::Error(400, "Invalid user_id specified");
  }
  CHECK(type == "chat_member");
  return BotCommandScope(make_object<td_api::botCommandScopeChatMember>(0, user_id), std::move(chat_id), user_id);
}

td::Result<Client::BotCommandScope> Client::get_bot_command_scope(const Query *query) {
  auto scope = query->arg("scope");
  if (scope.empty()) {
    return BotCommandScope(make_object<td_api::botCommandScopeDefault>());
  }

  LOG(INFO) << "Parsing JSON object: " << scope;
  auto r_value = json_decode(scope);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return Status::Error(400, "Can't parse BotCommandScope JSON object");
  }

  auto r_scope = get_bot_command_scope(r_value.move_as_ok());
  if (r_scope.is_error()) {
    return Status::Error(400, PSLICE() << "Can't parse BotCommandScope: " << r_scope.error().message());
  }
  return r_scope.move_as_ok();
}

td::Result<td_api::object_ptr<td_api::botCommand>> Client::get_bot_command(JsonValue &&value) {
  if (value.type() != JsonValue::Type::Object) {
    return Status::Error(400, "expected an Object");
  }

  auto &object = value.get_object();

  TRY_RESULT(command, get_json_object_string_field(object, "command", false));
  TRY_RESULT(description, get_json_object_string_field(object, "description", false));

  return make_object<td_api::botCommand>(command, description);
}

td::Result<td::vector<td_api::object_ptr<td_api::botCommand>>> Client::get_bot_commands(const Query *query) {
  auto commands = query->arg("commands");
  if (commands.empty()) {
    return td::vector<object_ptr<td_api::botCommand>>();
  }
  LOG(INFO) << "Parsing JSON object: " << commands;
  auto r_value = json_decode(commands);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return Status::Error(400, "Can't parse commands JSON object");
  }

  auto value = r_value.move_as_ok();
  if (value.type() != JsonValue::Type::Array) {
    return Status::Error(400, "Expected an Array of BotCommand");
  }

  td::vector<object_ptr<td_api::botCommand>> bot_commands;
  for (auto &command : value.get_array()) {
    auto r_bot_command = get_bot_command(std::move(command));
    if (r_bot_command.is_error()) {
      return Status::Error(400, PSLICE() << "Can't parse BotCommand: " << r_bot_command.error().message());
    }
    bot_commands.push_back(r_bot_command.move_as_ok());
  }
  return std::move(bot_commands);
}

td::Result<td_api::object_ptr<td_api::botMenuButton>> Client::get_bot_menu_button(JsonValue &&value) {
  if (value.type() != JsonValue::Type::Object) {
    return Status::Error(400, "MenuButton must be an Object");
  }

  auto &object = value.get_object();

  TRY_RESULT(type, get_json_object_string_field(object, "type", false));
  if (type == "default") {
    return td_api::make_object<td_api::botMenuButton>("", "default");
  }
  if (type == "commands") {
    return nullptr;
  }
  if (type == "web_app") {
    TRY_RESULT(text, get_json_object_string_field(object, "text", false));
    TRY_RESULT(web_app, get_json_object_field(object, "web_app", JsonValue::Type::Object, false));
    auto &web_app_object = web_app.get_object();
    TRY_RESULT(url, get_json_object_string_field(web_app_object, "url", false));
    return td_api::make_object<td_api::botMenuButton>(text, url);
  }

  return Status::Error(400, "MenuButton has unsupported type");
}

td::Result<td_api::object_ptr<td_api::botMenuButton>> Client::get_bot_menu_button(const Query *query) {
  auto menu_button = query->arg("menu_button");
  if (menu_button.empty()) {
    return td_api::make_object<td_api::botMenuButton>("", "default");
  }

  LOG(INFO) << "Parsing JSON object: " << menu_button;
  auto r_value = json_decode(menu_button);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return Status::Error(400, "Can't parse menu button JSON object");
  }

  auto r_menu_button = get_bot_menu_button(r_value.move_as_ok());
  if (r_menu_button.is_error()) {
    return Status::Error(400, PSLICE() << "Can't parse menu button: " << r_menu_button.error().message());
  }
  return r_menu_button.move_as_ok();
}

td::Result<td_api::object_ptr<td_api::chatAdministratorRights>> Client::get_chat_administrator_rights(
    JsonValue &&value) {
  if (value.type() != JsonValue::Type::Object) {
    return Status::Error(400, "ChatAdministratorRights must be an Object");
  }

  auto &object = value.get_object();
  TRY_RESULT(can_manage_chat, get_json_object_bool_field(object, "can_manage_chat"));
  TRY_RESULT(can_change_info, get_json_object_bool_field(object, "can_change_info"));
  TRY_RESULT(can_post_messages, get_json_object_bool_field(object, "can_post_messages"));
  TRY_RESULT(can_edit_messages, get_json_object_bool_field(object, "can_edit_messages"));
  TRY_RESULT(can_delete_messages, get_json_object_bool_field(object, "can_delete_messages"));
  TRY_RESULT(can_invite_users, get_json_object_bool_field(object, "can_invite_users"));
  TRY_RESULT(can_restrict_members, get_json_object_bool_field(object, "can_restrict_members"));
  TRY_RESULT(can_pin_messages, get_json_object_bool_field(object, "can_pin_messages"));
  TRY_RESULT(can_promote_members, get_json_object_bool_field(object, "can_promote_members"));
  TRY_RESULT(can_manage_video_chats, get_json_object_bool_field(object, "can_manage_video_chats"));
  TRY_RESULT(is_anonymous, get_json_object_bool_field(object, "is_anonymous"));
  return make_object<td_api::chatAdministratorRights>(
      can_manage_chat, can_change_info, can_post_messages, can_edit_messages, can_delete_messages, can_invite_users,
      can_restrict_members, can_pin_messages, can_promote_members, can_manage_video_chats, is_anonymous);
}

td::Result<td_api::object_ptr<td_api::chatAdministratorRights>> Client::get_chat_administrator_rights(
    const Query *query) {
  auto rights = query->arg("rights");
  if (rights.empty()) {
    return nullptr;
  }

  LOG(INFO) << "Parsing JSON object: " << rights;
  auto r_value = json_decode(rights);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return Status::Error(400, "Can't parse ChatAdministratorRights JSON object");
  }

  auto r_rights = get_chat_administrator_rights(r_value.move_as_ok());
  if (r_rights.is_error()) {
    return Status::Error(400, PSLICE() << "Can't parse ChatAdministratorRights: " << r_rights.error().message());
  }
  return r_rights.move_as_ok();
}

td::Result<td_api::object_ptr<td_api::maskPosition>> Client::get_mask_position(JsonValue &&value) {
  if (value.type() != JsonValue::Type::Object) {
    return Status::Error(400, "MaskPosition must be an Object");
  }

  auto &object = value.get_object();

  TRY_RESULT(point_str, get_json_object_string_field(object, "point", false));
  point_str = td::trim(td::to_lower(point_str));
  int32 point;
  for (point = 0; point < MASK_POINTS_SIZE; point++) {
    if (MASK_POINTS[point] == point_str) {
      break;
    }
  }
  if (point == MASK_POINTS_SIZE) {
    return Status::Error(400, "Wrong point specified in MaskPosition");
  }

  TRY_RESULT(x_shift, get_json_object_double_field(object, "x_shift", false));
  TRY_RESULT(y_shift, get_json_object_double_field(object, "y_shift", false));
  TRY_RESULT(scale, get_json_object_double_field(object, "scale", false));

  return make_object<td_api::maskPosition>(mask_index_to_point(point), x_shift, y_shift, scale);
}

td::int32 Client::mask_point_to_index(const object_ptr<td_api::MaskPoint> &mask_point) {
  CHECK(mask_point != nullptr);
  switch (mask_point->get_id()) {
    case td_api::maskPointForehead::ID:
      return 0;
    case td_api::maskPointEyes::ID:
      return 1;
    case td_api::maskPointMouth::ID:
      return 2;
    case td_api::maskPointChin::ID:
      return 3;
    default:
      UNREACHABLE();
      return -1;
  }
}

td_api::object_ptr<td_api::MaskPoint> Client::mask_index_to_point(int32 index) {
  switch (index) {
    case 0:
      return make_object<td_api::maskPointForehead>();
    case 1:
      return make_object<td_api::maskPointEyes>();
    case 2:
      return make_object<td_api::maskPointMouth>();
    case 3:
      return make_object<td_api::maskPointChin>();
    default:
      UNREACHABLE();
      return nullptr;
  }
}

td::Result<td_api::object_ptr<td_api::maskPosition>> Client::get_mask_position(const Query *query, Slice field_name) {
  auto mask_position = query->arg(field_name);
  if (mask_position.empty()) {
    return nullptr;
  }

  LOG(INFO) << "Parsing JSON object: " << mask_position;
  auto r_value = json_decode(mask_position);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return Status::Error(400, "Can't parse mask position JSON object");
  }

  auto r_mask_position = get_mask_position(r_value.move_as_ok());
  if (r_mask_position.is_error()) {
    return Status::Error(400, PSLICE() << "Can't parse mask position: " << r_mask_position.error().message());
  }
  return r_mask_position.move_as_ok();
}

td::Result<td::vector<td_api::object_ptr<td_api::inputSticker>>> Client::get_input_stickers(const Query *query,
                                                                                            bool is_masks) const {
  auto emojis = query->arg("emojis");

  auto sticker = get_input_file(query, "png_sticker");
  object_ptr<td_api::StickerType> sticker_type;
  if (sticker != nullptr) {
    if (is_masks) {
      TRY_RESULT(mask_position, get_mask_position(query, "mask_position"));
      sticker_type = make_object<td_api::stickerTypeMask>(std::move(mask_position));
    } else {
      sticker_type = make_object<td_api::stickerTypeStatic>();
    }
  } else {
    sticker = get_input_file(query, "tgs_sticker", true);
    if (sticker != nullptr) {
      sticker_type = make_object<td_api::stickerTypeAnimated>();
    } else {
      sticker = get_input_file(query, "webm_sticker", true);
      if (sticker != nullptr) {
        sticker_type = make_object<td_api::stickerTypeVideo>();
      } else {
        if (!query->arg("tgs_sticker").empty()) {
          return Status::Error(400, "Bad Request: animated sticker must be uploaded as an InputFile");
        }
        if (!query->arg("webm_sticker").empty()) {
          return Status::Error(400, "Bad Request: video sticker must be uploaded as an InputFile");
        }
        return Status::Error(400, "Bad Request: there is no sticker file in the request");
      }
    }
  }

  td::vector<object_ptr<td_api::inputSticker>> stickers;
  stickers.push_back(make_object<td_api::inputSticker>(std::move(sticker), emojis.str(), std::move(sticker_type)));
  return std::move(stickers);
}

td::Result<td::string> Client::get_passport_element_hash(Slice encoded_hash) {
  if (!td::is_base64(encoded_hash)) {
    return Status::Error(400, "hash isn't a valid base64-encoded string");
  }
  return td::base64_decode(encoded_hash).move_as_ok();
}

td::Result<td_api::object_ptr<td_api::InputPassportElementErrorSource>> Client::get_passport_element_error_source(
    td::JsonObject &object) {
  TRY_RESULT(source, get_json_object_string_field(object, "source"));

  if (source.empty() || source == "unspecified") {
    TRY_RESULT(element_hash, get_json_object_string_field(object, "element_hash", false));
    TRY_RESULT(hash, get_passport_element_hash(element_hash));

    return make_object<td_api::inputPassportElementErrorSourceUnspecified>(hash);
  }
  if (source == "data") {
    TRY_RESULT(data_hash, get_json_object_string_field(object, "data_hash", false));
    TRY_RESULT(hash, get_passport_element_hash(data_hash));

    TRY_RESULT(field_name, get_json_object_string_field(object, "field_name", false));
    return make_object<td_api::inputPassportElementErrorSourceDataField>(field_name, hash);
  }
  if (source == "file" || source == "selfie" || source == "translation_file" || source == "front_side" ||
      source == "reverse_side") {
    TRY_RESULT(file_hash, get_json_object_string_field(object, "file_hash", false));
    TRY_RESULT(hash, get_passport_element_hash(file_hash));

    if (source == "front_side") {
      return make_object<td_api::inputPassportElementErrorSourceFrontSide>(hash);
    }
    if (source == "reverse_side") {
      return make_object<td_api::inputPassportElementErrorSourceReverseSide>(hash);
    }
    if (source == "selfie") {
      return make_object<td_api::inputPassportElementErrorSourceSelfie>(hash);
    }
    if (source == "translation_file") {
      return make_object<td_api::inputPassportElementErrorSourceTranslationFile>(hash);
    }
    if (source == "file") {
      return make_object<td_api::inputPassportElementErrorSourceFile>(hash);
    }
    UNREACHABLE();
  }
  if (source == "files" || source == "translation_files") {
    td::vector<td::string> input_hashes;
    TRY_RESULT(file_hashes, get_json_object_field(object, "file_hashes", JsonValue::Type::Array, false));
    for (auto &input_hash : file_hashes.get_array()) {
      if (input_hash.type() != JsonValue::Type::String) {
        return Status::Error(400, "hash must be a string");
      }
      TRY_RESULT(hash, get_passport_element_hash(input_hash.get_string()));
      input_hashes.push_back(std::move(hash));
    }
    if (source == "files") {
      return make_object<td_api::inputPassportElementErrorSourceFiles>(std::move(input_hashes));
    }
    if (source == "translation_files") {
      return make_object<td_api::inputPassportElementErrorSourceTranslationFiles>(std::move(input_hashes));
    }
    UNREACHABLE();
  }
  return Status::Error(400, "wrong source specified");
}

td::Result<td_api::object_ptr<td_api::inputPassportElementError>> Client::get_passport_element_error(
    JsonValue &&value) {
  if (value.type() != JsonValue::Type::Object) {
    return Status::Error(400, "expected an Object");
  }

  auto &object = value.get_object();

  TRY_RESULT(input_type, get_json_object_string_field(object, "type", false));
  auto type = get_passport_element_type(input_type);
  if (type == nullptr) {
    return Status::Error(400, "wrong Telegram Passport element type specified");
  }
  TRY_RESULT(message, get_json_object_string_field(object, "message", false));
  TRY_RESULT(source, get_passport_element_error_source(object));

  return make_object<td_api::inputPassportElementError>(std::move(type), message, std::move(source));
}

td::Result<td::vector<td_api::object_ptr<td_api::inputPassportElementError>>> Client::get_passport_element_errors(
    const Query *query) {
  auto input_errors = query->arg("errors");
  LOG(INFO) << "Parsing JSON object: " << input_errors;
  auto r_value = json_decode(input_errors);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return Status::Error(400, "Can't parse errors JSON object");
  }

  auto value = r_value.move_as_ok();
  if (value.type() != JsonValue::Type::Array) {
    return Status::Error(400, "Expected an Array of PassportElementError");
  }

  td::vector<object_ptr<td_api::inputPassportElementError>> errors;
  for (auto &input_error : value.get_array()) {
    auto r_error = get_passport_element_error(std::move(input_error));
    if (r_error.is_error()) {
      return Status::Error(400, PSLICE() << "Can't parse PassportElementError: " << r_error.error().message());
    }
    errors.push_back(r_error.move_as_ok());
  }
  return std::move(errors);
}

JsonValue Client::get_input_entities(const Query *query, Slice field_name) {
  auto entities = query->arg(field_name);
  if (!entities.empty()) {
    auto r_value = json_decode(entities);
    if (r_value.is_ok()) {
      return r_value.move_as_ok();
    }

    LOG(INFO) << "Can't parse entities JSON object: " << r_value.error();
  }

  return JsonValue();
}

td::Result<td_api::object_ptr<td_api::formattedText>> Client::get_caption(const Query *query) {
  return get_formatted_text(query->arg("caption").str(), query->arg("parse_mode").str(),
                            get_input_entities(query, "caption_entities"));
}

td::Result<td_api::object_ptr<td_api::TextEntityType>> Client::get_text_entity_type(td::JsonObject &object) {
  TRY_RESULT(type, get_json_object_string_field(object, "type", false));
  if (type.empty()) {
    return Status::Error("Type is not specified");
  }

  if (type == "bold") {
    return make_object<td_api::textEntityTypeBold>();
  }
  if (type == "italic") {
    return make_object<td_api::textEntityTypeItalic>();
  }
  if (type == "underline") {
    return make_object<td_api::textEntityTypeUnderline>();
  }
  if (type == "strikethrough") {
    return make_object<td_api::textEntityTypeStrikethrough>();
  }
  if (type == "spoiler") {
    return make_object<td_api::textEntityTypeSpoiler>();
  }
  if (type == "code") {
    return make_object<td_api::textEntityTypeCode>();
  }
  if (type == "pre") {
    TRY_RESULT(language, get_json_object_string_field(object, "language"));
    if (language.empty()) {
      return make_object<td_api::textEntityTypePre>();
    }
    return make_object<td_api::textEntityTypePreCode>(language);
  }
  if (type == "text_link") {
    TRY_RESULT(url, get_json_object_string_field(object, "url", false));
    return make_object<td_api::textEntityTypeTextUrl>(url);
  }
  if (type == "text_mention") {
    TRY_RESULT(user, get_json_object_field(object, "user", JsonValue::Type::Object, false));
    CHECK(user.type() == JsonValue::Type::Object);
    TRY_RESULT(user_id, get_json_object_long_field(user.get_object(), "id", false));
    return make_object<td_api::textEntityTypeMentionName>(user_id);
  }
  if (type == "mention" || type == "hashtag" || type == "cashtag" || type == "bot_command" || type == "url" ||
      type == "email" || type == "phone_number" || type == "bank_card_number") {
    return nullptr;
  }

  return Status::Error("Unsupported type specified");
}

td::Result<td_api::object_ptr<td_api::textEntity>> Client::get_text_entity(JsonValue &&value) {
  if (value.type() != JsonValue::Type::Object) {
    return Status::Error(400, "expected an Object");
  }

  auto &object = value.get_object();
  TRY_RESULT(offset, get_json_object_int_field(object, "offset", false));
  TRY_RESULT(length, get_json_object_int_field(object, "length", false));
  TRY_RESULT(type, get_text_entity_type(object));

  if (type == nullptr) {
    return nullptr;
  }

  return make_object<td_api::textEntity>(offset, length, std::move(type));
}

td::Result<td_api::object_ptr<td_api::formattedText>> Client::get_formatted_text(td::string text, td::string parse_mode,
                                                                                 JsonValue &&input_entities) {
  td::to_lower_inplace(parse_mode);
  if (!text.empty() && !parse_mode.empty() && parse_mode != "none") {
    object_ptr<td_api::TextParseMode> text_parse_mode;
    if (parse_mode == "markdown") {
      text_parse_mode = make_object<td_api::textParseModeMarkdown>(1);
    } else if (parse_mode == "markdownv2") {
      text_parse_mode = make_object<td_api::textParseModeMarkdown>(2);
    } else if (parse_mode == "html") {
      text_parse_mode = make_object<td_api::textParseModeHTML>();
    } else {
      return Status::Error(400, "Unsupported parse_mode");
    }

    auto parsed_text = execute(make_object<td_api::parseTextEntities>(text, std::move(text_parse_mode)));
    if (parsed_text->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(parsed_text);
      return Status::Error(error->code_, error->message_);
    }

    CHECK(parsed_text->get_id() == td_api::formattedText::ID);
    return move_object_as<td_api::formattedText>(parsed_text);
  }

  td::vector<object_ptr<td_api::textEntity>> entities;
  if (input_entities.type() == JsonValue::Type::Array) {
    for (auto &input_entity : input_entities.get_array()) {
      auto r_entity = get_text_entity(std::move(input_entity));
      if (r_entity.is_error()) {
        return Status::Error(400, PSLICE() << "Can't parse MessageEntity: " << r_entity.error().message());
      }
      if (r_entity.ok() == nullptr) {
        continue;
      }
      entities.push_back(r_entity.move_as_ok());
    }
  }

  return make_object<td_api::formattedText>(text, std::move(entities));
}

td::Result<td_api::object_ptr<td_api::inputMessageText>> Client::get_input_message_text(const Query *query) {
  return get_input_message_text(query->arg("text").str(), to_bool(query->arg("disable_web_page_preview")),
                                query->arg("parse_mode").str(), get_input_entities(query, "entities"));
}

td::Result<td_api::object_ptr<td_api::inputMessageText>> Client::get_input_message_text(td::string text,
                                                                                        bool disable_web_page_preview,
                                                                                        td::string parse_mode,
                                                                                        JsonValue &&input_entities) {
  if (text.empty()) {
    return Status::Error(400, "Message text is empty");
  }

  TRY_RESULT(formatted_text, get_formatted_text(std::move(text), std::move(parse_mode), std::move(input_entities)));

  return make_object<td_api::inputMessageText>(std::move(formatted_text), disable_web_page_preview, false);
}

td::Result<td_api::object_ptr<td_api::location>> Client::get_location(const Query *query) {
  auto latitude = trim(query->arg("latitude"));
  if (latitude.empty()) {
    return Status::Error(400, "Bad Request: latitude is empty");
  }
  auto longitude = trim(query->arg("longitude"));
  if (longitude.empty()) {
    return Status::Error(400, "Bad Request: longitude is empty");
  }
  auto horizontal_accuracy = trim(query->arg("horizontal_accuracy"));

  return make_object<td_api::location>(td::to_double(latitude), td::to_double(longitude),
                                       td::to_double(horizontal_accuracy));
}

td::Result<td_api::object_ptr<td_api::chatPermissions>> Client::get_chat_permissions(const Query *query,
                                                                                     bool &allow_legacy) {
  auto can_send_messages = false;
  auto can_send_media_messages = false;
  auto can_send_polls = false;
  auto can_send_other_messages = false;
  auto can_add_web_page_previews = false;
  auto can_change_info = false;
  auto can_invite_users = false;
  auto can_pin_messages = false;

  if (query->has_arg("permissions")) {
    allow_legacy = false;

    auto r_value = json_decode(query->arg("permissions"));
    if (r_value.is_error()) {
      LOG(INFO) << "Can't parse JSON object: " << r_value.error();
      return Status::Error(400, "Can't parse permissions JSON object");
    }

    auto value = r_value.move_as_ok();
    if (value.type() != JsonValue::Type::Object) {
      return Status::Error(400, "Object expected as permissions");
    }
    auto &object = value.get_object();

    auto status = [&] {
      TRY_RESULT_ASSIGN(can_send_messages, get_json_object_bool_field(object, "can_send_messages"));
      TRY_RESULT_ASSIGN(can_send_media_messages, get_json_object_bool_field(object, "can_send_media_messages"));
      TRY_RESULT_ASSIGN(can_send_polls, get_json_object_bool_field(object, "can_send_polls"));
      TRY_RESULT_ASSIGN(can_send_other_messages, get_json_object_bool_field(object, "can_send_other_messages"));
      TRY_RESULT_ASSIGN(can_add_web_page_previews, get_json_object_bool_field(object, "can_add_web_page_previews"));
      TRY_RESULT_ASSIGN(can_change_info, get_json_object_bool_field(object, "can_change_info"));
      TRY_RESULT_ASSIGN(can_invite_users, get_json_object_bool_field(object, "can_invite_users"));
      TRY_RESULT_ASSIGN(can_pin_messages, get_json_object_bool_field(object, "can_pin_messages"));
      return Status::OK();
    }();

    if (status.is_error()) {
      return Status::Error(400, PSLICE() << "Can't parse chat permissions: " << status.error().message());
    }
  } else if (allow_legacy) {
    allow_legacy = false;

    can_send_messages = to_bool(query->arg("can_send_messages"));
    can_send_media_messages = to_bool(query->arg("can_send_media_messages"));
    can_send_other_messages = to_bool(query->arg("can_send_other_messages"));
    can_add_web_page_previews = to_bool(query->arg("can_add_web_page_previews"));

    if (can_send_messages && can_send_media_messages && can_send_other_messages && can_add_web_page_previews) {
      // legacy unrestrict
      can_send_polls = true;
      can_change_info = true;
      can_invite_users = true;
      can_pin_messages = true;
    } else if (query->has_arg("can_send_messages") || query->has_arg("can_send_media_messages") ||
               query->has_arg("can_send_other_messages") || query->has_arg("can_add_web_page_previews")) {
      allow_legacy = true;
    }
  }

  if (can_send_other_messages || can_add_web_page_previews) {
    can_send_media_messages = true;
  }
  return make_object<td_api::chatPermissions>(can_send_messages, can_send_media_messages, can_send_polls,
                                              can_send_other_messages, can_add_web_page_previews, can_change_info,
                                              can_invite_users, can_pin_messages);
}

td::Result<td_api::object_ptr<td_api::InputMessageContent>> Client::get_input_media(const Query *query,
                                                                                    JsonValue &&input_media,
                                                                                    bool for_album) const {
  if (input_media.type() != JsonValue::Type::Object) {
    return Status::Error(400, "expected an Object");
  }

  auto &object = input_media.get_object();

  TRY_RESULT(input_caption, get_json_object_string_field(object, "caption"));
  TRY_RESULT(parse_mode, get_json_object_string_field(object, "parse_mode"));
  auto entities = get_json_object_field_force(object, "caption_entities");
  TRY_RESULT(caption, get_formatted_text(std::move(input_caption), std::move(parse_mode), std::move(entities)));
  // TRY_RESULT(ttl, get_json_object_int_field(object, "ttl"));
  int32 ttl = 0;
  TRY_RESULT(media, get_json_object_string_field(object, "media", true));

  auto input_file = get_input_file(query, Slice(), media, false);
  if (input_file == nullptr) {
    return Status::Error(400, "media not found");
  }

  TRY_RESULT(thumbnail, get_json_object_string_field(object, "thumb"));
  object_ptr<td_api::inputThumbnail> input_thumbnail;
  auto thumbanil_input_file = get_input_file(query, thumbnail.empty() ? Slice("thumb") : Slice(), thumbnail, true);
  if (thumbanil_input_file != nullptr) {
    input_thumbnail = make_object<td_api::inputThumbnail>(std::move(thumbanil_input_file), 0, 0);
  }

  TRY_RESULT(type, get_json_object_string_field(object, "type", false));
  if (type == "photo") {
    return make_object<td_api::inputMessagePhoto>(std::move(input_file), std::move(input_thumbnail),
                                                  td::vector<int32>(), 0, 0, std::move(caption), ttl);
  }
  if (type == "video") {
    TRY_RESULT(width, get_json_object_int_field(object, "width"));
    TRY_RESULT(height, get_json_object_int_field(object, "height"));
    TRY_RESULT(duration, get_json_object_int_field(object, "duration"));
    TRY_RESULT(supports_streaming, get_json_object_bool_field(object, "supports_streaming"));
    width = td::clamp(width, 0, MAX_LENGTH);
    height = td::clamp(height, 0, MAX_LENGTH);
    duration = td::clamp(duration, 0, MAX_DURATION);

    return make_object<td_api::inputMessageVideo>(std::move(input_file), std::move(input_thumbnail),
                                                  td::vector<int32>(), duration, width, height, supports_streaming,
                                                  std::move(caption), ttl);
  }
  if (for_album && type == "animation") {
    return Status::Error(400, PSLICE() << "type \"" << type << "\" can't be used in sendMediaGroup");
  }
  if (type == "animation") {
    TRY_RESULT(width, get_json_object_int_field(object, "width"));
    TRY_RESULT(height, get_json_object_int_field(object, "height"));
    TRY_RESULT(duration, get_json_object_int_field(object, "duration"));
    width = td::clamp(width, 0, MAX_LENGTH);
    height = td::clamp(height, 0, MAX_LENGTH);
    duration = td::clamp(duration, 0, MAX_DURATION);
    return make_object<td_api::inputMessageAnimation>(std::move(input_file), std::move(input_thumbnail),
                                                      td::vector<int32>(), duration, width, height, std::move(caption));
  }
  if (type == "audio") {
    TRY_RESULT(duration, get_json_object_int_field(object, "duration"));
    TRY_RESULT(title, get_json_object_string_field(object, "title"));
    TRY_RESULT(performer, get_json_object_string_field(object, "performer"));
    duration = td::clamp(duration, 0, MAX_DURATION);
    return make_object<td_api::inputMessageAudio>(std::move(input_file), std::move(input_thumbnail), duration, title,
                                                  performer, std::move(caption));
  }
  if (type == "document") {
    TRY_RESULT(disable_content_type_detection, get_json_object_bool_field(object, "disable_content_type_detection"));
    return make_object<td_api::inputMessageDocument>(std::move(input_file), std::move(input_thumbnail),
                                                     disable_content_type_detection || for_album, std::move(caption));
  }

  return Status::Error(400, PSLICE() << "type \"" << type << "\" is unsupported");
}

td::Result<td_api::object_ptr<td_api::InputMessageContent>> Client::get_input_media(const Query *query,
                                                                                    Slice field_name,
                                                                                    bool for_album) const {
  TRY_RESULT(media, get_required_string_arg(query, field_name));

  LOG(INFO) << "Parsing JSON object: " << media;
  auto r_value = json_decode(media);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return Status::Error(400, "Can't parse input media JSON object");
  }

  auto r_input_message_content = get_input_media(query, r_value.move_as_ok(), for_album);
  if (r_input_message_content.is_error()) {
    return Status::Error(400, PSLICE() << "Can't parse InputMedia: " << r_input_message_content.error().message());
  }
  return r_input_message_content.move_as_ok();
}

td::Result<td::vector<td_api::object_ptr<td_api::InputMessageContent>>> Client::get_input_message_contents(
    const Query *query, Slice field_name) const {
  TRY_RESULT(media, get_required_string_arg(query, field_name));

  LOG(INFO) << "Parsing JSON object: " << media;
  auto r_value = json_decode(media);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return Status::Error(400, "Can't parse media JSON object");
  }

  return get_input_message_contents(query, r_value.move_as_ok());
}

td::Result<td::vector<td_api::object_ptr<td_api::InputMessageContent>>> Client::get_input_message_contents(
    const Query *query, JsonValue &&value) const {
  if (value.type() != JsonValue::Type::Array) {
    return Status::Error(400, "Expected an Array of InputMedia");
  }

  td::vector<object_ptr<td_api::InputMessageContent>> contents;
  for (auto &input_media : value.get_array()) {
    TRY_RESULT(input_message_content, get_input_media(query, std::move(input_media), true));
    contents.push_back(std::move(input_message_content));
  }
  return std::move(contents);
}

td::Result<td_api::object_ptr<td_api::inputMessageInvoice>> Client::get_input_message_invoice(const Query *query) {
  TRY_RESULT(title, get_required_string_arg(query, "title"));
  TRY_RESULT(description, get_required_string_arg(query, "description"));
  TRY_RESULT(payload, get_required_string_arg(query, "payload"));
  if (!td::check_utf8(payload.str())) {
    return Status::Error(400, "The payload must be encoded in UTF-8");
  }
  TRY_RESULT(provider_token, get_required_string_arg(query, "provider_token"));
  auto provider_data = query->arg("provider_data");
  auto start_parameter = query->arg("start_parameter");
  TRY_RESULT(currency, get_required_string_arg(query, "currency"));

  TRY_RESULT(labeled_price_parts, get_required_string_arg(query, "prices"));
  auto r_labeled_price_parts_value = json_decode(labeled_price_parts);
  if (r_labeled_price_parts_value.is_error()) {
    return Status::Error(400, "Can't parse prices JSON object");
  }

  TRY_RESULT(prices, get_labeled_price_parts(r_labeled_price_parts_value.ok_ref()));

  int64 max_tip_amount = 0;
  td::vector<int64> suggested_tip_amounts;
  {
    auto max_tip_amount_str = query->arg("max_tip_amount");
    if (!max_tip_amount_str.empty()) {
      auto r_max_tip_amount = td::to_integer_safe<int64>(max_tip_amount_str);
      if (r_max_tip_amount.is_error()) {
        return Status::Error(400, "Can't parse \"max_tip_amount\" as Number");
      }
      max_tip_amount = r_max_tip_amount.ok();
    }

    auto suggested_tip_amounts_str = query->arg("suggested_tip_amounts");
    if (!suggested_tip_amounts_str.empty()) {
      auto r_suggested_tip_amounts_value = json_decode(suggested_tip_amounts_str);
      if (r_suggested_tip_amounts_value.is_error()) {
        return Status::Error(400, "Can't parse suggested_tip_amounts JSON object");
      }

      TRY_RESULT_ASSIGN(suggested_tip_amounts, get_suggested_tip_amounts(r_suggested_tip_amounts_value.ok_ref()));
    }
  }

  auto photo_url = query->arg("photo_url");
  int32 photo_size = get_integer_arg(query, "photo_size", 0, 0, 1000000000);
  int32 photo_width = get_integer_arg(query, "photo_width", 0, 0, MAX_LENGTH);
  int32 photo_height = get_integer_arg(query, "photo_height", 0, 0, MAX_LENGTH);

  auto need_name = to_bool(query->arg("need_name"));
  auto need_phone_number = to_bool(query->arg("need_phone_number"));
  auto need_email_address = to_bool(query->arg("need_email"));
  auto need_shipping_address = to_bool(query->arg("need_shipping_address"));
  auto send_phone_number_to_provider = to_bool(query->arg("send_phone_number_to_provider"));
  auto send_email_address_to_provider = to_bool(query->arg("send_email_to_provider"));
  auto is_flexible = to_bool(query->arg("is_flexible"));

  return make_object<td_api::inputMessageInvoice>(
      make_object<td_api::invoice>(currency.str(), std::move(prices), max_tip_amount, std::move(suggested_tip_amounts),
                                   td::string(), false, need_name, need_phone_number, need_email_address,
                                   need_shipping_address, send_phone_number_to_provider, send_email_address_to_provider,
                                   is_flexible),
      title.str(), description.str(), photo_url.str(), photo_size, photo_width, photo_height, payload.str(),
      provider_token.str(), provider_data.str(), start_parameter.str());
}

td::Result<td::vector<td::string>> Client::get_poll_options(const Query *query) {
  auto input_options = query->arg("options");
  LOG(INFO) << "Parsing JSON object: " << input_options;
  auto r_value = json_decode(input_options);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return Status::Error(400, "Can't parse options JSON object");
  }

  auto value = r_value.move_as_ok();
  if (value.type() != JsonValue::Type::Array) {
    return Status::Error(400, "Expected an Array of String as options");
  }

  td::vector<td::string> options;
  for (auto &input_option : value.get_array()) {
    if (input_option.type() != JsonValue::Type::String) {
      return Status::Error(400, "Expected an option to be of type String");
    }
    options.push_back(input_option.get_string().str());
  }
  return std::move(options);
}

td::int32 Client::get_integer_arg(const Query *query, Slice field_name, int32 default_value, int32 min_value,
                                  int32 max_value) {
  auto s_arg = query->arg(field_name);
  auto value = s_arg.empty() ? default_value : td::to_integer<int32>(s_arg);
  return td::clamp(value, min_value, max_value);
}

td::Result<td::MutableSlice> Client::get_required_string_arg(const Query *query, Slice field_name) {
  auto s_arg = query->arg(field_name);
  if (s_arg.empty()) {
    return Status::Error(400, PSLICE() << "Parameter \"" << field_name << "\" is required");
  }
  return s_arg;
}

td::int64 Client::get_message_id(const Query *query, Slice field_name) {
  auto s_arg = query->arg(field_name);
  if (s_arg.empty()) {
    return 0;
  }

  int arg = td::to_integer<int32>(s_arg);
  if (arg < 0) {
    return 0;
  }

  return as_tdlib_message_id(arg);
}

td::Result<td::Slice> Client::get_inline_message_id(const Query *query, Slice field_name) {
  auto s_arg = query->arg(field_name);
  if (s_arg.empty()) {
    return Status::Error(400, "Message identifier is not specified");
  }
  return s_arg;
}

td::Result<td::int64> Client::get_user_id(const Query *query, Slice field_name) {
  int64 user_id = td::max(td::to_integer<int64>(query->arg(field_name)), static_cast<int64>(0));
  if (user_id == 0) {
    return Status::Error(400, PSLICE() << "Invalid " << field_name << " specified");
  }
  return user_id;
}

td::int64 Client::extract_yet_unsent_message_query_id(int64 chat_id, int64 message_id,
                                                      bool *is_reply_to_message_deleted) {
  auto yet_unsent_message_it = yet_unsent_messages_.find({chat_id, message_id});
  CHECK(yet_unsent_message_it != yet_unsent_messages_.end());

  auto reply_to_message_id = yet_unsent_message_it->second.reply_to_message_id;
  if (is_reply_to_message_deleted != nullptr && yet_unsent_message_it->second.is_reply_to_message_deleted) {
    *is_reply_to_message_deleted = true;
  }
  auto query_id = yet_unsent_message_it->second.send_message_query_id;

  yet_unsent_messages_.erase(yet_unsent_message_it);

  auto count_it = yet_unsent_message_count_.find(chat_id);
  CHECK(count_it != yet_unsent_message_count_.end());
  CHECK(count_it->second > 0);
  if (--count_it->second == 0) {
    yet_unsent_message_count_.erase(count_it);
  }

  if (reply_to_message_id > 0) {
    auto it = yet_unsent_reply_message_ids_.find({chat_id, reply_to_message_id});
    CHECK(it != yet_unsent_reply_message_ids_.end());
    auto erased_count = it->second.erase(message_id);
    CHECK(erased_count > 0);
    if (it->second.empty()) {
      yet_unsent_reply_message_ids_.erase(it);
    }
  }

  return query_id;
}

void Client::on_message_send_succeeded(object_ptr<td_api::message> &&message, int64 old_message_id) {
  auto full_message_id = add_message(std::move(message), true);

  int64 chat_id = full_message_id.chat_id;
  int64 new_message_id = full_message_id.message_id;
  CHECK(new_message_id > 0);

  auto message_info = get_message(chat_id, new_message_id);
  CHECK(message_info != nullptr);
  message_info->is_content_changed = false;

  auto query_id =
      extract_yet_unsent_message_query_id(chat_id, old_message_id, &message_info->is_reply_to_message_deleted);
  auto &query = *pending_send_message_queries_[query_id];
  if (query.is_multisend) {
    query.messages.push_back(td::json_encode<td::string>(JsonMessage(message_info, true, "sent message", this)));
    query.awaited_message_count--;

    if (query.awaited_message_count == 0) {
      if (query.error == nullptr) {
        answer_query(JsonMessages(query.messages), std::move(query.query));
      } else {
        fail_query_with_error(std::move(query.query), std::move(query.error));
      }
      pending_send_message_queries_.erase(query_id);
    }
  } else {
    CHECK(query.awaited_message_count == 1);
    if (query.query->method() == "copymessage") {
      answer_query(JsonMessageId(new_message_id), std::move(query.query));
    } else {
      answer_query(JsonMessage(message_info, true, "sent message", this), std::move(query.query));
    }
    pending_send_message_queries_.erase(query_id);
  }
}

void Client::on_message_send_failed(int64 chat_id, int64 old_message_id, int64 new_message_id, Status result) {
  auto error = make_object<td_api::error>(result.code(), result.message().str());

  auto query_id = extract_yet_unsent_message_query_id(chat_id, old_message_id, nullptr);
  auto &query = *pending_send_message_queries_[query_id];
  if (query.is_multisend) {
    if (query.error == nullptr || query.error->message_ == "Group send failed") {
      if (error->code_ == 429 || error->message_ == "Group send failed") {
        query.error = std::move(error);
      } else {
        auto pos = (query.total_message_count - query.awaited_message_count + 1);
        query.error = make_object<td_api::error>(error->code_, PSTRING() << "Failed to send message #" << pos
                                                                         << " with the error message \""
                                                                         << error->message_ << '"');
      }
    }
    query.awaited_message_count--;

    if (query.awaited_message_count == 0) {
      fail_query_with_error(std::move(query.query), std::move(query.error));
      pending_send_message_queries_.erase(query_id);
    }
  } else {
    CHECK(query.awaited_message_count == 1);
    fail_query_with_error(std::move(query.query), std::move(error));
    pending_send_message_queries_.erase(query_id);
  }

  if (new_message_id != 0 && !logging_out_ && !closing_) {
    send_request(make_object<td_api::deleteMessages>(chat_id, td::vector<int64>{new_message_id}, false),
                 td::make_unique<TdOnDeleteFailedToSendMessageCallback>(this, chat_id, new_message_id));
  }
}

void Client::on_cmd(PromisedQueryPtr query) {
  LOG(DEBUG) << "Process query " << *query;
  if (!td_client_.empty()) {
    if (query->method() == "close") {
      auto retry_after = static_cast<int>(10 * 60 - (td::Time::now() - start_time_));
      if (retry_after > 0 && start_time_ > parameters_->start_time_ + 10 * 60) {
        return query->set_retry_after_error(retry_after);
      }
      need_close_ = true;
      return do_send_request(make_object<td_api::close>(), td::make_unique<TdOnOkQueryCallback>(std::move(query)));
    }
    if (query->method() == "logout") {
      clear_tqueue_ = true;
      return do_send_request(make_object<td_api::logOut>(), td::make_unique<TdOnOkQueryCallback>(std::move(query)));
    }
  }

  if (logging_out_) {
    return fail_query(LOGGING_OUT_ERROR_CODE, get_logging_out_error_description(), std::move(query));
  }
  if (closing_) {
    return fail_query(CLOSING_ERROR_CODE, CLOSING_ERROR_DESCRIPTION, std::move(query));
  }
  CHECK(was_authorized_);

  unresolved_bot_usernames_.clear();

  auto method_it = methods_.find(query->method().str());
  if (method_it == methods_.end()) {
    return fail_query(404, "Not Found: method not found", std::move(query));
  }

  auto result = (this->*(method_it->second))(query);
  if (result.is_error()) {
    fail_query_with_error(std::move(query), result.code(), result.message());
  }
}

td::Status Client::process_get_me_query(PromisedQueryPtr &query) {
  answer_query(JsonUser(my_id_, this, true), std::move(query));
  return Status::OK();
}

td::Status Client::process_get_my_commands_query(PromisedQueryPtr &query) {
  TRY_RESULT(scope, get_bot_command_scope(query.get()));

  check_bot_command_scope(std::move(scope), std::move(query),
                          [this](object_ptr<td_api::BotCommandScope> &&scope, PromisedQueryPtr query) mutable {
                            auto language_code = query->arg("language_code").str();
                            send_request(make_object<td_api::getCommands>(std::move(scope), language_code),
                                         td::make_unique<TdOnGetMyCommandsCallback>(std::move(query)));
                          });
  return Status::OK();
}

td::Status Client::process_set_my_commands_query(PromisedQueryPtr &query) {
  TRY_RESULT(bot_commands, get_bot_commands(query.get()));
  TRY_RESULT(scope, get_bot_command_scope(query.get()));

  check_bot_command_scope(
      std::move(scope), std::move(query),
      [this, bot_commands = std::move(bot_commands)](object_ptr<td_api::BotCommandScope> &&scope,
                                                     PromisedQueryPtr query) mutable {
        auto language_code = query->arg("language_code").str();
        send_request(make_object<td_api::setCommands>(std::move(scope), language_code, std::move(bot_commands)),
                     td::make_unique<TdOnOkQueryCallback>(std::move(query)));
      });
  return Status::OK();
}

td::Status Client::process_delete_my_commands_query(PromisedQueryPtr &query) {
  TRY_RESULT(scope, get_bot_command_scope(query.get()));

  check_bot_command_scope(std::move(scope), std::move(query),
                          [this](object_ptr<td_api::BotCommandScope> &&scope, PromisedQueryPtr query) mutable {
                            auto language_code = query->arg("language_code").str();
                            send_request(make_object<td_api::deleteCommands>(std::move(scope), language_code),
                                         td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                          });
  return Status::OK();
}

td::Status Client::process_get_my_default_administrator_rights_query(PromisedQueryPtr &query) {
  bool for_channels = to_bool(query->arg("for_channels"));
  send_request(make_object<td_api::getUserFullInfo>(my_id_),
               td::make_unique<TdOnGetMyDefaultAdministratorRightsCallback>(for_channels, std::move(query)));
  return Status::OK();
}

td::Status Client::process_set_my_default_administrator_rights_query(PromisedQueryPtr &query) {
  bool for_channels = to_bool(query->arg("for_channels"));
  TRY_RESULT(rights, get_chat_administrator_rights(query.get()));

  if (for_channels) {
    send_request(make_object<td_api::setDefaultChannelAdministratorRights>(std::move(rights)),
                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  } else {
    send_request(make_object<td_api::setDefaultGroupAdministratorRights>(std::move(rights)),
                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  }
  return Status::OK();
}

td::Status Client::process_get_chat_menu_button_query(PromisedQueryPtr &query) {
  if (query->has_arg("chat_id")) {
    TRY_RESULT(user_id, get_user_id(query.get(), "chat_id"));
    check_user(user_id, std::move(query), [this, user_id](PromisedQueryPtr query) {
      send_request(make_object<td_api::getMenuButton>(user_id),
                   td::make_unique<TdOnGetMenuButtonCallback>(std::move(query)));
    });
  } else {
    send_request(make_object<td_api::getMenuButton>(0), td::make_unique<TdOnGetMenuButtonCallback>(std::move(query)));
  }
  return Status::OK();
}

td::Status Client::process_set_chat_menu_button_query(PromisedQueryPtr &query) {
  TRY_RESULT(menu_button, get_bot_menu_button(query.get()));
  if (query->has_arg("chat_id")) {
    TRY_RESULT(user_id, get_user_id(query.get(), "chat_id"));
    check_user(user_id, std::move(query),
               [this, user_id, menu_button = std::move(menu_button)](PromisedQueryPtr query) mutable {
                 send_request(make_object<td_api::setMenuButton>(user_id, std::move(menu_button)),
                              td::make_unique<TdOnOkQueryCallback>(std::move(query)));
               });
  } else {
    send_request(make_object<td_api::setMenuButton>(0, std::move(menu_button)),
                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  }
  return Status::OK();
}

td::Status Client::process_get_user_profile_photos_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  int32 offset = get_integer_arg(query.get(), "offset", 0, 0);
  int32 limit = get_integer_arg(query.get(), "limit", 100, 1, 100);

  check_user(user_id, std::move(query), [this, user_id, offset, limit](PromisedQueryPtr query) {
    send_request(make_object<td_api::getUserProfilePhotos>(user_id, offset, limit),
                 td::make_unique<TdOnGetUserProfilePhotosCallback>(this, std::move(query)));
  });
  return Status::OK();
}

td::Status Client::process_send_message_query(PromisedQueryPtr &query) {
  TRY_RESULT(input_message_text, get_input_message_text(query.get()));
  do_send_message(std::move(input_message_text), std::move(query));
  return Status::OK();
}

td::Status Client::process_send_animation_query(PromisedQueryPtr &query) {
  auto animation = get_input_file(query.get(), "animation");
  if (animation == nullptr) {
    return Status::Error(400, "There is no animation in the request");
  }
  auto thumbnail = get_input_thumbnail(query.get(), "thumb");
  int32 duration = get_integer_arg(query.get(), "duration", 0, 0, MAX_DURATION);
  int32 width = get_integer_arg(query.get(), "width", 0, 0, MAX_LENGTH);
  int32 height = get_integer_arg(query.get(), "height", 0, 0, MAX_LENGTH);
  TRY_RESULT(caption, get_caption(query.get()));
  do_send_message(
      make_object<td_api::inputMessageAnimation>(std::move(animation), std::move(thumbnail), td::vector<int32>(),
                                                 duration, width, height, std::move(caption)),
      std::move(query));
  return Status::OK();
}

td::Status Client::process_send_audio_query(PromisedQueryPtr &query) {
  auto audio = get_input_file(query.get(), "audio");
  if (audio == nullptr) {
    return Status::Error(400, "There is no audio in the request");
  }
  auto thumbnail = get_input_thumbnail(query.get(), "thumb");
  int32 duration = get_integer_arg(query.get(), "duration", 0, 0, MAX_DURATION);
  auto title = query->arg("title").str();
  auto performer = query->arg("performer").str();
  TRY_RESULT(caption, get_caption(query.get()));
  do_send_message(make_object<td_api::inputMessageAudio>(std::move(audio), std::move(thumbnail), duration, title,
                                                         performer, std::move(caption)),
                  std::move(query));
  return Status::OK();
}

td::Status Client::process_send_dice_query(PromisedQueryPtr &query) {
  auto emoji = query->arg("emoji");
  do_send_message(make_object<td_api::inputMessageDice>(emoji.str(), false), std::move(query));
  return Status::OK();
}

td::Status Client::process_send_document_query(PromisedQueryPtr &query) {
  auto document = get_input_file(query.get(), "document");
  if (document == nullptr) {
    return Status::Error(400, "There is no document in the request");
  }
  auto thumbnail = get_input_thumbnail(query.get(), "thumb");
  TRY_RESULT(caption, get_caption(query.get()));
  bool disable_content_type_detection = to_bool(query->arg("disable_content_type_detection"));
  do_send_message(make_object<td_api::inputMessageDocument>(std::move(document), std::move(thumbnail),
                                                            disable_content_type_detection, std::move(caption)),
                  std::move(query));
  return Status::OK();
}

td::Status Client::process_send_photo_query(PromisedQueryPtr &query) {
  auto photo = get_input_file(query.get(), "photo");
  if (photo == nullptr) {
    return Status::Error(400, "There is no photo in the request");
  }
  TRY_RESULT(caption, get_caption(query.get()));
  auto ttl = 0;
  do_send_message(make_object<td_api::inputMessagePhoto>(std::move(photo), nullptr, td::vector<int32>(), 0, 0,
                                                         std::move(caption), ttl),
                  std::move(query));
  return Status::OK();
}

td::Status Client::process_send_sticker_query(PromisedQueryPtr &query) {
  auto sticker = get_input_file(query.get(), "sticker");
  if (sticker == nullptr) {
    return Status::Error(400, "There is no sticker in the request");
  }
  do_send_message(make_object<td_api::inputMessageSticker>(std::move(sticker), nullptr, 0, 0, td::string()),
                  std::move(query));
  return Status::OK();
}

td::Status Client::process_send_video_query(PromisedQueryPtr &query) {
  auto video = get_input_file(query.get(), "video");
  if (video == nullptr) {
    return Status::Error(400, "There is no video in the request");
  }
  auto thumbnail = get_input_thumbnail(query.get(), "thumb");
  int32 duration = get_integer_arg(query.get(), "duration", 0, 0, MAX_DURATION);
  int32 width = get_integer_arg(query.get(), "width", 0, 0, MAX_LENGTH);
  int32 height = get_integer_arg(query.get(), "height", 0, 0, MAX_LENGTH);
  bool supports_streaming = to_bool(query->arg("supports_streaming"));
  TRY_RESULT(caption, get_caption(query.get()));
  auto ttl = 0;
  do_send_message(
      make_object<td_api::inputMessageVideo>(std::move(video), std::move(thumbnail), td::vector<int32>(), duration,
                                             width, height, supports_streaming, std::move(caption), ttl),
      std::move(query));
  return Status::OK();
}

td::Status Client::process_send_video_note_query(PromisedQueryPtr &query) {
  auto video_note = get_input_file(query.get(), "video_note");
  if (video_note == nullptr) {
    return Status::Error(400, "There is no video note in the request");
  }
  auto thumbnail = get_input_thumbnail(query.get(), "thumb");
  int32 duration = get_integer_arg(query.get(), "duration", 0, 0, MAX_DURATION);
  int32 length = get_integer_arg(query.get(), "length", 0, 0, MAX_LENGTH);
  do_send_message(
      make_object<td_api::inputMessageVideoNote>(std::move(video_note), std::move(thumbnail), duration, length),
      std::move(query));
  return Status::OK();
}

td::Status Client::process_send_voice_query(PromisedQueryPtr &query) {
  auto voice_note = get_input_file(query.get(), "voice");
  if (voice_note == nullptr) {
    return Status::Error(400, "There is no voice in the request");
  }
  int32 duration = get_integer_arg(query.get(), "duration", 0, 0, MAX_DURATION);
  TRY_RESULT(caption, get_caption(query.get()));
  do_send_message(make_object<td_api::inputMessageVoiceNote>(std::move(voice_note), duration, "", std::move(caption)),
                  std::move(query));
  return Status::OK();
}

td::Status Client::process_send_game_query(PromisedQueryPtr &query) {
  TRY_RESULT(game_short_name, get_required_string_arg(query.get(), "game_short_name"));
  do_send_message(make_object<td_api::inputMessageGame>(my_id_, game_short_name.str()), std::move(query));
  return Status::OK();
}

td::Status Client::process_send_invoice_query(PromisedQueryPtr &query) {
  TRY_RESULT(input_message_invoice, get_input_message_invoice(query.get()));
  do_send_message(std::move(input_message_invoice), std::move(query));
  return Status::OK();
}

td::Status Client::process_send_location_query(PromisedQueryPtr &query) {
  TRY_RESULT(location, get_location(query.get()));
  int32 live_period = get_integer_arg(query.get(), "live_period", 0);
  int32 heading = get_integer_arg(query.get(), "heading", 0);
  int32 proximity_alert_radius = get_integer_arg(query.get(), "proximity_alert_radius", 0);

  do_send_message(
      make_object<td_api::inputMessageLocation>(std::move(location), live_period, heading, proximity_alert_radius),
      std::move(query));
  return Status::OK();
}

td::Status Client::process_send_venue_query(PromisedQueryPtr &query) {
  TRY_RESULT(location, get_location(query.get()));

  auto title = query->arg("title");
  auto address = query->arg("address");
  td::string provider;
  td::string venue_id;
  td::string venue_type;

  auto google_place_id = query->arg("google_place_id");
  auto google_place_type = query->arg("google_place_type");
  if (!google_place_id.empty() || !google_place_type.empty()) {
    provider = "gplaces";
    venue_id = google_place_id.str();
    venue_type = google_place_type.str();
  }
  auto foursquare_id = query->arg("foursquare_id");
  auto foursquare_type = query->arg("foursquare_type");
  if (!foursquare_id.empty() || !foursquare_type.empty()) {
    provider = "foursquare";
    venue_id = foursquare_id.str();
    venue_type = foursquare_type.str();
  }

  do_send_message(make_object<td_api::inputMessageVenue>(make_object<td_api::venue>(
                      std::move(location), title.str(), address.str(), provider, venue_id, venue_type)),
                  std::move(query));
  return Status::OK();
}

td::Status Client::process_send_contact_query(PromisedQueryPtr &query) {
  TRY_RESULT(phone_number, get_required_string_arg(query.get(), "phone_number"));
  TRY_RESULT(first_name, get_required_string_arg(query.get(), "first_name"));
  auto last_name = query->arg("last_name");
  auto vcard = query->arg("vcard");
  do_send_message(make_object<td_api::inputMessageContact>(make_object<td_api::contact>(
                      phone_number.str(), first_name.str(), last_name.str(), vcard.str(), 0)),
                  std::move(query));
  return Status::OK();
}

td::Status Client::process_send_poll_query(PromisedQueryPtr &query) {
  auto question = query->arg("question");
  TRY_RESULT(options, get_poll_options(query.get()));
  bool is_anonymous = true;
  if (query->has_arg("is_anonymous")) {
    is_anonymous = to_bool(query->arg("is_anonymous"));
  }

  object_ptr<td_api::PollType> poll_type;
  auto type = query->arg("type");
  if (type == "quiz") {
    TRY_RESULT(explanation,
               get_formatted_text(query->arg("explanation").str(), query->arg("explanation_parse_mode").str(),
                                  get_input_entities(query.get(), "explanation_entities")));

    poll_type = make_object<td_api::pollTypeQuiz>(get_integer_arg(query.get(), "correct_option_id", -1),
                                                  std::move(explanation));
  } else if (type.empty() || type == "regular") {
    poll_type = make_object<td_api::pollTypeRegular>(to_bool(query->arg("allows_multiple_answers")));
  } else {
    return Status::Error(400, "Unsupported poll type specified");
  }
  int32 open_period = get_integer_arg(query.get(), "open_period", 0, 0, 10 * 60);
  int32 close_date = get_integer_arg(query.get(), "close_date", 0);
  auto is_closed = to_bool(query->arg("is_closed"));
  do_send_message(make_object<td_api::inputMessagePoll>(question.str(), std::move(options), is_anonymous,
                                                        std::move(poll_type), open_period, close_date, is_closed),
                  std::move(query));
  return Status::OK();
}

td::Status Client::process_stop_poll_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  TRY_RESULT(reply_markup, get_reply_markup(query.get()));

  resolve_reply_markup_bot_usernames(
      std::move(reply_markup), std::move(query),
      [this, chat_id = chat_id.str(), message_id](object_ptr<td_api::ReplyMarkup> reply_markup,
                                                  PromisedQueryPtr query) {
        check_message(chat_id, message_id, false, AccessRights::Edit, "message with poll to stop", std::move(query),
                      [this, reply_markup = std::move(reply_markup)](int64 chat_id, int64 message_id,
                                                                     PromisedQueryPtr query) mutable {
                        send_request(
                            make_object<td_api::stopPoll>(chat_id, message_id, std::move(reply_markup)),
                            td::make_unique<TdOnStopPollCallback>(this, chat_id, message_id, std::move(query)));
                      });
      });
  return Status::OK();
}

td::Status Client::process_copy_message_query(PromisedQueryPtr &query) {
  TRY_RESULT(from_chat_id, get_required_string_arg(query.get(), "from_chat_id"));
  auto message_id = get_message_id(query.get());
  bool replace_caption = query->has_arg("caption");
  object_ptr<td_api::formattedText> caption;
  if (replace_caption) {
    TRY_RESULT_ASSIGN(caption, get_caption(query.get()));
  }
  auto options = make_object<td_api::messageCopyOptions>(true, replace_caption, std::move(caption));

  check_message(
      from_chat_id, message_id, false, AccessRights::Read, "message to copy", std::move(query),
      [this, options = std::move(options)](int64 from_chat_id, int64 message_id, PromisedQueryPtr query) mutable {
        do_send_message(make_object<td_api::inputMessageForwarded>(from_chat_id, message_id, false, std::move(options)),
                        std::move(query));
      });
  return Status::OK();
}

td::Status Client::process_forward_message_query(PromisedQueryPtr &query) {
  TRY_RESULT(from_chat_id, get_required_string_arg(query.get(), "from_chat_id"));
  auto message_id = get_message_id(query.get());

  check_message(from_chat_id, message_id, false, AccessRights::Read, "message to forward", std::move(query),
                [this](int64 from_chat_id, int64 message_id, PromisedQueryPtr query) {
                  do_send_message(make_object<td_api::inputMessageForwarded>(from_chat_id, message_id, false, nullptr),
                                  std::move(query));
                });
  return Status::OK();
}

td::Status Client::process_send_media_group_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto reply_to_message_id = get_message_id(query.get(), "reply_to_message_id");
  auto allow_sending_without_reply = to_bool(query->arg("allow_sending_without_reply"));
  auto disable_notification = to_bool(query->arg("disable_notification"));
  auto protect_content = to_bool(query->arg("protect_content"));
  // TRY_RESULT(reply_markup, get_reply_markup(query.get()));
  auto reply_markup = nullptr;
  TRY_RESULT(input_message_contents, get_input_message_contents(query.get(), "media"));

  resolve_reply_markup_bot_usernames(
      std::move(reply_markup), std::move(query),
      [this, chat_id = chat_id.str(), reply_to_message_id, allow_sending_without_reply, disable_notification,
       protect_content, input_message_contents = std::move(input_message_contents)](
          object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) mutable {
        auto on_success = [this, disable_notification, protect_content,
                           input_message_contents = std::move(input_message_contents),
                           reply_markup = std::move(reply_markup)](int64 chat_id, int64 reply_to_message_id,
                                                                   PromisedQueryPtr query) mutable {
          auto it = yet_unsent_message_count_.find(chat_id);
          if (it != yet_unsent_message_count_.end() && it->second > MAX_CONCURRENTLY_SENT_CHAT_MESSAGES) {
            return query->set_retry_after_error(60);
          }

          send_request(
              make_object<td_api::sendMessageAlbum>(chat_id, 0, reply_to_message_id,
                                                    get_message_send_options(disable_notification, protect_content),
                                                    std::move(input_message_contents), false),
              td::make_unique<TdOnSendMessageAlbumCallback>(this, std::move(query)));
        };
        check_message(chat_id, reply_to_message_id, reply_to_message_id <= 0 || allow_sending_without_reply,
                      AccessRights::Write, "replied message", std::move(query), std::move(on_success));
      });
  return Status::OK();
}

td::Status Client::process_send_chat_action_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  object_ptr<td_api::ChatAction> action = get_chat_action(query.get());
  if (action == nullptr) {
    return Status::Error(400, "Wrong parameter action in request");
  }

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, action = std::move(action)](int64 chat_id, PromisedQueryPtr query) mutable {
               send_request(make_object<td_api::sendChatAction>(chat_id, 0, std::move(action)),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return Status::OK();
}

td::Status Client::process_edit_message_text_query(PromisedQueryPtr &query) {
  TRY_RESULT(input_message_text, get_input_message_text(query.get()));
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  TRY_RESULT(reply_markup, get_reply_markup(query.get()));

  if (chat_id.empty() && message_id == 0) {
    TRY_RESULT(inline_message_id, get_inline_message_id(query.get()));
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, inline_message_id = inline_message_id.str(), input_message_text = std::move(input_message_text)](
            object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) mutable {
          send_request(make_object<td_api::editInlineMessageText>(inline_message_id, std::move(reply_markup),
                                                                  std::move(input_message_text)),
                       td::make_unique<TdOnEditInlineMessageCallback>(std::move(query)));
        });
  } else {
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, chat_id = chat_id.str(), message_id, input_message_text = std::move(input_message_text)](
            object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) mutable {
          check_message(
              chat_id, message_id, false, AccessRights::Edit, "message to edit", std::move(query),
              [this, input_message_text = std::move(input_message_text), reply_markup = std::move(reply_markup)](
                  int64 chat_id, int64 message_id, PromisedQueryPtr query) mutable {
                send_request(make_object<td_api::editMessageText>(chat_id, message_id, std::move(reply_markup),
                                                                  std::move(input_message_text)),
                             td::make_unique<TdOnEditMessageCallback>(this, std::move(query)));
              });
        });
  }
  return Status::OK();
}

td::Status Client::process_edit_message_live_location_query(PromisedQueryPtr &query) {
  object_ptr<td_api::location> location = nullptr;
  int32 heading = get_integer_arg(query.get(), "heading", 0);
  int32 proximity_alert_radius = get_integer_arg(query.get(), "proximity_alert_radius", 0);
  if (query->method() == "editmessagelivelocation") {
    TRY_RESULT_ASSIGN(location, get_location(query.get()));
  }
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  TRY_RESULT(reply_markup, get_reply_markup(query.get()));

  if (chat_id.empty() && message_id == 0) {
    TRY_RESULT(inline_message_id, get_inline_message_id(query.get()));
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, inline_message_id = inline_message_id.str(), location = std::move(location), heading,
         proximity_alert_radius](object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) mutable {
          send_request(
              make_object<td_api::editInlineMessageLiveLocation>(inline_message_id, std::move(reply_markup),
                                                                 std::move(location), heading, proximity_alert_radius),
              td::make_unique<TdOnEditInlineMessageCallback>(std::move(query)));
        });
  } else {
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, chat_id = chat_id.str(), message_id, location = std::move(location), heading, proximity_alert_radius](
            object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) mutable {
          check_message(chat_id, message_id, false, AccessRights::Edit, "message to edit", std::move(query),
                        [this, location = std::move(location), heading, proximity_alert_radius,
                         reply_markup = std::move(reply_markup)](int64 chat_id, int64 message_id,
                                                                 PromisedQueryPtr query) mutable {
                          send_request(make_object<td_api::editMessageLiveLocation>(
                                           chat_id, message_id, std::move(reply_markup), std::move(location), heading,
                                           proximity_alert_radius),
                                       td::make_unique<TdOnEditMessageCallback>(this, std::move(query)));
                        });
        });
  }
  return Status::OK();
}

td::Status Client::process_edit_message_media_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  TRY_RESULT(reply_markup, get_reply_markup(query.get()));
  TRY_RESULT(input_media, get_input_media(query.get(), "media", false));

  if (chat_id.empty() && message_id == 0) {
    TRY_RESULT(inline_message_id, get_inline_message_id(query.get()));
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, inline_message_id = inline_message_id.str(), input_message_content = std::move(input_media)](
            object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) mutable {
          send_request(make_object<td_api::editInlineMessageMedia>(inline_message_id, std::move(reply_markup),
                                                                   std::move(input_message_content)),
                       td::make_unique<TdOnEditInlineMessageCallback>(std::move(query)));
        });
  } else {
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, chat_id = chat_id.str(), message_id, input_message_content = std::move(input_media)](
            object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) mutable {
          check_message(
              chat_id, message_id, false, AccessRights::Edit, "message to edit", std::move(query),
              [this, reply_markup = std::move(reply_markup), input_message_content = std::move(input_message_content)](
                  int64 chat_id, int64 message_id, PromisedQueryPtr query) mutable {
                send_request(make_object<td_api::editMessageMedia>(chat_id, message_id, std::move(reply_markup),
                                                                   std::move(input_message_content)),
                             td::make_unique<TdOnEditMessageCallback>(this, std::move(query)));
              });
        });
  }
  return Status::OK();
}

td::Status Client::process_edit_message_caption_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  TRY_RESULT(reply_markup, get_reply_markup(query.get()));
  TRY_RESULT(caption, get_caption(query.get()));

  if (chat_id.empty() && message_id == 0) {
    TRY_RESULT(inline_message_id, get_inline_message_id(query.get()));
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, inline_message_id = inline_message_id.str(), caption = std::move(caption)](
            object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) mutable {
          send_request(make_object<td_api::editInlineMessageCaption>(inline_message_id, std::move(reply_markup),
                                                                     std::move(caption)),
                       td::make_unique<TdOnEditInlineMessageCallback>(std::move(query)));
        });
  } else {
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, chat_id = chat_id.str(), message_id, caption = std::move(caption)](
            object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) mutable {
          check_message(chat_id, message_id, false, AccessRights::Edit, "message to edit", std::move(query),
                        [this, reply_markup = std::move(reply_markup), caption = std::move(caption)](
                            int64 chat_id, int64 message_id, PromisedQueryPtr query) mutable {
                          send_request(make_object<td_api::editMessageCaption>(
                                           chat_id, message_id, std::move(reply_markup), std::move(caption)),
                                       td::make_unique<TdOnEditMessageCallback>(this, std::move(query)));
                        });
        });
  }
  return Status::OK();
}

td::Status Client::process_edit_message_reply_markup_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  TRY_RESULT(reply_markup, get_reply_markup(query.get()));

  if (chat_id.empty() && message_id == 0) {
    TRY_RESULT(inline_message_id, get_inline_message_id(query.get()));
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, inline_message_id = inline_message_id.str()](object_ptr<td_api::ReplyMarkup> reply_markup,
                                                            PromisedQueryPtr query) {
          send_request(make_object<td_api::editInlineMessageReplyMarkup>(inline_message_id, std::move(reply_markup)),
                       td::make_unique<TdOnEditInlineMessageCallback>(std::move(query)));
        });
  } else {
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, chat_id = chat_id.str(), message_id](object_ptr<td_api::ReplyMarkup> reply_markup,
                                                    PromisedQueryPtr query) {
          check_message(chat_id, message_id, false, AccessRights::Edit, "message to edit", std::move(query),
                        [this, reply_markup = std::move(reply_markup)](int64 chat_id, int64 message_id,
                                                                       PromisedQueryPtr query) mutable {
                          send_request(
                              make_object<td_api::editMessageReplyMarkup>(chat_id, message_id, std::move(reply_markup)),
                              td::make_unique<TdOnEditMessageCallback>(this, std::move(query)));
                        });
        });
  }
  return Status::OK();
}

td::Status Client::process_delete_message_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());

  if (chat_id.empty()) {
    return Status::Error(400, "Chat identifier is not specified");
  }

  if (message_id == 0) {
    return Status::Error(400, "Message identifier is not specified");
  }

  check_message(chat_id, message_id, false, AccessRights::Write, "message to delete", std::move(query),
                [this](int64 chat_id, int64 message_id, PromisedQueryPtr query) {
                  delete_message(chat_id, message_id, false);
                  send_request(make_object<td_api::deleteMessages>(chat_id, td::vector<int64>{message_id}, true),
                               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                });
  return Status::OK();
}

td::Status Client::process_create_invoice_link_query(PromisedQueryPtr &query) {
  TRY_RESULT(input_message_invoice, get_input_message_invoice(query.get()));
  send_request(make_object<td_api::createInvoiceLink>(std::move(input_message_invoice)),
               td::make_unique<TdOnCreateInvoiceLinkCallback>(std::move(query)));
  return Status::OK();
}

td::Status Client::process_set_game_score_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto score = td::to_integer<int32>(query->arg("score"));
  auto force = to_bool(query->arg("force"));
  bool edit_message = true;
  if (query->has_arg("disable_edit_message")) {
    edit_message = !to_bool(query->arg("disable_edit_message"));
  } else if (query->has_arg("edit_message")) {
    edit_message = to_bool(query->arg("edit_message"));
  }

  if (chat_id.empty() && message_id == 0) {
    TRY_RESULT(inline_message_id, get_inline_message_id(query.get()));
    check_user_no_fail(
        user_id, std::move(query),
        [this, inline_message_id = inline_message_id.str(), edit_message, user_id, score,
         force](PromisedQueryPtr query) {
          send_request(make_object<td_api::setInlineGameScore>(inline_message_id, edit_message, user_id, score, force),
                       td::make_unique<TdOnEditInlineMessageCallback>(std::move(query)));
        });
  } else {
    check_message(chat_id, message_id, false, AccessRights::Edit, "message to set game score", std::move(query),
                  [this, user_id, score, force, edit_message](int64 chat_id, int64 message_id, PromisedQueryPtr query) {
                    check_user_no_fail(
                        user_id, std::move(query),
                        [this, chat_id, message_id, user_id, score, force, edit_message](PromisedQueryPtr query) {
                          send_request(make_object<td_api::setGameScore>(chat_id, message_id, edit_message, user_id,
                                                                         score, force),
                                       td::make_unique<TdOnEditMessageCallback>(this, std::move(query)));
                        });
                  });
  }
  return Status::OK();
}

td::Status Client::process_get_game_high_scores_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  TRY_RESULT(user_id, get_user_id(query.get()));

  if (chat_id.empty() && message_id == 0) {
    TRY_RESULT(inline_message_id, get_inline_message_id(query.get()));
    check_user_no_fail(user_id, std::move(query),
                       [this, inline_message_id = inline_message_id.str(), user_id](PromisedQueryPtr query) {
                         send_request(make_object<td_api::getInlineGameHighScores>(inline_message_id, user_id),
                                      td::make_unique<TdOnGetGameHighScoresCallback>(this, std::move(query)));
                       });
  } else {
    check_message(chat_id, message_id, false, AccessRights::Read, "message to get game high scores", std::move(query),
                  [this, user_id](int64 chat_id, int64 message_id, PromisedQueryPtr query) {
                    check_user_no_fail(
                        user_id, std::move(query), [this, chat_id, message_id, user_id](PromisedQueryPtr query) {
                          send_request(make_object<td_api::getGameHighScores>(chat_id, message_id, user_id),
                                       td::make_unique<TdOnGetGameHighScoresCallback>(this, std::move(query)));
                        });
                  });
  }
  return Status::OK();
}

td::Status Client::process_answer_web_app_query_query(PromisedQueryPtr &query) {
  auto web_app_query_id = query->arg("web_app_query_id");

  TRY_RESULT(result, get_inline_query_result(query.get()));
  td::vector<object_ptr<td_api::InputInlineQueryResult>> results;
  results.push_back(std::move(result));

  resolve_inline_query_results_bot_usernames(
      std::move(results), std::move(query),
      [this, web_app_query_id = web_app_query_id.str()](td::vector<object_ptr<td_api::InputInlineQueryResult>> results,
                                                        PromisedQueryPtr query) {
        CHECK(results.size() == 1);
        send_request(make_object<td_api::answerWebAppQuery>(web_app_query_id, std::move(results[0])),
                     td::make_unique<TdOnAnswerWebAppQueryCallback>(std::move(query)));
      });
  return Status::OK();
}

td::Status Client::process_answer_inline_query_query(PromisedQueryPtr &query) {
  auto inline_query_id = td::to_integer<int64>(query->arg("inline_query_id"));
  auto is_personal = to_bool(query->arg("is_personal"));
  int32 cache_time = get_integer_arg(query.get(), "cache_time", 300, 0, 24 * 60 * 60);
  auto next_offset = query->arg("next_offset");
  auto switch_pm_text = query->arg("switch_pm_text");
  auto switch_pm_parameter = query->arg("switch_pm_parameter");

  TRY_RESULT(results, get_inline_query_results(query.get()));

  resolve_inline_query_results_bot_usernames(
      std::move(results), std::move(query),
      [this, inline_query_id, is_personal, cache_time, next_offset = next_offset.str(),
       switch_pm_text = switch_pm_text.str(), switch_pm_parameter = switch_pm_parameter.str()](
          td::vector<object_ptr<td_api::InputInlineQueryResult>> results, PromisedQueryPtr query) {
        send_request(
            make_object<td_api::answerInlineQuery>(inline_query_id, is_personal, std::move(results), cache_time,
                                                   next_offset, switch_pm_text, switch_pm_parameter),
            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
      });
  return Status::OK();
}

td::Status Client::process_answer_callback_query_query(PromisedQueryPtr &query) {
  auto callback_query_id = td::to_integer<int64>(query->arg("callback_query_id"));
  td::string text = query->arg("text").str();
  bool show_alert = to_bool(query->arg("show_alert"));
  td::string url = query->arg("url").str();
  int32 cache_time = get_integer_arg(query.get(), "cache_time", 0, 0, 24 * 30 * 60 * 60);

  send_request(make_object<td_api::answerCallbackQuery>(callback_query_id, text, show_alert, url, cache_time),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return Status::OK();
}

td::Status Client::process_answer_shipping_query_query(PromisedQueryPtr &query) {
  auto shipping_query_id = td::to_integer<int64>(query->arg("shipping_query_id"));
  auto ok = to_bool(query->arg("ok"));
  td::vector<object_ptr<td_api::shippingOption>> shipping_options;
  td::MutableSlice error_message;
  if (ok) {
    TRY_RESULT_ASSIGN(shipping_options, get_shipping_options(query.get()));
  } else {
    TRY_RESULT_ASSIGN(error_message, get_required_string_arg(query.get(), "error_message"));
  }
  send_request(
      make_object<td_api::answerShippingQuery>(shipping_query_id, std::move(shipping_options), error_message.str()),
      td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return Status::OK();
}

td::Status Client::process_answer_pre_checkout_query_query(PromisedQueryPtr &query) {
  auto pre_checkout_query_id = td::to_integer<int64>(query->arg("pre_checkout_query_id"));
  auto ok = to_bool(query->arg("ok"));
  td::MutableSlice error_message;
  if (!ok) {
    TRY_RESULT_ASSIGN(error_message, get_required_string_arg(query.get(), "error_message"));
  }

  send_request(make_object<td_api::answerPreCheckoutQuery>(pre_checkout_query_id, error_message.str()),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return Status::OK();
}

td::Status Client::process_export_chat_invite_link_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::Write, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    send_request(make_object<td_api::replacePrimaryChatInviteLink>(chat_id),
                 td::make_unique<TdOnReplacePrimaryChatInviteLinkCallback>(std::move(query)));
  });
  return Status::OK();
}

td::Status Client::process_create_chat_invite_link_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto name = query->arg("name");
  auto expire_date = get_integer_arg(query.get(), "expire_date", 0, 0);
  auto member_limit = get_integer_arg(query.get(), "member_limit", 0, 0, 100000);
  auto creates_join_request = to_bool(query->arg("creates_join_request"));

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, name = name.str(), expire_date, member_limit, creates_join_request](int64 chat_id,
                                                                                        PromisedQueryPtr query) {
               send_request(make_object<td_api::createChatInviteLink>(chat_id, name, expire_date, member_limit,
                                                                      creates_join_request),
                            td::make_unique<TdOnGetChatInviteLinkCallback>(this, std::move(query)));
             });
  return Status::OK();
}

td::Status Client::process_edit_chat_invite_link_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto invite_link = query->arg("invite_link");
  auto name = query->arg("name");
  auto expire_date = get_integer_arg(query.get(), "expire_date", 0, 0);
  auto member_limit = get_integer_arg(query.get(), "member_limit", 0, 0, 100000);
  auto creates_join_request = to_bool(query->arg("creates_join_request"));

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, invite_link = invite_link.str(), name = name.str(), expire_date, member_limit,
              creates_join_request](int64 chat_id, PromisedQueryPtr query) {
               send_request(make_object<td_api::editChatInviteLink>(chat_id, invite_link, name, expire_date,
                                                                    member_limit, creates_join_request),
                            td::make_unique<TdOnGetChatInviteLinkCallback>(this, std::move(query)));
             });
  return Status::OK();
}

td::Status Client::process_revoke_chat_invite_link_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto invite_link = query->arg("invite_link");

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, invite_link = invite_link.str()](int64 chat_id, PromisedQueryPtr query) {
               send_request(make_object<td_api::revokeChatInviteLink>(chat_id, invite_link),
                            td::make_unique<TdOnGetChatInviteLinkCallback>(this, std::move(query)));
             });
  return Status::OK();
}

td::Status Client::process_get_chat_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::Read, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    auto chat_info = get_chat(chat_id);
    CHECK(chat_info != nullptr);
    switch (chat_info->type) {
      case ChatInfo::Type::Private:
        return send_request(make_object<td_api::getUserFullInfo>(chat_info->user_id),
                            td::make_unique<TdOnGetChatFullInfoCallback>(this, chat_id, std::move(query)));
      case ChatInfo::Type::Group:
        return send_request(make_object<td_api::getBasicGroupFullInfo>(chat_info->group_id),
                            td::make_unique<TdOnGetChatFullInfoCallback>(this, chat_id, std::move(query)));
      case ChatInfo::Type::Supergroup:
        return send_request(make_object<td_api::getSupergroupFullInfo>(chat_info->supergroup_id),
                            td::make_unique<TdOnGetChatFullInfoCallback>(this, chat_id, std::move(query)));
      case ChatInfo::Type::Unknown:
      default:
        UNREACHABLE();
    }
  });
  return Status::OK();
}

td::Status Client::process_set_chat_photo_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto photo = get_input_file(query.get(), "photo", true);
  if (photo == nullptr) {
    if (query->arg("photo").empty()) {
      return Status::Error(400, "There is no photo in the request");
    }
    return Status::Error(400, "Photo must be uploaded as an InputFile");
  }

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, photo = std::move(photo)](int64 chat_id, PromisedQueryPtr query) mutable {
               send_request(make_object<td_api::setChatPhoto>(
                                chat_id, make_object<td_api::inputChatPhotoStatic>(std::move(photo))),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return Status::OK();
}

td::Status Client::process_delete_chat_photo_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::Write, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    send_request(make_object<td_api::setChatPhoto>(chat_id, nullptr),
                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  });
  return Status::OK();
}

td::Status Client::process_set_chat_title_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto title = query->arg("title");

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, title = title.str()](int64 chat_id, PromisedQueryPtr query) {
               send_request(make_object<td_api::setChatTitle>(chat_id, title),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return Status::OK();
}

td::Status Client::process_set_chat_permissions_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  bool allow_legacy = false;
  TRY_RESULT(permissions, get_chat_permissions(query.get(), allow_legacy));
  CHECK(!allow_legacy);

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, permissions = std::move(permissions)](int64 chat_id, PromisedQueryPtr query) mutable {
               send_request(make_object<td_api::setChatPermissions>(chat_id, std::move(permissions)),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return Status::OK();
}

td::Status Client::process_set_chat_description_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto description = query->arg("description");

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, description = description.str()](int64 chat_id, PromisedQueryPtr query) {
               send_request(make_object<td_api::setChatDescription>(chat_id, description),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return Status::OK();
}

td::Status Client::process_pin_chat_message_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  auto disable_notification = to_bool(query->arg("disable_notification"));

  check_message(chat_id, message_id, false, AccessRights::Write, "message to pin", std::move(query),
                [this, disable_notification](int64 chat_id, int64 message_id, PromisedQueryPtr query) {
                  send_request(make_object<td_api::pinChatMessage>(chat_id, message_id, disable_notification, false),
                               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                });
  return Status::OK();
}

td::Status Client::process_unpin_chat_message_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());

  if (message_id == 0) {
    check_chat(chat_id, AccessRights::Write, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
      send_request(make_object<td_api::getChatPinnedMessage>(chat_id),
                   td::make_unique<TdOnGetChatPinnedMessageToUnpinCallback>(this, chat_id, std::move(query)));
    });
  } else {
    check_message(chat_id, message_id, false, AccessRights::Write, "message to unpin", std::move(query),
                  [this](int64 chat_id, int64 message_id, PromisedQueryPtr query) {
                    send_request(make_object<td_api::unpinChatMessage>(chat_id, message_id),
                                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                  });
  }
  return Status::OK();
}

td::Status Client::process_unpin_all_chat_messages_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::Write, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    send_request(make_object<td_api::unpinAllChatMessages>(chat_id),
                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  });
  return Status::OK();
}

td::Status Client::process_set_chat_sticker_set_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto sticker_set_name = query->arg("sticker_set_name");

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, sticker_set_name = sticker_set_name.str()](int64 chat_id, PromisedQueryPtr query) {
               if (get_chat_type(chat_id) != ChatType::Supergroup) {
                 return fail_query(400, "Bad Request: method is available only for supergroups", std::move(query));
               }

               resolve_sticker_set(
                   sticker_set_name, std::move(query), [this, chat_id](int64 sticker_set_id, PromisedQueryPtr query) {
                     auto chat_info = get_chat(chat_id);
                     CHECK(chat_info != nullptr);
                     CHECK(chat_info->type == ChatInfo::Type::Supergroup);
                     send_request(
                         make_object<td_api::setSupergroupStickerSet>(chat_info->supergroup_id, sticker_set_id),
                         td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                   });
             });
  return Status::OK();
}

td::Status Client::process_delete_chat_sticker_set_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::Write, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    if (get_chat_type(chat_id) != ChatType::Supergroup) {
      return fail_query(400, "Bad Request: method is available only for supergroups", std::move(query));
    }

    auto chat_info = get_chat(chat_id);
    CHECK(chat_info != nullptr);
    CHECK(chat_info->type == ChatInfo::Type::Supergroup);
    send_request(make_object<td_api::setSupergroupStickerSet>(chat_info->supergroup_id, 0),
                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  });
  return Status::OK();
}

td::Status Client::process_get_chat_member_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(user_id, get_user_id(query.get()));

  check_chat(chat_id, AccessRights::ReadMembers, std::move(query),
             [this, user_id](int64 chat_id, PromisedQueryPtr query) {
               get_chat_member(chat_id, user_id, std::move(query),
                               [this, chat_type = get_chat_type(chat_id)](object_ptr<td_api::chatMember> &&chat_member,
                                                                          PromisedQueryPtr query) {
                                 answer_query(JsonChatMember(chat_member.get(), chat_type, this), std::move(query));
                               });
             });
  return Status::OK();
}

td::Status Client::process_get_chat_administrators_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::ReadMembers, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    auto chat_info = get_chat(chat_id);
    CHECK(chat_info != nullptr);
    switch (chat_info->type) {
      case ChatInfo::Type::Private:
        return fail_query(400, "Bad Request: there are no administrators in the private chat", std::move(query));
      case ChatInfo::Type::Group:
        return send_request(make_object<td_api::getBasicGroupFullInfo>(chat_info->group_id),
                            td::make_unique<TdOnGetGroupMembersCallback>(this, true, std::move(query)));
      case ChatInfo::Type::Supergroup:
        return send_request(
            make_object<td_api::getSupergroupMembers>(
                chat_info->supergroup_id, make_object<td_api::supergroupMembersFilterAdministrators>(), 0, 100),
            td::make_unique<TdOnGetSupergroupMembersCallback>(this, get_chat_type(chat_id), std::move(query)));
      case ChatInfo::Type::Unknown:
      default:
        UNREACHABLE();
    }
  });
  return Status::OK();
}

td::Status Client::process_get_chat_member_count_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::ReadMembers, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    auto chat_info = get_chat(chat_id);
    CHECK(chat_info != nullptr);
    switch (chat_info->type) {
      case ChatInfo::Type::Private:
        return answer_query(td::VirtuallyJsonableInt(1 + (chat_info->user_id != my_id_)), std::move(query));
      case ChatInfo::Type::Group: {
        auto group_info = get_group_info(chat_info->group_id);
        CHECK(group_info != nullptr);
        return answer_query(td::VirtuallyJsonableInt(group_info->member_count), std::move(query));
      }
      case ChatInfo::Type::Supergroup:
        return send_request(make_object<td_api::getSupergroupFullInfo>(chat_info->supergroup_id),
                            td::make_unique<TdOnGetSupergroupMembersCountCallback>(std::move(query)));
      case ChatInfo::Type::Unknown:
      default:
        UNREACHABLE();
    }
  });
  return Status::OK();
}

td::Status Client::process_leave_chat_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::Read, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    send_request(make_object<td_api::leaveChat>(chat_id), td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  });
  return Status::OK();
}

td::Status Client::process_promote_chat_member_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto can_manage_chat = to_bool(query->arg("can_manage_chat"));
  auto can_change_info = to_bool(query->arg("can_change_info"));
  auto can_post_messages = to_bool(query->arg("can_post_messages"));
  auto can_edit_messages = to_bool(query->arg("can_edit_messages"));
  auto can_delete_messages = to_bool(query->arg("can_delete_messages"));
  auto can_invite_users = to_bool(query->arg("can_invite_users"));
  auto can_restrict_members = to_bool(query->arg("can_restrict_members"));
  auto can_pin_messages = to_bool(query->arg("can_pin_messages"));
  auto can_promote_members = to_bool(query->arg("can_promote_members"));
  auto can_manage_video_chats =
      to_bool(query->arg("can_manage_voice_chats")) || to_bool(query->arg("can_manage_video_chats"));
  auto is_anonymous = to_bool(query->arg("is_anonymous"));
  auto status = make_object<td_api::chatMemberStatusAdministrator>(
      td::string(), true,
      make_object<td_api::chatAdministratorRights>(
          can_manage_chat, can_change_info, can_post_messages, can_edit_messages, can_delete_messages, can_invite_users,
          can_restrict_members, can_pin_messages, can_promote_members, can_manage_video_chats, is_anonymous));
  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, user_id, status = std::move(status)](int64 chat_id, PromisedQueryPtr query) mutable {
               auto chat_info = get_chat(chat_id);
               CHECK(chat_info != nullptr);
               if (chat_info->type != ChatInfo::Type::Supergroup) {
                 return fail_query(400, "Bad Request: method is available for supergroup and channel chats only",
                                   std::move(query));
               }

               get_chat_member(
                   chat_id, user_id, std::move(query),
                   [this, chat_id, user_id, status = std::move(status)](object_ptr<td_api::chatMember> &&chat_member,
                                                                        PromisedQueryPtr query) mutable {
                     if (chat_member->status_->get_id() == td_api::chatMemberStatusAdministrator::ID) {
                       auto administrator =
                           static_cast<const td_api::chatMemberStatusAdministrator *>(chat_member->status_.get());
                       status->custom_title_ = std::move(administrator->custom_title_);
                     }

                     send_request(make_object<td_api::setChatMemberStatus>(
                                      chat_id, make_object<td_api::messageSenderUser>(user_id), std::move(status)),
                                  td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                   });
             });
  return Status::OK();
}

td::Status Client::process_set_chat_administrator_custom_title_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(user_id, get_user_id(query.get()));

  check_chat(chat_id, AccessRights::Write, std::move(query), [this, user_id](int64 chat_id, PromisedQueryPtr query) {
    if (get_chat_type(chat_id) != ChatType::Supergroup) {
      return fail_query(400, "Bad Request: method is available only for supergroups", std::move(query));
    }

    get_chat_member(
        chat_id, user_id, std::move(query),
        [this, chat_id, user_id](object_ptr<td_api::chatMember> &&chat_member, PromisedQueryPtr query) {
          if (chat_member->status_->get_id() == td_api::chatMemberStatusCreator::ID) {
            return fail_query(400, "Bad Request: only creator can edit their custom title", std::move(query));
          }
          if (chat_member->status_->get_id() != td_api::chatMemberStatusAdministrator::ID) {
            return fail_query(400, "Bad Request: user is not an administrator", std::move(query));
          }
          auto administrator = td_api::move_object_as<td_api::chatMemberStatusAdministrator>(chat_member->status_);
          if (!administrator->can_be_edited_) {
            return fail_query(400, "Bad Request: not enough rights to change custom title of the user",
                              std::move(query));
          }
          administrator->custom_title_ = query->arg("custom_title").str();

          send_request(make_object<td_api::setChatMemberStatus>(
                           chat_id, make_object<td_api::messageSenderUser>(user_id), std::move(administrator)),
                       td::make_unique<TdOnOkQueryCallback>(std::move(query)));
        });
  });
  return Status::OK();
}

td::Status Client::process_ban_chat_member_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(user_id, get_user_id(query.get()));
  int32 until_date = get_integer_arg(query.get(), "until_date", 0);
  auto revoke_messages = to_bool(query->arg("revoke_messages"));

  check_chat(
      chat_id, AccessRights::Write, std::move(query),
      [this, user_id, until_date, revoke_messages](int64 chat_id, PromisedQueryPtr query) {
        check_user_no_fail(
            user_id, std::move(query), [this, chat_id, user_id, until_date, revoke_messages](PromisedQueryPtr query) {
              send_request(make_object<td_api::banChatMember>(chat_id, make_object<td_api::messageSenderUser>(user_id),
                                                              until_date, revoke_messages),
                           td::make_unique<TdOnOkQueryCallback>(std::move(query)));
            });
      });
  return Status::OK();
}

td::Status Client::process_restrict_chat_member_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(user_id, get_user_id(query.get()));
  int32 until_date = get_integer_arg(query.get(), "until_date", 0);
  bool allow_legacy = true;
  TRY_RESULT(permissions, get_chat_permissions(query.get(), allow_legacy));

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, user_id, until_date, is_legacy = allow_legacy, permissions = std::move(permissions)](
                 int64 chat_id, PromisedQueryPtr query) mutable {
               if (get_chat_type(chat_id) != ChatType::Supergroup) {
                 return fail_query(400, "Bad Request: method is available only for supergroups", std::move(query));
               }

               get_chat_member(
                   chat_id, user_id, std::move(query),
                   [this, chat_id, user_id, until_date, is_legacy, permissions = std::move(permissions)](
                       object_ptr<td_api::chatMember> &&chat_member, PromisedQueryPtr query) mutable {
                     if (is_legacy && chat_member->status_->get_id() == td_api::chatMemberStatusRestricted::ID) {
                       auto restricted =
                           static_cast<const td_api::chatMemberStatusRestricted *>(chat_member->status_.get());
                       auto *old_permissions = restricted->permissions_.get();
                       permissions->can_send_polls_ = old_permissions->can_send_polls_;
                       permissions->can_change_info_ = old_permissions->can_change_info_;
                       permissions->can_invite_users_ = old_permissions->can_invite_users_;
                       permissions->can_pin_messages_ = old_permissions->can_pin_messages_;
                     }

                     send_request(make_object<td_api::setChatMemberStatus>(
                                      chat_id, make_object<td_api::messageSenderUser>(user_id),
                                      make_object<td_api::chatMemberStatusRestricted>(
                                          is_chat_member(chat_member->status_), until_date, std::move(permissions))),
                                  td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                   });
             });
  return Status::OK();
}

td::Status Client::process_unban_chat_member_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto only_if_banned = to_bool(query->arg("only_if_banned"));

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, user_id, only_if_banned](int64 chat_id, PromisedQueryPtr query) {
               auto chat_info = get_chat(chat_id);
               CHECK(chat_info != nullptr);
               if (chat_info->type != ChatInfo::Type::Supergroup) {
                 return fail_query(400, "Bad Request: method is available for supergroup and channel chats only",
                                   std::move(query));
               }

               if (only_if_banned) {
                 get_chat_member(
                     chat_id, user_id, std::move(query),
                     [this, chat_id, user_id](object_ptr<td_api::chatMember> &&chat_member, PromisedQueryPtr query) {
                       if (chat_member->status_->get_id() != td_api::chatMemberStatusBanned::ID) {
                         return answer_query(td::JsonTrue(), std::move(query));
                       }

                       send_request(make_object<td_api::setChatMemberStatus>(
                                        chat_id, make_object<td_api::messageSenderUser>(user_id),
                                        make_object<td_api::chatMemberStatusLeft>()),
                                    td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                     });
               } else {
                 check_user_no_fail(user_id, std::move(query), [this, chat_id, user_id](PromisedQueryPtr query) {
                   send_request(make_object<td_api::setChatMemberStatus>(
                                    chat_id, make_object<td_api::messageSenderUser>(user_id),
                                    make_object<td_api::chatMemberStatusLeft>()),
                                td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                 });
               }
             });
  return Status::OK();
}

td::Status Client::process_ban_chat_sender_chat_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto sender_chat_id = query->arg("sender_chat_id");
  int32 until_date = get_integer_arg(query.get(), "until_date", 0);

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, sender_chat_id = sender_chat_id.str(), until_date](int64 chat_id, PromisedQueryPtr query) {
               check_chat_no_fail(sender_chat_id, std::move(query),
                                  [this, chat_id, until_date](int64 sender_chat_id, PromisedQueryPtr query) {
                                    send_request(make_object<td_api::banChatMember>(
                                                     chat_id, make_object<td_api::messageSenderChat>(sender_chat_id),
                                                     until_date, false),
                                                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                                  });
             });
  return Status::OK();
}

td::Status Client::process_unban_chat_sender_chat_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto sender_chat_id = query->arg("sender_chat_id");

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, sender_chat_id = sender_chat_id.str()](int64 chat_id, PromisedQueryPtr query) {
               check_chat_no_fail(sender_chat_id, std::move(query),
                                  [this, chat_id](int64 sender_chat_id, PromisedQueryPtr query) {
                                    send_request(make_object<td_api::setChatMemberStatus>(
                                                     chat_id, make_object<td_api::messageSenderChat>(sender_chat_id),
                                                     make_object<td_api::chatMemberStatusLeft>()),
                                                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                                  });
             });
  return Status::OK();
}

td::Status Client::process_approve_chat_join_request_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(user_id, get_user_id(query.get()));

  check_chat(chat_id, AccessRights::Write, std::move(query), [this, user_id](int64 chat_id, PromisedQueryPtr query) {
    check_user_no_fail(user_id, std::move(query), [this, chat_id, user_id](PromisedQueryPtr query) {
      send_request(make_object<td_api::processChatJoinRequest>(chat_id, user_id, true),
                   td::make_unique<TdOnOkQueryCallback>(std::move(query)));
    });
  });
  return Status::OK();
}

td::Status Client::process_decline_chat_join_request_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(user_id, get_user_id(query.get()));

  check_chat(chat_id, AccessRights::Write, std::move(query), [this, user_id](int64 chat_id, PromisedQueryPtr query) {
    check_user_no_fail(user_id, std::move(query), [this, chat_id, user_id](PromisedQueryPtr query) {
      send_request(make_object<td_api::processChatJoinRequest>(chat_id, user_id, false),
                   td::make_unique<TdOnOkQueryCallback>(std::move(query)));
    });
  });
  return Status::OK();
}

td::Status Client::process_get_sticker_set_query(PromisedQueryPtr &query) {
  auto name = query->arg("name");
  if (td::trim(to_lower(name)) == to_lower(GREAT_MINDS_SET_NAME)) {
    send_request(make_object<td_api::getStickerSet>(GREAT_MINDS_SET_ID),
                 td::make_unique<TdOnReturnStickerSetCallback>(this, true, std::move(query)));
  } else {
    send_request(make_object<td_api::searchStickerSet>(name.str()),
                 td::make_unique<TdOnReturnStickerSetCallback>(this, true, std::move(query)));
  }
  return Status::OK();
}

td::Status Client::process_upload_sticker_file_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto png_sticker = get_input_file(query.get(), "png_sticker");

  check_user(user_id, std::move(query),
             [this, user_id, png_sticker = std::move(png_sticker)](PromisedQueryPtr query) mutable {
               send_request(make_object<td_api::uploadStickerFile>(
                                user_id, make_object<td_api::inputSticker>(std::move(png_sticker), "",
                                                                           make_object<td_api::stickerTypeStatic>())),
                            td::make_unique<TdOnReturnFileCallback>(this, std::move(query)));
             });
  return Status::OK();
}

td::Status Client::process_create_new_sticker_set_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto name = query->arg("name");
  auto title = query->arg("title");
  auto is_masks = to_bool(query->arg("contains_masks"));
  TRY_RESULT(stickers, get_input_stickers(query.get(), is_masks));

  check_user(user_id, std::move(query),
             [this, user_id, title, name, stickers = std::move(stickers)](PromisedQueryPtr query) mutable {
               send_request(make_object<td_api::createNewStickerSet>(user_id, title.str(), name.str(),
                                                                     std::move(stickers), PSTRING() << "bot" << my_id_),
                            td::make_unique<TdOnReturnStickerSetCallback>(this, false, std::move(query)));
             });
  return Status::OK();
}

td::Status Client::process_add_sticker_to_set_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto name = query->arg("name");
  TRY_RESULT(stickers, get_input_stickers(query.get(), true));
  CHECK(!stickers.empty());

  check_user(user_id, std::move(query),
             [this, user_id, name, sticker = std::move(stickers[0])](PromisedQueryPtr query) mutable {
               send_request(make_object<td_api::addStickerToSet>(user_id, name.str(), std::move(sticker)),
                            td::make_unique<TdOnReturnStickerSetCallback>(this, false, std::move(query)));
             });
  return Status::OK();
}

td::Status Client::process_set_sticker_set_thumb_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto name = query->arg("name");
  auto thumbnail = get_input_file(query.get(), "thumb");
  check_user(user_id, std::move(query),
             [this, user_id, name, thumbnail = std::move(thumbnail)](PromisedQueryPtr query) mutable {
               send_request(make_object<td_api::setStickerSetThumbnail>(user_id, name.str(), std::move(thumbnail)),
                            td::make_unique<TdOnReturnStickerSetCallback>(this, false, std::move(query)));
             });
  return Status::OK();
}

td::Status Client::process_set_sticker_position_in_set_query(PromisedQueryPtr &query) {
  auto file_id = trim(query->arg("sticker"));
  if (file_id.empty()) {
    return Status::Error(400, "Sticker is not specified");
  }
  int32 position = get_integer_arg(query.get(), "position", -1);

  send_request(
      make_object<td_api::setStickerPositionInSet>(make_object<td_api::inputFileRemote>(file_id.str()), position),
      td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return Status::OK();
}

td::Status Client::process_delete_sticker_from_set_query(PromisedQueryPtr &query) {
  auto file_id = trim(query->arg("sticker"));
  if (file_id.empty()) {
    return Status::Error(400, "Sticker is not specified");
  }

  send_request(make_object<td_api::removeStickerFromSet>(make_object<td_api::inputFileRemote>(file_id.str())),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return Status::OK();
}

td::Status Client::process_set_passport_data_errors_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  TRY_RESULT(passport_element_errors, get_passport_element_errors(query.get()));

  check_user(user_id, std::move(query),
             [this, user_id, errors = std::move(passport_element_errors)](PromisedQueryPtr query) mutable {
               send_request(make_object<td_api::setPassportElementErrors>(user_id, std::move(errors)),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return Status::OK();
}

td::Status Client::process_send_custom_request_query(PromisedQueryPtr &query) {
  TRY_RESULT(method, get_required_string_arg(query.get(), "method"));
  auto parameters = query->arg("parameters");
  send_request(make_object<td_api::sendCustomRequest>(method.str(), parameters.str()),
               td::make_unique<TdOnSendCustomRequestCallback>(std::move(query)));
  return Status::OK();
}

td::Status Client::process_answer_custom_query_query(PromisedQueryPtr &query) {
  auto custom_query_id = td::to_integer<int64>(query->arg("custom_query_id"));
  auto data = query->arg("data");
  send_request(make_object<td_api::answerCustomQuery>(custom_query_id, data.str()),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return Status::OK();
}

td::Status Client::process_get_updates_query(PromisedQueryPtr &query) {
  if (!webhook_url_.empty() || webhook_set_query_) {
    fail_query_conflict(
        "Conflict: can't use getUpdates method while webhook is active; use deleteWebhook to delete the webhook first",
        std::move(query));
    return Status::OK();
  }
  int32 offset = get_integer_arg(query.get(), "offset", 0);
  int32 limit = get_integer_arg(query.get(), "limit", 100, 1, 100);
  int32 timeout = get_integer_arg(query.get(), "timeout", 0, 0, LONG_POLL_MAX_TIMEOUT);

  update_allowed_update_types(query.get());

  auto now = td::Time::now_cached();
  if (offset == previous_get_updates_offset_ && timeout < 3 && now < previous_get_updates_start_time_ + 3.0) {
    timeout = 3;
  }
  previous_get_updates_offset_ = offset;
  previous_get_updates_start_time_ = now;
  do_get_updates(offset, limit, timeout, std::move(query));
  return Status::OK();
}

td::Status Client::process_set_webhook_query(PromisedQueryPtr &query) {
  Slice new_url;
  if (query->method() == "setwebhook") {
    new_url = query->arg("url");
  }

  auto now = td::Time::now_cached();
  if (!new_url.empty() && !query->is_internal()) {
    if (now < next_allowed_set_webhook_time_) {
      query->set_retry_after_error(1);
      return Status::OK();
    }
    next_allowed_set_webhook_time_ = now + 1;
  }

  // do not send warning just after webhook was deleted or set
  next_bot_updates_warning_time_ = td::max(next_bot_updates_warning_time_, now + BOT_UPDATES_WARNING_DELAY);

  bool new_has_certificate = new_url.empty() ? false
                                             : (get_webhook_certificate(query.get()) != nullptr ||
                                                (query->is_internal() && query->arg("certificate") == "previous"));
  int32 new_max_connections = new_url.empty() ? 0 : get_webhook_max_connections(query.get());
  Slice new_ip_address = new_url.empty() ? Slice() : query->arg("ip_address");
  bool new_fix_ip_address = new_url.empty() ? false : get_webhook_fix_ip_address(query.get());
  Slice new_secret_token = new_url.empty() ? Slice() : query->arg("secret_token");
  bool drop_pending_updates = to_bool(query->arg("drop_pending_updates"));
  if (webhook_set_query_) {
    // already updating webhook. Cancel previous request
    fail_query_conflict("Conflict: terminated by other setWebhook", std::move(webhook_set_query_));
  } else if (webhook_url_ == new_url && !has_webhook_certificate_ && !new_has_certificate &&
             new_max_connections == webhook_max_connections_ && new_fix_ip_address == webhook_fix_ip_address_ &&
             new_secret_token == webhook_secret_token_ &&
             (!new_fix_ip_address || new_ip_address == webhook_ip_address_) && !drop_pending_updates) {
    if (update_allowed_update_types(query.get())) {
      save_webhook();
    } else if (now > next_webhook_is_not_modified_warning_time_) {
      next_webhook_is_not_modified_warning_time_ = now + 300;
      LOG(WARNING) << "Webhook is not modified: \"" << new_url << '"';
    }
    answer_query(td::JsonTrue(), std::move(query),
                 new_url.empty() ? Slice("Webhook is already deleted") : Slice("Webhook is already set"));
    return Status::OK();
  }

  if (now > next_set_webhook_logging_time_ || webhook_url_ != new_url) {
    next_set_webhook_logging_time_ = now + 300;
    LOG(WARNING) << "Set webhook to " << new_url << ", max_connections = " << new_max_connections
                 << ", IP address = " << new_ip_address;
  }

  if (!new_url.empty()) {
    abort_long_poll(true);
  }

  webhook_generation_++;
  // need to close old webhook first
  if (!webhook_url_.empty()) {
    if (!webhook_id_.empty()) {
      send_closure_later(std::move(webhook_id_), &WebhookActor::close);
    }

    // wait for webhook_close callback
    webhook_query_type_ = WebhookQueryType::Cancel;
    webhook_set_query_ = std::move(query);
    return Status::OK();
  }
  do_set_webhook(std::move(query), false);
  return Status::OK();
}

td::Status Client::process_get_webhook_info_query(PromisedQueryPtr &query) {
  update_last_synchronization_error_date();
  answer_query(JsonWebhookInfo(this), std::move(query));
  return Status::OK();
}

td::Status Client::process_get_file_query(PromisedQueryPtr &query) {
  td::string file_id = query->arg("file_id").str();
  check_remote_file_id(file_id, std::move(query), [this](object_ptr<td_api::file> file, PromisedQueryPtr query) {
    do_get_file(std::move(file), std::move(query));
  });
  return Status::OK();
}

void Client::do_get_file(object_ptr<td_api::file> file, PromisedQueryPtr query) {
  if (!parameters_->local_mode_ &&
      td::max(file->expected_size_, file->local_->downloaded_size_) > MAX_DOWNLOAD_FILE_SIZE) {  // speculative check
    return fail_query(400, "Bad Request: file is too big", std::move(query));
  }

  auto file_id = file->id_;
  file_download_listeners_[file_id].push_back(std::move(query));
  if (file->local_->is_downloading_completed_) {
    Slice relative_path = td::PathView::relative(file->local_->path_, dir_, true);
    if (!relative_path.empty()) {
      auto r_stat = td::stat(file->local_->path_);
      if (r_stat.is_ok() && r_stat.ok().is_reg_ && r_stat.ok().size_ == file->size_) {
        return on_file_download(file_id, std::move(file));
      }
    }
  }

  send_request(make_object<td_api::downloadFile>(file_id, 1, 0, 0, false),
               td::make_unique<TdOnDownloadFileCallback>(this, file_id));
}

bool Client::is_file_being_downloaded(int32 file_id) const {
  return file_download_listeners_.count(file_id) > 0;
}

void Client::on_file_download(int32 file_id, td::Result<object_ptr<td_api::file>> r_file) {
  auto it = file_download_listeners_.find(file_id);
  if (it == file_download_listeners_.end()) {
    return;
  }
  auto queries = std::move(it->second);
  file_download_listeners_.erase(it);
  download_started_file_ids_.erase(file_id);
  for (auto &query : queries) {
    if (r_file.is_error()) {
      const auto &error = r_file.error();
      fail_query_with_error(std::move(query), error.code(), error.public_message());
    } else {
      answer_query(JsonFile(r_file.ok().get(), this, true), std::move(query));
    }
  }
}

void Client::webhook_verified(td::string cached_ip_address) {
  if (get_link_token() != webhook_generation_) {
    return;
  }
  bool need_save = webhook_set_query_ || cached_ip_address != webhook_ip_address_;
  webhook_ip_address_ = cached_ip_address;
  if (webhook_set_query_) {
    LOG(WARNING) << "Webhook verified";
    answer_query(td::JsonTrue(), std::move(webhook_set_query_), "Webhook was set");
  }
  if (need_save) {
    save_webhook();
  }
}

void Client::save_webhook() const {
  td::string value;
  if (has_webhook_certificate_) {
    value += "cert/";
  }
  value += PSTRING() << "#maxc" << webhook_max_connections_ << '/';
  if (!webhook_ip_address_.empty()) {
    value += PSTRING() << "#ip" << webhook_ip_address_ << '/';
  }
  if (webhook_fix_ip_address_) {
    value += "#fix_ip/";
  }
  if (!webhook_secret_token_.empty()) {
    value += PSTRING() << "#secret" << webhook_secret_token_ << '/';
  }
  if (allowed_update_types_ != DEFAULT_ALLOWED_UPDATE_TYPES) {
    value += PSTRING() << "#allow" << allowed_update_types_ << '/';
  }
  value += webhook_url_;
  LOG(INFO) << "Save webhook " << value;
  parameters_->shared_data_->webhook_db_->set(bot_token_with_dc_, value);
}

void Client::webhook_success() {
  next_bot_updates_warning_time_ = td::Time::now() + BOT_UPDATES_WARNING_DELAY;
  if (was_bot_updates_warning_) {
    send_request(make_object<td_api::setBotUpdatesStatus>(0, ""), td::make_unique<TdOnOkCallback>());
    was_bot_updates_warning_ = false;
  }
}

void Client::webhook_error(Status status) {
  CHECK(status.is_error());
  last_webhook_error_date_ = get_unix_time();
  last_webhook_error_ = std::move(status);

  auto pending_update_count = get_pending_update_count();
  if (pending_update_count >= MIN_PENDING_UPDATES_WARNING && td::Time::now() > next_bot_updates_warning_time_) {
    send_request(make_object<td_api::setBotUpdatesStatus>(td::narrow_cast<int32>(pending_update_count),
                                                          "Webhook error. " + last_webhook_error_.message().str()),
                 td::make_unique<TdOnOkCallback>());
    next_bot_updates_warning_time_ = td::Time::now_cached() + BOT_UPDATES_WARNING_DELAY;
    was_bot_updates_warning_ = true;
  }
}

void Client::webhook_closed(Status status) {
  LOG(WARNING) << "Webhook closed: " << status
               << ", webhook_query_type = " << (webhook_query_type_ == WebhookQueryType::Verify ? "verify" : "change");
  webhook_id_.release();
  webhook_url_ = td::string();
  if (has_webhook_certificate_) {
    td::unlink(get_webhook_certificate_path()).ignore();
    has_webhook_certificate_ = false;
  }
  webhook_max_connections_ = 0;
  webhook_ip_address_ = td::string();
  webhook_fix_ip_address_ = false;
  webhook_secret_token_ = td::string();
  webhook_set_time_ = td::Time::now();
  last_webhook_error_date_ = 0;
  last_webhook_error_ = Status::OK();
  parameters_->shared_data_->webhook_db_->erase(bot_token_with_dc_);

  if (webhook_set_query_) {
    if (webhook_query_type_ == WebhookQueryType::Verify) {
      fail_query(400, PSLICE() << "Bad Request: bad webhook: " << status.message(), std::move(webhook_set_query_));
    } else {
      do_set_webhook(std::move(webhook_set_query_), true);
    }
  }
}

void Client::hangup_shared() {
  webhook_closed(Status::Error("Unknown"));
}

td::string Client::get_webhook_certificate_path() const {
  return dir_ + "cert.pem";
}

const td::HttpFile *Client::get_webhook_certificate(const Query *query) const {
  auto file = query->file("certificate");
  if (file == nullptr) {
    auto attach_name = query->arg("certificate");
    Slice attach_protocol{"attach://"};
    if (td::begins_with(attach_name, attach_protocol)) {
      file = query->file(attach_name.substr(attach_protocol.size()));
    }
  }
  return file;
}

td::int32 Client::get_webhook_max_connections(const Query *query) const {
  auto default_value = parameters_->default_max_webhook_connections_;
  auto max_value = parameters_->local_mode_ ? 100000 : 100;
  return get_integer_arg(query, "max_connections", default_value, 1, max_value);
}

bool Client::get_webhook_fix_ip_address(const Query *query) {
  if (query->is_internal()) {
    return query->has_arg("fix_ip_address");
  }
  return !query->arg("ip_address").empty();
}

void Client::do_set_webhook(PromisedQueryPtr query, bool was_deleted) {
  CHECK(webhook_url_.empty());
  if (to_bool(query->arg("drop_pending_updates"))) {
    clear_tqueue();
  }
  Slice new_url;
  if (query->method() == "setwebhook") {
    new_url = query->arg("url");
  }
  if (!new_url.empty()) {
    auto url = td::parse_url(new_url, td::HttpUrl::Protocol::Https);
    if (url.is_error()) {
      return fail_query(400, "Bad Request: invalid webhook URL specified", std::move(query));
    }
    auto secret_token = query->arg("secret_token");
    if (secret_token.size() > 256) {
      return fail_query(400, "Bad Request: secret token is too long", std::move(query));
    }
    if (!td::is_base64url_characters(secret_token)) {
      return fail_query(400, "Bad Request: secret token contains unallowed characters", std::move(query));
    }

    has_webhook_certificate_ = false;
    auto *cert_file_ptr = get_webhook_certificate(query.get());
    if (cert_file_ptr != nullptr) {
      auto size = cert_file_ptr->size;
      if (size > MAX_CERTIFICATE_FILE_SIZE) {
        return fail_query(400, PSLICE() << "Bad Request: certificate size is too big (" << size << " bytes)",
                          std::move(query));
      }
      auto from_path = cert_file_ptr->temp_file_name;
      auto to_path = get_webhook_certificate_path();
      auto status = td::copy_file(from_path, to_path, size);
      if (status.is_error()) {
        return fail_query(500, "Internal Server Error: failed to save certificate", std::move(query));
      }
      has_webhook_certificate_ = true;
    } else if (query->is_internal() && query->arg("certificate") == "previous") {
      has_webhook_certificate_ = true;
    }

    webhook_url_ = new_url.str();
    webhook_set_time_ = td::Time::now();
    webhook_max_connections_ = get_webhook_max_connections(query.get());
    webhook_secret_token_ = secret_token.str();
    webhook_ip_address_ = query->arg("ip_address").str();
    webhook_fix_ip_address_ = get_webhook_fix_ip_address(query.get());
    last_webhook_error_date_ = 0;
    last_webhook_error_ = Status::OK();

    update_allowed_update_types(query.get());

    LOG(WARNING) << "Create " << (has_webhook_certificate_ ? "" : "not ") << "self signed webhook: " << url.ok();
    auto webhook_actor_name = PSTRING() << "Webhook " << url.ok();
    webhook_id_ = td::create_actor<WebhookActor>(
        webhook_actor_name, actor_shared(this, webhook_generation_), tqueue_id_, url.move_as_ok(),
        has_webhook_certificate_ ? get_webhook_certificate_path() : "", webhook_max_connections_, query->is_internal(),
        webhook_ip_address_, webhook_fix_ip_address_, webhook_secret_token_, parameters_);
    // wait for webhook verified or webhook callback
    webhook_query_type_ = WebhookQueryType::Verify;
    webhook_set_query_ = std::move(query);
  } else {
    answer_query(td::JsonTrue(), std::move(query),
                 was_deleted ? Slice("Webhook was deleted") : Slice("Webhook is already deleted"));
  }
}

void Client::do_send_message(object_ptr<td_api::InputMessageContent> input_message_content, PromisedQueryPtr query) {
  auto chat_id = query->arg("chat_id");
  auto reply_to_message_id = get_message_id(query.get(), "reply_to_message_id");
  auto allow_sending_without_reply = to_bool(query->arg("allow_sending_without_reply"));
  auto disable_notification = to_bool(query->arg("disable_notification"));
  auto protect_content = to_bool(query->arg("protect_content"));
  auto r_reply_markup = get_reply_markup(query.get());
  if (r_reply_markup.is_error()) {
    return fail_query_with_error(std::move(query), 400, r_reply_markup.error().message());
  }
  auto reply_markup = r_reply_markup.move_as_ok();

  resolve_reply_markup_bot_usernames(
      std::move(reply_markup), std::move(query),
      [this, chat_id = chat_id.str(), reply_to_message_id, allow_sending_without_reply, disable_notification,
       protect_content, input_message_content = std::move(input_message_content)](
          object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) mutable {
        auto on_success = [this, disable_notification, protect_content,
                           input_message_content = std::move(input_message_content),
                           reply_markup = std::move(reply_markup)](int64 chat_id, int64 reply_to_message_id,
                                                                   PromisedQueryPtr query) mutable {
          auto it = yet_unsent_message_count_.find(chat_id);
          if (it != yet_unsent_message_count_.end() && it->second > MAX_CONCURRENTLY_SENT_CHAT_MESSAGES) {
            return query->set_retry_after_error(60);
          }

          send_request(make_object<td_api::sendMessage>(chat_id, 0, reply_to_message_id,
                                                        get_message_send_options(disable_notification, protect_content),
                                                        std::move(reply_markup), std::move(input_message_content)),
                       td::make_unique<TdOnSendMessageCallback>(this, std::move(query)));
        };
        check_message(chat_id, reply_to_message_id, reply_to_message_id <= 0 || allow_sending_without_reply,
                      AccessRights::Write, "replied message", std::move(query), std::move(on_success));
      });
}

td::int64 Client::get_send_message_query_id(PromisedQueryPtr query, bool is_multisend) {
  auto query_id = current_send_message_query_id_++;
  auto &pending_query = pending_send_message_queries_[query_id];
  CHECK(pending_query == nullptr);
  pending_query = td::make_unique<PendingSendMessageQuery>();
  pending_query->query = std::move(query);
  pending_query->is_multisend = is_multisend;
  return query_id;
}

void Client::on_sent_message(object_ptr<td_api::message> &&message, int64 query_id) {
  CHECK(message != nullptr);
  int64 chat_id = message->chat_id_;
  int64 message_id = message->id_;
  int64 reply_to_message_id = message->reply_to_message_id_;
  if (reply_to_message_id > 0) {
    CHECK(message->reply_in_chat_id_ == chat_id);
    bool is_inserted = yet_unsent_reply_message_ids_[{chat_id, reply_to_message_id}].insert(message_id).second;
    CHECK(is_inserted);
  }

  FullMessageId yet_unsent_message_id{chat_id, message_id};
  YetUnsentMessage yet_unsent_message;
  yet_unsent_message.reply_to_message_id = reply_to_message_id;
  yet_unsent_message.send_message_query_id = query_id;
  auto emplace_result = yet_unsent_messages_.emplace(yet_unsent_message_id, yet_unsent_message);
  CHECK(emplace_result.second);
  yet_unsent_message_count_[chat_id]++;

  auto &query = *pending_send_message_queries_[query_id];
  query.awaited_message_count++;
  query.total_message_count++;
}

void Client::abort_long_poll(bool from_set_webhook) {
  if (long_poll_query_) {
    Slice message;
    if (from_set_webhook) {
      message = Slice("Conflict: terminated by setWebhook request");
    } else {
      message =
          Slice("Conflict: terminated by other getUpdates request; make sure that only one bot instance is running");
    }
    fail_query_conflict(message, std::move(long_poll_query_));
  }
}

void Client::fail_query_conflict(Slice message, PromisedQueryPtr &&query) {
  auto now = td::Time::now_cached();
  if (now >= next_get_updates_conflict_time_) {
    fail_query(409, message, std::move(query));
    next_get_updates_conflict_time_ = now + 3.0;
  } else {
    td::create_actor<td::SleepActor>(
        "FailQueryConflictSleepActor", 3.0,
        td::PromiseCreator::lambda([message = message.str(), query = std::move(query)](td::Result<> result) mutable {
          fail_query(409, message, std::move(query));
        }))
        .release();
  }
}

class Client::JsonUpdates final : public Jsonable {
 public:
  explicit JsonUpdates(td::Span<td::TQueue::Event> updates) : updates_(updates) {
  }
  void store(JsonValueScope *scope) const {
    auto array = scope->enter_array();
    int left_len = 1 << 22;
    for (auto &update : updates_) {
      left_len -= 50 + td::narrow_cast<int>(update.data.size());
      if (left_len <= 0) {
        break;
      }
      array << JsonUpdate(update.id.value(), update.data);
    }
  }

 private:
  td::Span<td::TQueue::Event> updates_;
};

void Client::do_get_updates(int32 offset, int32 limit, int32 timeout, PromisedQueryPtr query) {
  auto &tqueue = parameters_->shared_data_->tqueue_;
  LOG(DEBUG) << "Get updates with offset = " << offset << ", limit = " << limit << " and timeout = " << timeout;
  LOG(DEBUG) << "Queue head = " << tqueue->get_head(tqueue_id_) << ", queue tail = " << tqueue->get_tail(tqueue_id_);

  if (offset <= 0) {
    auto head = tqueue->get_head(tqueue_id_);
    if (!head.empty() && offset < 0) {
      // negative offset is counted from the end
      auto tail = tqueue->get_tail(tqueue_id_);
      CHECK(!tail.empty());
      offset += tail.value();
    }
    if (offset < head.value()) {
      offset = head.value();
    }
  }

  td::MutableSpan<td::TQueue::Event> updates(parameters_->shared_data_->event_buffer_,
                                             SharedData::TQUEUE_EVENT_BUFFER_SIZE);
  updates.truncate(limit);
  td::TQueue::EventId from;
  size_t total_size = 0;
  if (offset <= 0) {
    // queue is not created yet
    updates = {};
  } else {
    bool is_ok = false;
    auto r_offset = td::TQueue::EventId::from_int32(offset);
    auto now = get_unix_time();
    if (r_offset.is_ok()) {
      from = r_offset.ok();
      auto r_total_size = tqueue->get(tqueue_id_, from, true, now, updates);
      if (r_total_size.is_ok()) {
        is_ok = true;
        total_size = r_total_size.move_as_ok();
      }
    }
    if (!is_ok) {
      from = tqueue->get_head(tqueue_id_);
      auto r_total_size = tqueue->get(tqueue_id_, from, true, now, updates);
      CHECK(r_total_size.is_ok());
      total_size = r_total_size.move_as_ok();
    }
  }
  CHECK(total_size >= updates.size());
  total_size -= updates.size();

  bool need_warning = false;
  if (total_size <= MIN_PENDING_UPDATES_WARNING / 2) {
    if (last_pending_update_count_ > MIN_PENDING_UPDATES_WARNING) {
      need_warning = true;
      last_pending_update_count_ = MIN_PENDING_UPDATES_WARNING;
    }
  } else if (total_size >= last_pending_update_count_) {
    need_warning = true;
    while (total_size >= last_pending_update_count_) {
      last_pending_update_count_ *= 2;
    }
  }
  if (need_warning) {
    LOG(WARNING) << "Found " << updates.size() << " updates out of " << (total_size + updates.size())
                 << " after last getUpdates call " << (query->start_timestamp() - previous_get_updates_finish_time_)
                 << " seconds ago in " << (td::Time::now() - query->start_timestamp()) << " seconds";
  } else {
    LOG(DEBUG) << "Found " << updates.size() << " updates out of " << total_size << " from " << from;
  }

  if (timeout != 0 && updates.size() == 0) {
    abort_long_poll(false);
    long_poll_offset_ = offset;
    long_poll_limit_ = limit;
    long_poll_query_ = std::move(query);
    long_poll_was_wakeup_ = false;
    long_poll_hard_timeout_ = td::Time::now_cached() + timeout;
    long_poll_slot_.set_event(td::EventCreator::raw(actor_id(), static_cast<td::uint64>(0)));
    long_poll_slot_.set_timeout_at(long_poll_hard_timeout_);
    return;
  }
  previous_get_updates_finish_time_ = td::Time::now();
  next_bot_updates_warning_time_ = td::Time::now() + BOT_UPDATES_WARNING_DELAY;
  if (total_size == updates.size() && was_bot_updates_warning_) {
    send_request(make_object<td_api::setBotUpdatesStatus>(0, ""), td::make_unique<TdOnOkCallback>());
    was_bot_updates_warning_ = false;
  }
  answer_query(JsonUpdates(updates), std::move(query));
}

void Client::long_poll_wakeup(bool force_flag) {
  if (!long_poll_query_) {
    auto pending_update_count = get_pending_update_count();
    if (pending_update_count >= MIN_PENDING_UPDATES_WARNING && td::Time::now() > next_bot_updates_warning_time_) {
      send_request(make_object<td_api::setBotUpdatesStatus>(td::narrow_cast<int32>(pending_update_count),
                                                            "The getUpdates method is not called for too long"),
                   td::make_unique<TdOnOkCallback>());
      next_bot_updates_warning_time_ =
          td::Time::now_cached() + BOT_UPDATES_WARNING_DELAY;  // do not send warnings too often
      was_bot_updates_warning_ = true;
    }
    return;
  }
  if (force_flag) {
    do_get_updates(long_poll_offset_, long_poll_limit_, 0, std::move(long_poll_query_));
  } else {
    double now = td::Time::now();
    if (!long_poll_was_wakeup_) {
      long_poll_hard_timeout_ = td::min(now + LONG_POLL_MAX_DELAY, long_poll_hard_timeout_);
      long_poll_was_wakeup_ = true;
    }
    double timeout = td::min(now + LONG_POLL_WAIT_AFTER, long_poll_hard_timeout_);
    long_poll_slot_.set_event(td::EventCreator::raw(actor_id(), static_cast<td::uint64>(0)));
    long_poll_slot_.set_timeout_at(timeout);
  }
}

void Client::add_user(UserInfo *user_info, object_ptr<td_api::user> &&user) {
  user_info->first_name = std::move(user->first_name_);
  user_info->last_name = std::move(user->last_name_);
  user_info->username = std::move(user->username_);
  user_info->language_code = std::move(user->language_code_);

  user_info->have_access = user->have_access_;
  user_info->is_premium = user->is_premium_;
  user_info->added_to_attachment_menu = user->added_to_attachment_menu_;

  switch (user->type_->get_id()) {
    case td_api::userTypeRegular::ID:
      user_info->type = UserInfo::Type::Regular;
      break;
    case td_api::userTypeBot::ID: {
      user_info->type = UserInfo::Type::Bot;
      auto *bot = static_cast<const td_api::userTypeBot *>(user->type_.get());
      user_info->can_join_groups = bot->can_join_groups_;
      user_info->can_read_all_group_messages = bot->can_read_all_group_messages_;
      user_info->is_inline_bot = bot->is_inline_;
      break;
    }
    case td_api::userTypeDeleted::ID:
      user_info->type = UserInfo::Type::Deleted;
      break;
    case td_api::userTypeUnknown::ID:
      user_info->type = UserInfo::Type::Unknown;
      break;
    default:
      UNREACHABLE();
      break;
  }
}

Client::UserInfo *Client::add_user_info(int64 user_id) {
  auto emplace_result = users_.emplace(user_id, nullptr);
  auto &user_info = emplace_result.first->second;
  if (emplace_result.second) {
    user_info = td::make_unique<UserInfo>();
  } else {
    CHECK(user_info != nullptr);
  }
  return user_info.get();
}

const Client::UserInfo *Client::get_user_info(int64 user_id) const {
  auto it = users_.find(user_id);
  return it == users_.end() ? nullptr : it->second.get();
}

void Client::set_user_photo(int64 user_id, object_ptr<td_api::chatPhoto> &&photo) {
  add_user_info(user_id)->photo = std::move(photo);
}

void Client::set_user_bio(int64 user_id, td::string &&bio) {
  add_user_info(user_id)->bio = std::move(bio);
}

void Client::set_user_has_private_forwards(int64 user_id, bool has_private_forwards) {
  add_user_info(user_id)->has_private_forwards = has_private_forwards;
}

void Client::add_group(GroupInfo *group_info, object_ptr<td_api::basicGroup> &&group) {
  group_info->member_count = group->member_count_;
  group_info->left = group->status_->get_id() == td_api::chatMemberStatusLeft::ID;
  group_info->kicked = group->status_->get_id() == td_api::chatMemberStatusBanned::ID;
  group_info->is_active = group->is_active_;
  group_info->upgraded_to_supergroup_id = group->upgraded_to_supergroup_id_;
  if (!group_info->left && !group_info->kicked && group_info->member_count == 0) {
    group_info->member_count = 1;
  }
}

Client::GroupInfo *Client::add_group_info(int64 group_id) {
  auto emplace_result = groups_.emplace(group_id, nullptr);
  auto &group_info = emplace_result.first->second;
  if (emplace_result.second) {
    group_info = td::make_unique<GroupInfo>();
  } else {
    CHECK(group_info != nullptr);
  }
  return group_info.get();
}

const Client::GroupInfo *Client::get_group_info(int64 group_id) const {
  auto it = groups_.find(group_id);
  return it == groups_.end() ? nullptr : it->second.get();
}

void Client::set_group_photo(int64 group_id, object_ptr<td_api::chatPhoto> &&photo) {
  add_group_info(group_id)->photo = std::move(photo);
}

void Client::set_group_description(int64 group_id, td::string &&descripton) {
  add_group_info(group_id)->description = std::move(descripton);
}

void Client::set_group_invite_link(int64 group_id, td::string &&invite_link) {
  add_group_info(group_id)->invite_link = std::move(invite_link);
}

void Client::add_supergroup(SupergroupInfo *supergroup_info, object_ptr<td_api::supergroup> &&supergroup) {
  supergroup_info->username = std::move(supergroup->username_);
  supergroup_info->date = supergroup->date_;
  supergroup_info->status = std::move(supergroup->status_);
  supergroup_info->is_supergroup = !supergroup->is_channel_;
  supergroup_info->has_location = supergroup->has_location_;
  supergroup_info->join_to_send_messages = supergroup->join_to_send_messages_;
  supergroup_info->join_by_request = supergroup->join_by_request_;
}

void Client::set_supergroup_photo(int64 supergroup_id, object_ptr<td_api::chatPhoto> &&photo) {
  add_supergroup_info(supergroup_id)->photo = std::move(photo);
}

void Client::set_supergroup_description(int64 supergroup_id, td::string &&descripton) {
  add_supergroup_info(supergroup_id)->description = std::move(descripton);
}

void Client::set_supergroup_invite_link(int64 supergroup_id, td::string &&invite_link) {
  add_supergroup_info(supergroup_id)->invite_link = std::move(invite_link);
}

void Client::set_supergroup_sticker_set_id(int64 supergroup_id, int64 sticker_set_id) {
  add_supergroup_info(supergroup_id)->sticker_set_id = sticker_set_id;
}

void Client::set_supergroup_can_set_sticker_set(int64 supergroup_id, bool can_set_sticker_set) {
  add_supergroup_info(supergroup_id)->can_set_sticker_set = can_set_sticker_set;
}

void Client::set_supergroup_slow_mode_delay(int64 supergroup_id, int32 slow_mode_delay) {
  add_supergroup_info(supergroup_id)->slow_mode_delay = slow_mode_delay;
}

void Client::set_supergroup_linked_chat_id(int64 supergroup_id, int64 linked_chat_id) {
  add_supergroup_info(supergroup_id)->linked_chat_id = linked_chat_id;
}

void Client::set_supergroup_location(int64 supergroup_id, object_ptr<td_api::chatLocation> location) {
  add_supergroup_info(supergroup_id)->location = std::move(location);
}

Client::SupergroupInfo *Client::add_supergroup_info(int64 supergroup_id) {
  auto emplace_result = supergroups_.emplace(supergroup_id, nullptr);
  auto &supergroup_info = emplace_result.first->second;
  if (emplace_result.second) {
    supergroup_info = td::make_unique<SupergroupInfo>();
  } else {
    CHECK(supergroup_info != nullptr);
  }
  return supergroup_info.get();
}

const Client::SupergroupInfo *Client::get_supergroup_info(int64 supergroup_id) const {
  auto it = supergroups_.find(supergroup_id);
  return it == supergroups_.end() ? nullptr : it->second.get();
}

Client::ChatInfo *Client::add_chat(int64 chat_id) {
  LOG(DEBUG) << "Update chat " << chat_id;
  auto emplace_result = chats_.emplace(chat_id, nullptr);
  auto &chat_info = emplace_result.first->second;
  if (emplace_result.second) {
    chat_info = td::make_unique<ChatInfo>();
  } else {
    CHECK(chat_info != nullptr);
  }
  return chat_info.get();
}

const Client::ChatInfo *Client::get_chat(int64 chat_id) const {
  auto it = chats_.find(chat_id);
  if (it == chats_.end()) {
    return nullptr;
  }
  return it->second.get();
}

Client::ChatType Client::get_chat_type(int64 chat_id) const {
  auto chat_info = get_chat(chat_id);
  if (chat_info == nullptr) {
    return ChatType::Unknown;
  }
  switch (chat_info->type) {
    case ChatInfo::Type::Private:
      return ChatType::Private;
    case ChatInfo::Type::Group:
      return ChatType::Group;
    case ChatInfo::Type::Supergroup: {
      auto supergroup_info = get_supergroup_info(chat_info->supergroup_id);
      if (supergroup_info == nullptr) {
        return ChatType::Unknown;
      }
      if (supergroup_info->is_supergroup) {
        return ChatType::Supergroup;
      } else {
        return ChatType::Channel;
      }
    }
    case ChatInfo::Type::Unknown:
      return ChatType::Unknown;
    default:
      UNREACHABLE();
      return ChatType::Unknown;
  }
}

td::string Client::get_chat_description(int64 chat_id) const {
  auto chat_info = get_chat(chat_id);
  if (chat_info == nullptr) {
    return PSTRING() << "unknown chat " << chat_id;
  }
  switch (chat_info->type) {
    case ChatInfo::Type::Private: {
      auto user_info = get_user_info(chat_info->user_id);
      return PSTRING() << "private " << (user_info == nullptr || !user_info->have_access ? "un" : "")
                       << "accessible chat " << chat_id;
    }
    case ChatInfo::Type::Group: {
      auto group_info = get_group_info(chat_info->group_id);
      if (group_info == nullptr) {
        return PSTRING() << "unknown group chat " << chat_id;
      }
      return PSTRING() << (group_info->is_active ? "" : "in") << "active group chat " << chat_id << ", chat status = "
                       << (group_info->kicked ? "kicked" : (group_info->left ? "left" : "member"));
    }
    case ChatInfo::Type::Supergroup: {
      auto supergroup_info = get_supergroup_info(chat_info->supergroup_id);
      if (supergroup_info == nullptr) {
        return PSTRING() << "unknown supergroup chat " << chat_id;
      }
      return PSTRING() << (supergroup_info->is_supergroup ? "supergroup" : "channel") << " chat " << chat_id
                       << ", chat status = " << to_string(supergroup_info->status)
                       << ", username = " << supergroup_info->username;
    }
    case ChatInfo::Type::Unknown:
      return PSTRING() << "unknown chat " << chat_id;
    default:
      UNREACHABLE();
      return "";
  }
}

void Client::json_store_file(td::JsonObjectScope &object, const td_api::file *file, bool with_path) const {
  if (file->id_ == 0) {
    return;
  }

  LOG_IF(ERROR, file->remote_->id_.empty()) << "File remote identifier is empty: " << td::oneline(to_string(*file));

  object("file_id", file->remote_->id_);
  object("file_unique_id", file->remote_->unique_id_);
  if (file->size_) {
    object("file_size", file->size_);
  }
  if (with_path && file->local_->is_downloading_completed_) {
    if (parameters_->local_mode_) {
      if (td::check_utf8(file->local_->path_)) {
        object("file_path", file->local_->path_);
      } else {
        object("file_path", td::JsonRawString(file->local_->path_));
      }
    } else {
      Slice relative_path = td::PathView::relative(file->local_->path_, dir_, true);
      if (!relative_path.empty() && file->local_->downloaded_size_ <= MAX_DOWNLOAD_FILE_SIZE) {
        object("file_path", relative_path);
      }
    }
  }
}

void Client::json_store_thumbnail(td::JsonObjectScope &object, const td_api::thumbnail *thumbnail) const {
  if (thumbnail == nullptr || thumbnail->format_->get_id() == td_api::thumbnailFormatMpeg4::ID) {
    return;
  }

  CHECK(thumbnail->file_->id_ > 0);
  object("thumb", JsonThumbnail(thumbnail, this));
}

void Client::json_store_callback_query_payload(td::JsonObjectScope &object,
                                               const td_api::CallbackQueryPayload *payload) {
  CHECK(payload != nullptr);
  switch (payload->get_id()) {
    case td_api::callbackQueryPayloadData::ID: {
      auto data = static_cast<const td_api::callbackQueryPayloadData *>(payload);
      if (!td::check_utf8(data->data_)) {
        LOG(WARNING) << "Receive non-UTF-8 callback query data";
        object("data", td::JsonRawString(data->data_));
      } else {
        object("data", data->data_);
      }
      break;
    }
    case td_api::callbackQueryPayloadGame::ID:
      object("game_short_name", static_cast<const td_api::callbackQueryPayloadGame *>(payload)->game_short_name_);
      break;
    case td_api::callbackQueryPayloadDataWithPassword::ID:
      UNREACHABLE();
      break;
    default:
      UNREACHABLE();
  }
}

void Client::json_store_administrator_rights(td::JsonObjectScope &object, const td_api::chatAdministratorRights *rights,
                                             ChatType chat_type) {
  object("can_manage_chat", td::JsonBool(rights->can_manage_chat_));
  object("can_change_info", td::JsonBool(rights->can_change_info_));
  if (chat_type == ChatType::Channel) {
    object("can_post_messages", td::JsonBool(rights->can_post_messages_));
    object("can_edit_messages", td::JsonBool(rights->can_edit_messages_));
  }
  object("can_delete_messages", td::JsonBool(rights->can_delete_messages_));
  object("can_invite_users", td::JsonBool(rights->can_invite_users_));
  object("can_restrict_members", td::JsonBool(rights->can_restrict_members_));
  if (chat_type == ChatType::Group || chat_type == ChatType::Supergroup) {
    object("can_pin_messages", td::JsonBool(rights->can_pin_messages_));
  }
  object("can_promote_members", td::JsonBool(rights->can_promote_members_));
  object("can_manage_video_chats", td::JsonBool(rights->can_manage_video_chats_));
  object("is_anonymous", td::JsonBool(rights->is_anonymous_));
}

void Client::json_store_permissions(td::JsonObjectScope &object, const td_api::chatPermissions *permissions) {
  object("can_send_messages", td::JsonBool(permissions->can_send_messages_));
  object("can_send_media_messages", td::JsonBool(permissions->can_send_media_messages_));
  object("can_send_polls", td::JsonBool(permissions->can_send_polls_));
  object("can_send_other_messages", td::JsonBool(permissions->can_send_other_messages_));
  object("can_add_web_page_previews", td::JsonBool(permissions->can_add_web_page_previews_));
  object("can_change_info", td::JsonBool(permissions->can_change_info_));
  object("can_invite_users", td::JsonBool(permissions->can_invite_users_));
  object("can_pin_messages", td::JsonBool(permissions->can_pin_messages_));
}

Client::Slice Client::get_update_type_name(UpdateType update_type) {
  switch (update_type) {
    case UpdateType::Message:
      return Slice("message");
    case UpdateType::EditedMessage:
      return Slice("edited_message");
    case UpdateType::ChannelPost:
      return Slice("channel_post");
    case UpdateType::EditedChannelPost:
      return Slice("edited_channel_post");
    case UpdateType::InlineQuery:
      return Slice("inline_query");
    case UpdateType::ChosenInlineResult:
      return Slice("chosen_inline_result");
    case UpdateType::CallbackQuery:
      return Slice("callback_query");
    case UpdateType::CustomEvent:
      return Slice("custom_event");
    case UpdateType::CustomQuery:
      return Slice("custom_query");
    case UpdateType::ShippingQuery:
      return Slice("shipping_query");
    case UpdateType::PreCheckoutQuery:
      return Slice("pre_checkout_query");
    case UpdateType::Poll:
      return Slice("poll");
    case UpdateType::PollAnswer:
      return Slice("poll_answer");
    case UpdateType::MyChatMember:
      return Slice("my_chat_member");
    case UpdateType::ChatMember:
      return Slice("chat_member");
    case UpdateType::ChatJoinRequest:
      return Slice("chat_join_request");
    default:
      UNREACHABLE();
      return Slice();
  }
}

td::uint32 Client::get_allowed_update_types(td::MutableSlice allowed_updates, bool is_internal) {
  if (allowed_updates.empty()) {
    return 0;
  }

  LOG(INFO) << "Parsing JSON object: " << allowed_updates;
  auto r_value = json_decode(allowed_updates);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return 0;
  }

  td::uint32 result = 0;
  auto value = r_value.move_as_ok();
  if (value.type() != JsonValue::Type::Array) {
    if (value.type() == JsonValue::Type::Number && is_internal) {
      auto r_number = td::to_integer_safe<td::uint32>(value.get_number());
      if (r_number.is_ok() && r_number.ok() > 0) {
        return r_number.ok();
      }
    }
    return 0;
  }
  for (auto &update_type_name : value.get_array()) {
    if (update_type_name.type() != JsonValue::Type::String) {
      return 0;
    }
    auto type_name = update_type_name.get_string();
    to_lower_inplace(type_name);
    for (int32 i = 0; i < static_cast<int32>(UpdateType::Size); i++) {
      if (get_update_type_name(static_cast<UpdateType>(i)) == type_name) {
        result |= (1 << i);
      }
    }
  }

  if (result == 0) {
    return DEFAULT_ALLOWED_UPDATE_TYPES;
  }
  return result;
}

bool Client::update_allowed_update_types(const Query *query) {
  auto allowed_update_types = get_allowed_update_types(query->arg("allowed_updates"), query->is_internal());
  if (allowed_update_types != 0 && allowed_update_types != allowed_update_types_) {
    allowed_update_types_ = allowed_update_types;
    object_ptr<td_api::OptionValue> value;
    if (allowed_update_types == DEFAULT_ALLOWED_UPDATE_TYPES) {
      value = make_object<td_api::optionValueEmpty>();
    } else {
      value = make_object<td_api::optionValueInteger>(allowed_update_types);
    }
    send_request(make_object<td_api::setOption>("xallowed_update_types", std::move(value)),
                 td::make_unique<TdOnOkCallback>());
    return true;
  }
  return false;
}

template <class T>
class UpdateJsonable final : public td::VirtuallyJsonable {
 public:
  explicit UpdateJsonable(const T &update) : update(update) {
  }
  void store(JsonValueScope *scope) const final {
    *scope << update;
  }

 private:
  const T &update;
};

template <class T>
void Client::add_update(UpdateType update_type, const T &update, int32 timeout, int64 webhook_queue_id) {
  add_update_impl(update_type, UpdateJsonable<T>(update), timeout, webhook_queue_id);
}

void Client::add_update_impl(UpdateType update_type, const td::VirtuallyJsonable &update, int32 timeout,
                             int64 webhook_queue_id) {
  update_last_synchronization_error_date();
  last_update_creation_time_ = td::Time::now();

  if (((allowed_update_types_ >> static_cast<int32>(update_type)) & 1) == 0) {
    return;
  }

  send_closure(stat_actor_, &BotStatActor::add_event<ServerBotStat::Update>, ServerBotStat::Update{}, td::Time::now());

  const size_t BUF_SIZE = 1 << 16;
  auto buf = td::StackAllocator::alloc(BUF_SIZE);
  td::JsonBuilder jb(td::StringBuilder(buf.as_slice(), true));
  jb.enter_value() << get_update_type_name(update_type);
  jb.string_builder() << ":";
  jb.enter_value() << update;
  if (jb.string_builder().is_error()) {
    LOG(ERROR) << "JSON buffer overflow";
    return;
  }

  auto update_slice = jb.string_builder().as_cslice();
  auto r_id = parameters_->shared_data_->tqueue_->push(tqueue_id_, update_slice.str(), get_unix_time() + timeout,
                                                       webhook_queue_id, td::TQueue::EventId());
  if (r_id.is_ok()) {
    auto id = r_id.move_as_ok();
    LOG(DEBUG) << "Update " << id << " was added for " << timeout << " seconds: " << update_slice;
    if (webhook_url_.empty()) {
      long_poll_wakeup(false);
    } else {
      send_closure(webhook_id_, &WebhookActor::update);
    }
  } else {
    LOG(DEBUG) << "Update failed to be added with error " << r_id.error() << " for " << timeout
               << " seconds: " << update_slice;
  }
}

void Client::add_new_message(object_ptr<td_api::message> &&message, bool is_edited) {
  CHECK(message != nullptr);

  if (message->sending_state_ != nullptr) {
    return;
  }

  auto chat_id = message->chat_id_;
  CHECK(chat_id != 0);
  new_message_queues_[chat_id].queue_.emplace(std::move(message), is_edited);
  process_new_message_queue(chat_id);
}

void Client::add_update_poll(object_ptr<td_api::updatePoll> &&update) {
  CHECK(update != nullptr);
  add_update(UpdateType::Poll, JsonPoll(update->poll_.get(), this), 86400, update->poll_->id_);
}

void Client::add_update_poll_answer(object_ptr<td_api::updatePollAnswer> &&update) {
  CHECK(update != nullptr);
  add_update(UpdateType::PollAnswer, JsonPollAnswer(update.get(), this), 86400, update->poll_id_);
}

void Client::add_new_inline_query(int64 inline_query_id, int64 sender_user_id, object_ptr<td_api::location> location,
                                  object_ptr<td_api::ChatType> chat_type, const td::string &query,
                                  const td::string &offset) {
  add_update(UpdateType::InlineQuery,
             JsonInlineQuery(inline_query_id, sender_user_id, location.get(), chat_type.get(), query, offset, this), 30,
             sender_user_id + (static_cast<int64>(1) << 33));
}

void Client::add_new_chosen_inline_result(int64 sender_user_id, object_ptr<td_api::location> location,
                                          const td::string &query, const td::string &result_id,
                                          const td::string &inline_message_id) {
  add_update(UpdateType::ChosenInlineResult,
             JsonChosenInlineResult(sender_user_id, location.get(), query, result_id, inline_message_id, this), 600,
             sender_user_id + (static_cast<int64>(2) << 33));
}

void Client::add_new_callback_query(object_ptr<td_api::updateNewCallbackQuery> &&query) {
  CHECK(query != nullptr);
  auto user_id = query->sender_user_id_;
  CHECK(user_id != 0);
  new_callback_query_queues_[user_id].queue_.push(std::move(query));
  process_new_callback_query_queue(user_id, 0);
}

void Client::process_new_callback_query_queue(int64 user_id, int state) {
  auto &queue = new_callback_query_queues_[user_id];
  if (queue.has_active_request_) {
    CHECK(state == 0);
    return;
  }
  if (logging_out_ || closing_) {
    new_callback_query_queues_.erase(user_id);
    return;
  }
  while (!queue.queue_.empty()) {
    auto &query = queue.queue_.front();
    int64 chat_id = query->chat_id_;
    int64 message_id = query->message_id_;
    auto message_info = get_message(chat_id, message_id);
    // callback message can be already deleted in the bot outbox
    if (state == 0) {
      if (message_info == nullptr) {
        // get the message from the server
        queue.has_active_request_ = true;
        return send_request(make_object<td_api::getCallbackQueryMessage>(chat_id, message_id, query->id_),
                            td::make_unique<TdOnGetCallbackQueryMessageCallback>(this, user_id, state));
      }
      state = 1;
    }
    if (state == 1) {
      auto reply_to_message_id =
          message_info == nullptr || message_info->is_reply_to_message_deleted ? 0 : message_info->reply_to_message_id;
      if (reply_to_message_id > 0 && get_message(chat_id, reply_to_message_id) == nullptr) {
        queue.has_active_request_ = true;
        return send_request(make_object<td_api::getRepliedMessage>(chat_id, message_id),
                            td::make_unique<TdOnGetCallbackQueryMessageCallback>(this, user_id, state));
      }
      state = 2;
    }
    if (state == 2) {
      auto message_sticker_set_id = message_info == nullptr ? 0 : get_sticker_set_id(message_info->content);
      if (!have_sticker_set_name(message_sticker_set_id)) {
        queue.has_active_request_ = true;
        return send_request(make_object<td_api::getStickerSet>(message_sticker_set_id),
                            td::make_unique<TdOnGetStickerSetCallback>(this, message_sticker_set_id, user_id, 0));
      }
      auto reply_to_message_id =
          message_info == nullptr || message_info->is_reply_to_message_deleted ? 0 : message_info->reply_to_message_id;
      if (reply_to_message_id > 0) {
        auto reply_to_message_info = get_message(chat_id, reply_to_message_id);
        auto reply_sticker_set_id =
            reply_to_message_info == nullptr ? 0 : get_sticker_set_id(reply_to_message_info->content);
        if (!have_sticker_set_name(reply_sticker_set_id)) {
          queue.has_active_request_ = true;
          return send_request(make_object<td_api::getStickerSet>(reply_sticker_set_id),
                              td::make_unique<TdOnGetStickerSetCallback>(this, reply_sticker_set_id, user_id, 0));
        }
      }
    }
    CHECK(state == 2);

    CHECK(user_id == query->sender_user_id_);
    add_update(UpdateType::CallbackQuery,
               JsonCallbackQuery(query->id_, user_id, chat_id, message_id, message_info, query->chat_instance_,
                                 query->payload_.get(), this),
               150, user_id + (static_cast<int64>(3) << 33));

    queue.queue_.pop();
  }
  new_callback_query_queues_.erase(user_id);
}

void Client::add_new_inline_callback_query(object_ptr<td_api::updateNewInlineCallbackQuery> &&query) {
  CHECK(query != nullptr);
  add_update(UpdateType::CallbackQuery,
             JsonInlineCallbackQuery(query->id_, query->sender_user_id_, query->inline_message_id_,
                                     query->chat_instance_, query->payload_.get(), this),
             150, query->sender_user_id_ + (static_cast<int64>(3) << 33));
}

void Client::add_new_shipping_query(object_ptr<td_api::updateNewShippingQuery> &&query) {
  CHECK(query != nullptr);
  add_update(UpdateType::ShippingQuery, JsonShippingQuery(query.get(), this), 150,
             query->sender_user_id_ + (static_cast<int64>(4) << 33));
}

void Client::add_new_pre_checkout_query(object_ptr<td_api::updateNewPreCheckoutQuery> &&query) {
  CHECK(query != nullptr);
  add_update(UpdateType::PreCheckoutQuery, JsonPreCheckoutQuery(query.get(), this), 150,
             query->sender_user_id_ + (static_cast<int64>(4) << 33));
}

void Client::add_new_custom_event(object_ptr<td_api::updateNewCustomEvent> &&event) {
  CHECK(event != nullptr);
  add_update(UpdateType::CustomEvent, JsonCustomJson(event->event_), 600, 0);
}

void Client::add_new_custom_query(object_ptr<td_api::updateNewCustomQuery> &&query) {
  CHECK(query != nullptr);
  int32 timeout = query->timeout_ <= 0 ? 86400 : query->timeout_;
  add_update(UpdateType::CustomQuery, JsonCustomJson(query->data_), timeout, 0);
}

void Client::add_update_chat_member(object_ptr<td_api::updateChatMember> &&update) {
  CHECK(update != nullptr);
  auto left_time = update->date_ + 86400 - get_unix_time();
  if (left_time > 0) {
    CHECK(update->old_chat_member_->member_id_ != nullptr);
    if (update->old_chat_member_->member_id_->get_id() != td_api::messageSenderUser::ID ||
        update->new_chat_member_->member_id_->get_id() != td_api::messageSenderUser::ID) {
      return;
    }
    auto user_id = static_cast<const td_api::messageSenderUser *>(update->old_chat_member_->member_id_.get())->user_id_;
    bool is_my = (user_id == my_id_);
    auto webhook_queue_id = update->chat_id_ + (static_cast<int64>(is_my ? 5 : 6) << 33);
    auto update_type = is_my ? UpdateType::MyChatMember : UpdateType::ChatMember;
    add_update(update_type, JsonChatMemberUpdated(update.get(), this), left_time, webhook_queue_id);
  }
}

void Client::add_update_chat_join_request(object_ptr<td_api::updateNewChatJoinRequest> &&update) {
  CHECK(update != nullptr);
  CHECK(update->request_ != nullptr);
  auto left_time = update->request_->date_ + 86400 - get_unix_time();
  if (left_time > 0) {
    auto webhook_queue_id = update->chat_id_ + (static_cast<int64>(6) << 33);
    add_update(UpdateType::ChatJoinRequest, JsonChatJoinRequest(update.get(), this), left_time, webhook_queue_id);
  }
}

td::int64 Client::choose_added_member_id(const td_api::messageChatAddMembers *message_add_members) const {
  CHECK(message_add_members != nullptr);
  for (auto &member_user_id : message_add_members->member_user_ids_) {
    if (member_user_id == my_id_) {
      return my_id_;
    }
  }
  if (message_add_members->member_user_ids_.empty()) {
    return 0;
  }
  return message_add_members->member_user_ids_[0];
}

bool Client::need_skip_update_message(int64 chat_id, const object_ptr<td_api::message> &message, bool is_edited) const {
  auto chat = get_chat(chat_id);
  CHECK(chat != nullptr);
  if (message->is_outgoing_) {
    switch (message->content_->get_id()) {
      case td_api::messageChatChangeTitle::ID:
      case td_api::messageChatChangePhoto::ID:
      case td_api::messageChatDeletePhoto::ID:
      case td_api::messageChatDeleteMember::ID:
      case td_api::messageChatSetTheme::ID:
      case td_api::messagePinMessage::ID:
      case td_api::messageProximityAlertTriggered::ID:
      case td_api::messageVideoChatScheduled::ID:
      case td_api::messageVideoChatStarted::ID:
      case td_api::messageVideoChatEnded::ID:
      case td_api::messageInviteVideoChatParticipants::ID:
        // don't skip
        break;
      default:
        return true;
    }
  }

  int32 message_date = message->edit_date_ == 0 ? message->date_ : message->edit_date_;
  if (message_date <= get_unix_time() - 86400) {
    // don't send messages received/edited more than 1 day ago
    return true;
  }

  if (chat->type == ChatInfo::Type::Supergroup) {
    auto supergroup_info = get_supergroup_info(chat->supergroup_id);
    if (supergroup_info->status->get_id() == td_api::chatMemberStatusLeft::ID ||
        supergroup_info->status->get_id() == td_api::chatMemberStatusBanned::ID) {
      // if we have left the chat, send only update about leaving the supergroup
      if (message->content_->get_id() == td_api::messageChatDeleteMember::ID) {
        auto user_id = static_cast<const td_api::messageChatDeleteMember *>(message->content_.get())->user_id_;
        return user_id != my_id_;
      }
      return true;
    }

    if (supergroup_info->date > message->date_ || authorization_date_ > message->date_) {
      // don't send messages received before join or getting authorization
      return true;
    }
  }

  if (message->ttl_ > 0 && message->ttl_expires_in_ == message->ttl_) {
    return true;
  }

  if (message->forward_info_ != nullptr &&
      message->forward_info_->origin_->get_id() == td_api::messageForwardOriginMessageImport::ID) {
    return true;
  }

  switch (message->content_->get_id()) {
    case td_api::messageChatAddMembers::ID: {
      auto content = static_cast<const td_api::messageChatAddMembers *>(message->content_.get());
      if (content->member_user_ids_.empty()) {
        LOG(ERROR) << "Got empty messageChatAddMembers";
        return true;
      }
      break;
    }
    case td_api::messageSupergroupChatCreate::ID: {
      if (chat->type != ChatInfo::Type::Supergroup) {
        LOG(ERROR) << "Receive messageSupergroupChatCreate in the non-supergroup chat " << chat_id;
        return true;
      }
      break;
    }
    case td_api::messagePinMessage::ID: {
      auto content = static_cast<const td_api::messagePinMessage *>(message->content_.get());
      auto pinned_message_id = content->message_id_;
      if (pinned_message_id <= 0) {
        return true;
      }
      const MessageInfo *pinned_message = get_message(chat_id, pinned_message_id);
      if (pinned_message == nullptr) {
        LOG(WARNING) << "Pinned unknown, inaccessible or deleted message " << pinned_message_id << " in " << chat_id;
        return true;
      }
      break;
    }
    case td_api::messageProximityAlertTriggered::ID: {
      auto content = static_cast<const td_api::messageProximityAlertTriggered *>(message->content_.get());
      return content->traveler_id_->get_id() != td_api::messageSenderUser::ID ||
             content->watcher_id_->get_id() != td_api::messageSenderUser::ID;
    }
    case td_api::messageGameScore::ID:
      return true;
    case td_api::messagePaymentSuccessful::ID:
      return true;
    case td_api::messagePassportDataSent::ID:
      return true;
    case td_api::messageCall::ID:
      return true;
    case td_api::messageUnsupported::ID:
      return true;
    case td_api::messageContactRegistered::ID:
      return true;
    case td_api::messageExpiredPhoto::ID:
      return true;
    case td_api::messageExpiredVideo::ID:
      return true;
    case td_api::messageCustomServiceAction::ID:
      return true;
    case td_api::messageChatSetTheme::ID:
      return true;
    case td_api::messageWebAppDataSent::ID:
      return true;
    default:
      break;
  }

  if (is_edited) {
    const MessageInfo *old_message = get_message(chat_id, message->id_);
    if (old_message != nullptr && !old_message->is_content_changed) {
      return true;
    }
  }

  return false;
}

td::int64 &Client::get_reply_to_message_id(object_ptr<td_api::message> &message) {
  if (message->content_->get_id() == td_api::messagePinMessage::ID) {
    CHECK(message->reply_to_message_id_ == 0);
    return static_cast<td_api::messagePinMessage *>(message->content_.get())->message_id_;
  }
  if (message->reply_in_chat_id_ != message->chat_id_ && message->reply_to_message_id_ != 0) {
    LOG(WARNING) << "Drop reply to message " << message->id_ << " in chat " << message->chat_id_
                 << " from another chat " << message->reply_in_chat_id_;
    message->reply_in_chat_id_ = 0;
    message->reply_to_message_id_ = 0;
  }
  return message->reply_to_message_id_;
}

void Client::set_message_reply_to_message_id(MessageInfo *message_info, int64 reply_to_message_id) {
  if (message_info->reply_to_message_id == reply_to_message_id) {
    return;
  }

  if (message_info->reply_to_message_id > 0) {
    LOG_IF(ERROR, reply_to_message_id > 0)
        << "Message " << message_info->id << " in chat " << message_info->chat_id
        << " has changed reply_to_message from " << message_info->reply_to_message_id << " to " << reply_to_message_id;
    auto it = reply_message_ids_.find({message_info->chat_id, message_info->reply_to_message_id});
    if (it != reply_message_ids_.end()) {
      it->second.erase(message_info->id);
      if (it->second.empty()) {
        reply_message_ids_.erase(it);
      }
    }
  }
  if (reply_to_message_id > 0) {
    reply_message_ids_[{message_info->chat_id, reply_to_message_id}].insert(message_info->id);
  }

  message_info->reply_to_message_id = reply_to_message_id;
}

td::CSlice Client::get_callback_data(const object_ptr<td_api::InlineKeyboardButtonType> &type) {
  CHECK(type != nullptr);
  switch (type->get_id()) {
    case td_api::inlineKeyboardButtonTypeCallback::ID:
      return static_cast<const td_api::inlineKeyboardButtonTypeCallback *>(type.get())->data_;
    case td_api::inlineKeyboardButtonTypeCallbackWithPassword::ID:
      return static_cast<const td_api::inlineKeyboardButtonTypeCallbackWithPassword *>(type.get())->data_;
    default:
      UNREACHABLE();
      return td::CSlice();
  }
}

bool Client::are_equal_inline_keyboard_buttons(const td_api::inlineKeyboardButton *lhs,
                                               const td_api::inlineKeyboardButton *rhs) {
  CHECK(lhs != nullptr);
  CHECK(rhs != nullptr);
  if (lhs->text_ != rhs->text_) {
    return false;
  }
  if (lhs->type_->get_id() != rhs->type_->get_id()) {
    return false;
  }
  switch (lhs->type_->get_id()) {
    case td_api::inlineKeyboardButtonTypeUrl::ID: {
      auto lhs_type = static_cast<const td_api::inlineKeyboardButtonTypeUrl *>(lhs->type_.get());
      auto rhs_type = static_cast<const td_api::inlineKeyboardButtonTypeUrl *>(rhs->type_.get());
      return lhs_type->url_ == rhs_type->url_;
    }
    case td_api::inlineKeyboardButtonTypeLoginUrl::ID: {
      auto lhs_type = static_cast<const td_api::inlineKeyboardButtonTypeLoginUrl *>(lhs->type_.get());
      auto rhs_type = static_cast<const td_api::inlineKeyboardButtonTypeLoginUrl *>(rhs->type_.get());
      return lhs_type->url_ == rhs_type->url_;  // don't compare id_ and forward_text_
    }
    case td_api::inlineKeyboardButtonTypeCallback::ID:
    case td_api::inlineKeyboardButtonTypeCallbackWithPassword::ID:
      return get_callback_data(lhs->type_) == get_callback_data(rhs->type_);
    case td_api::inlineKeyboardButtonTypeCallbackGame::ID:
      return true;
    case td_api::inlineKeyboardButtonTypeSwitchInline::ID: {
      auto lhs_type = static_cast<const td_api::inlineKeyboardButtonTypeSwitchInline *>(lhs->type_.get());
      auto rhs_type = static_cast<const td_api::inlineKeyboardButtonTypeSwitchInline *>(rhs->type_.get());
      return lhs_type->query_ == rhs_type->query_ && lhs_type->in_current_chat_ == rhs_type->in_current_chat_;
    }
    case td_api::inlineKeyboardButtonTypeBuy::ID:
      return true;
    case td_api::inlineKeyboardButtonTypeUser::ID: {
      auto lhs_type = static_cast<const td_api::inlineKeyboardButtonTypeUser *>(lhs->type_.get());
      auto rhs_type = static_cast<const td_api::inlineKeyboardButtonTypeUser *>(rhs->type_.get());
      return lhs_type->user_id_ == rhs_type->user_id_;
    }
    case td_api::inlineKeyboardButtonTypeWebApp::ID: {
      auto lhs_type = static_cast<const td_api::inlineKeyboardButtonTypeWebApp *>(lhs->type_.get());
      auto rhs_type = static_cast<const td_api::inlineKeyboardButtonTypeWebApp *>(rhs->type_.get());
      return lhs_type->url_ == rhs_type->url_;
    }
    default:
      UNREACHABLE();
      return false;
  }
}

bool Client::are_equal_inline_keyboards(const td_api::replyMarkupInlineKeyboard *lhs,
                                        const td_api::replyMarkupInlineKeyboard *rhs) {
  CHECK(lhs != nullptr);
  CHECK(rhs != nullptr);
  auto &old_rows = lhs->rows_;
  auto &new_rows = rhs->rows_;
  if (old_rows.size() != new_rows.size()) {
    return false;
  }
  for (size_t i = 0; i < old_rows.size(); i++) {
    if (old_rows[i].size() != new_rows[i].size()) {
      return false;
    }
    for (size_t j = 0; j < old_rows[i].size(); j++) {
      if (!are_equal_inline_keyboard_buttons(old_rows[i][j].get(), new_rows[i][j].get())) {
        return false;
      }
    }
  }
  return true;
}

void Client::set_message_reply_markup(MessageInfo *message_info, object_ptr<td_api::ReplyMarkup> &&reply_markup) {
  if (reply_markup != nullptr && reply_markup->get_id() != td_api::replyMarkupInlineKeyboard::ID) {
    reply_markup = nullptr;
  }
  if (reply_markup == nullptr && message_info->reply_markup == nullptr) {
    return;
  }
  if (reply_markup != nullptr && message_info->reply_markup != nullptr) {
    CHECK(message_info->reply_markup->get_id() == td_api::replyMarkupInlineKeyboard::ID);
    if (are_equal_inline_keyboards(
            static_cast<const td_api::replyMarkupInlineKeyboard *>(message_info->reply_markup.get()),
            static_cast<const td_api::replyMarkupInlineKeyboard *>(reply_markup.get()))) {
      return;
    }
  }
  message_info->reply_markup = std::move(reply_markup);
  message_info->is_content_changed = true;
}

td::int64 Client::get_sticker_set_id(const object_ptr<td_api::MessageContent> &content) {
  if (content->get_id() != td_api::messageSticker::ID) {
    return 0;
  }

  return static_cast<const td_api::messageSticker *>(content.get())->sticker_->set_id_;
}

bool Client::have_sticker_set_name(int64 sticker_set_id) const {
  return sticker_set_id == 0 || sticker_set_names_.count(sticker_set_id) > 0;
}

Client::Slice Client::get_sticker_set_name(int64 sticker_set_id) const {
  auto it = sticker_set_names_.find(sticker_set_id);
  if (it == sticker_set_names_.end()) {
    return Slice();
  }
  return it->second;
}

void Client::process_new_message_queue(int64 chat_id) {
  auto &queue = new_message_queues_[chat_id];
  if (queue.has_active_request_) {
    return;
  }
  if (logging_out_ || closing_) {
    new_message_queues_.erase(chat_id);
    return;
  }
  while (!queue.queue_.empty()) {
    auto &message_ref = queue.queue_.front().message;
    CHECK(chat_id == message_ref->chat_id_);
    int64 message_id = message_ref->id_;
    int64 reply_to_message_id = get_reply_to_message_id(message_ref);
    if (reply_to_message_id > 0 && get_message(chat_id, reply_to_message_id) == nullptr) {
      queue.has_active_request_ = true;
      return send_request(make_object<td_api::getRepliedMessage>(chat_id, message_id),
                          td::make_unique<TdOnGetReplyMessageCallback>(this, chat_id));
    }
    auto message_sticker_set_id = get_sticker_set_id(message_ref->content_);
    if (!have_sticker_set_name(message_sticker_set_id)) {
      queue.has_active_request_ = true;
      return send_request(make_object<td_api::getStickerSet>(message_sticker_set_id),
                          td::make_unique<TdOnGetStickerSetCallback>(this, message_sticker_set_id, 0, chat_id));
    }
    if (reply_to_message_id > 0) {
      auto reply_to_message_info = get_message(chat_id, reply_to_message_id);
      CHECK(reply_to_message_info != nullptr);
      auto reply_sticker_set_id = get_sticker_set_id(reply_to_message_info->content);
      if (!have_sticker_set_name(reply_sticker_set_id)) {
        queue.has_active_request_ = true;
        return send_request(make_object<td_api::getStickerSet>(reply_sticker_set_id),
                            td::make_unique<TdOnGetStickerSetCallback>(this, reply_sticker_set_id, 0, chat_id));
      }
    }

    auto message = std::move(message_ref);
    auto is_edited = queue.queue_.front().is_edited;
    queue.queue_.pop();
    if (need_skip_update_message(chat_id, message, is_edited)) {
      add_message(std::move(message));
      continue;
    }

    auto chat = get_chat(chat_id);
    CHECK(chat != nullptr);
    bool is_channel_post =
        (chat->type == ChatInfo::Type::Supergroup && !get_supergroup_info(chat->supergroup_id)->is_supergroup);

    UpdateType update_type;
    if (is_channel_post) {
      update_type = is_edited ? UpdateType::EditedChannelPost : UpdateType::ChannelPost;
    } else {
      update_type = is_edited ? UpdateType::EditedMessage : UpdateType::Message;
    }

    int32 message_date = message->edit_date_ == 0 ? message->date_ : message->edit_date_;
    auto now = get_unix_time();
    auto update_delay_time = now - td::max(message_date, parameters_->shared_data_->get_unix_time(webhook_set_time_));
    const auto UPDATE_DELAY_WARNING_TIME = 10 * 60;
    LOG_IF(ERROR, update_delay_time > UPDATE_DELAY_WARNING_TIME)
        << "Receive very old update " << get_update_type_name(update_type) << " sent at " << message_date << " to chat "
        << chat_id << " with a delay of " << update_delay_time << " seconds: " << to_string(message);
    auto left_time = message_date + 86400 - now;
    add_message(std::move(message));

    auto message_info = get_message(chat_id, message_id);
    CHECK(message_info != nullptr);

    message_info->is_content_changed = false;
    add_update(update_type, JsonMessage(message_info, true, get_update_type_name(update_type).str(), this), left_time,
               chat_id);
  }
  new_message_queues_.erase(chat_id);
}

void Client::remove_replies_to_message(int64 chat_id, int64 reply_to_message_id, bool only_from_cache) {
  if (!only_from_cache) {
    auto yet_unsent_it = yet_unsent_reply_message_ids_.find({chat_id, reply_to_message_id});
    if (yet_unsent_it != yet_unsent_reply_message_ids_.end()) {
      for (auto message_id : yet_unsent_it->second) {
        auto &message = yet_unsent_messages_[{chat_id, message_id}];
        CHECK(message.reply_to_message_id == reply_to_message_id);
        message.is_reply_to_message_deleted = true;
      }
    }
  }

  auto it = reply_message_ids_.find({chat_id, reply_to_message_id});
  if (it == reply_message_ids_.end()) {
    return;
  }

  if (!only_from_cache) {
    for (auto message_id : it->second) {
      auto message_info = get_message_editable(chat_id, message_id);
      CHECK(message_info != nullptr);
      CHECK(message_info->reply_to_message_id == reply_to_message_id);
      message_info->reply_to_message_id = 0;
    }
  }
  reply_message_ids_.erase(it);
}

void Client::delete_message(int64 chat_id, int64 message_id, bool only_from_cache) {
  remove_replies_to_message(chat_id, message_id, only_from_cache);

  auto it = messages_.find({chat_id, message_id});
  if (it == messages_.end()) {
    if (yet_unsent_messages_.count({chat_id, message_id}) > 0) {
      // yet unsent message is deleted, possible only if we are trying to write to inaccessible supergroup or
      // sent message was deleted before added to the chat
      auto chat_info = get_chat(chat_id);
      CHECK(chat_info != nullptr);

      Status error =
          Status::Error(500, "Internal Server Error: sent message was immediately deleted and can't be returned");
      if (chat_info->type == ChatInfo::Type::Supergroup) {
        auto supergroup_info = get_supergroup_info(chat_info->supergroup_id);
        CHECK(supergroup_info != nullptr);
        if (supergroup_info->status->get_id() == td_api::chatMemberStatusBanned::ID ||
            supergroup_info->status->get_id() == td_api::chatMemberStatusLeft::ID) {
          if (supergroup_info->is_supergroup) {
            error = Status::Error(403, "Forbidden: bot is not a member of the supergroup chat");
          } else {
            error = Status::Error(403, "Forbidden: bot is not a member of the channel chat");
          }
        }
      }

      on_message_send_failed(chat_id, message_id, 0, std::move(error));
    }
    return;
  }

  auto message_info = it->second.get();

  set_message_reply_to_message_id(message_info, 0);

  messages_.erase(it);
}

Client::FullMessageId Client::add_message(object_ptr<td_api::message> &&message, bool force_update_content) {
  CHECK(message != nullptr);
  CHECK(message->sending_state_ == nullptr);

  int64 chat_id = message->chat_id_;
  int64 message_id = message->id_;

  LOG(DEBUG) << "Add message " << message_id << " to chat " << chat_id;
  td::unique_ptr<MessageInfo> message_info;
  auto it = messages_.find({chat_id, message_id});
  if (it == messages_.end()) {
    message_info = td::make_unique<MessageInfo>();
  } else {
    message_info = std::move(it->second);
  }

  message_info->id = message_id;
  message_info->chat_id = chat_id;
  message_info->date = message->date_;
  message_info->edit_date = message->edit_date_;
  message_info->media_album_id = message->media_album_id_;
  message_info->via_bot_user_id = message->via_bot_user_id_;

  message_info->initial_chat_id = 0;
  message_info->initial_sender_user_id = 0;
  message_info->initial_sender_chat_id = 0;
  message_info->initial_send_date = 0;
  message_info->initial_message_id = 0;
  message_info->initial_author_signature = td::string();
  message_info->initial_sender_name = td::string();
  if (message->forward_info_ != nullptr) {
    message_info->initial_send_date = message->forward_info_->date_;
    auto origin = std::move(message->forward_info_->origin_);
    switch (origin->get_id()) {
      case td_api::messageForwardOriginUser::ID: {
        auto forward_info = move_object_as<td_api::messageForwardOriginUser>(origin);
        message_info->initial_sender_user_id = forward_info->sender_user_id_;
        break;
      }
      case td_api::messageForwardOriginChat::ID: {
        auto forward_info = move_object_as<td_api::messageForwardOriginChat>(origin);
        message_info->initial_sender_chat_id = forward_info->sender_chat_id_;
        message_info->initial_author_signature = std::move(forward_info->author_signature_);
        break;
      }
      case td_api::messageForwardOriginHiddenUser::ID: {
        auto forward_info = move_object_as<td_api::messageForwardOriginHiddenUser>(origin);
        message_info->initial_sender_name = std::move(forward_info->sender_name_);
        break;
      }
      case td_api::messageForwardOriginChannel::ID: {
        auto forward_info = move_object_as<td_api::messageForwardOriginChannel>(origin);
        message_info->initial_chat_id = forward_info->chat_id_;
        message_info->initial_message_id = forward_info->message_id_;
        message_info->initial_author_signature = std::move(forward_info->author_signature_);
        break;
      }
      case td_api::messageForwardOriginMessageImport::ID: {
        auto forward_info = move_object_as<td_api::messageForwardOriginMessageImport>(origin);
        message_info->initial_sender_name = std::move(forward_info->sender_name_);
        break;
      }
      default:
        UNREACHABLE();
    }
    auto from_chat_id = message->forward_info_->from_chat_id_;
    message_info->is_automatic_forward =
        from_chat_id != 0 && from_chat_id != chat_id && message->forward_info_->from_message_id_ != 0 &&
        get_chat_type(chat_id) == ChatType::Supergroup && get_chat_type(from_chat_id) == ChatType::Channel;
  }

  CHECK(message->sender_id_ != nullptr);
  switch (message->sender_id_->get_id()) {
    case td_api::messageSenderUser::ID: {
      auto sender_id = move_object_as<td_api::messageSenderUser>(message->sender_id_);
      message_info->sender_user_id = sender_id->user_id_;
      CHECK(message_info->sender_user_id > 0);
      break;
    }
    case td_api::messageSenderChat::ID: {
      auto sender_id = move_object_as<td_api::messageSenderChat>(message->sender_id_);
      message_info->sender_chat_id = sender_id->chat_id_;

      auto chat_type = get_chat_type(chat_id);
      if (chat_type != ChatType::Channel) {
        if (message_info->sender_chat_id == chat_id) {
          message_info->sender_user_id = group_anonymous_bot_user_id_;
        } else if (message_info->is_automatic_forward) {
          message_info->sender_user_id = service_notifications_user_id_;
        } else {
          message_info->sender_user_id = channel_bot_user_id_;
        }
        CHECK(message_info->sender_user_id > 0);
      }
      break;
    }
    default:
      UNREACHABLE();
  }

  message_info->can_be_saved = message->can_be_saved_;
  message_info->author_signature = std::move(message->author_signature_);

  if (message->reply_in_chat_id_ != chat_id && message->reply_to_message_id_ != 0) {
    LOG(WARNING) << "Drop reply to message " << message_id << " in chat " << chat_id << " from another chat "
                 << message->reply_in_chat_id_;
    message->reply_in_chat_id_ = 0;
    message->reply_to_message_id_ = 0;
  }
  set_message_reply_to_message_id(message_info.get(), message->reply_to_message_id_);
  if (message_info->content == nullptr || force_update_content) {
    message_info->content = std::move(message->content_);
    message_info->is_content_changed = true;

    auto sticker_set_id = get_sticker_set_id(message_info->content);
    if (!have_sticker_set_name(sticker_set_id)) {
      send_request(make_object<td_api::getStickerSet>(sticker_set_id),
                   td::make_unique<TdOnGetStickerSetCallback>(this, sticker_set_id, 0, 0));
    }
  }
  set_message_reply_markup(message_info.get(), std::move(message->reply_markup_));

  messages_[{chat_id, message_id}] = std::move(message_info);
  message = nullptr;

  return {chat_id, message_id};
}

void Client::update_message_content(int64 chat_id, int64 message_id, object_ptr<td_api::MessageContent> &&content) {
  auto message_info = get_message_editable(chat_id, message_id);
  if (message_info == nullptr) {
    return;
  }
  LOG(DEBUG) << "Update content of the message " << message_id << " from chat " << chat_id;

  message_info->content = std::move(content);
  message_info->is_content_changed = true;
}

void Client::on_update_message_edited(int64 chat_id, int64 message_id, int32 edit_date,
                                      object_ptr<td_api::ReplyMarkup> &&reply_markup) {
  auto message_info = get_message_editable(chat_id, message_id);
  if (message_info == nullptr) {
    return;
  }
  message_info->edit_date = edit_date;
  set_message_reply_markup(message_info, std::move(reply_markup));
}

const Client::MessageInfo *Client::get_message(int64 chat_id, int64 message_id) const {
  auto it = messages_.find({chat_id, message_id});
  if (it == messages_.end()) {
    LOG(DEBUG) << "Not found message " << message_id << " from chat " << chat_id;
    return nullptr;
  }
  LOG(DEBUG) << "Found message " << message_id << " from chat " << chat_id;

  return it->second.get();
}

Client::MessageInfo *Client::get_message_editable(int64 chat_id, int64 message_id) {
  auto it = messages_.find({chat_id, message_id});
  if (it == messages_.end()) {
    LOG(DEBUG) << "Not found message " << message_id << " from chat " << chat_id;
    return nullptr;
  }
  LOG(DEBUG) << "Found message " << message_id << " from chat " << chat_id;

  return it->second.get();
}

td::string Client::get_chat_member_status(const object_ptr<td_api::ChatMemberStatus> &status) {
  CHECK(status != nullptr);
  switch (status->get_id()) {
    case td_api::chatMemberStatusCreator::ID:
      return "creator";
    case td_api::chatMemberStatusAdministrator::ID:
      return "administrator";
    case td_api::chatMemberStatusMember::ID:
      return "member";
    case td_api::chatMemberStatusRestricted::ID:
      return "restricted";
    case td_api::chatMemberStatusLeft::ID:
      return "left";
    case td_api::chatMemberStatusBanned::ID:
      return "kicked";
    default:
      UNREACHABLE();
      return "";
  }
}

td::string Client::get_passport_element_type(int32 id) {
  switch (id) {
    case td_api::passportElementTypePersonalDetails::ID:
      return "personal_details";
    case td_api::passportElementTypePassport::ID:
      return "passport";
    case td_api::passportElementTypeDriverLicense::ID:
      return "driver_license";
    case td_api::passportElementTypeIdentityCard::ID:
      return "identity_card";
    case td_api::passportElementTypeInternalPassport::ID:
      return "internal_passport";
    case td_api::passportElementTypeAddress::ID:
      return "address";
    case td_api::passportElementTypeUtilityBill::ID:
      return "utility_bill";
    case td_api::passportElementTypeBankStatement::ID:
      return "bank_statement";
    case td_api::passportElementTypeRentalAgreement::ID:
      return "rental_agreement";
    case td_api::passportElementTypePassportRegistration::ID:
      return "passport_registration";
    case td_api::passportElementTypeTemporaryRegistration::ID:
      return "temporary_registration";
    case td_api::passportElementTypePhoneNumber::ID:
      return "phone_number";
    case td_api::passportElementTypeEmailAddress::ID:
      return "email";
    default:
      UNREACHABLE();
      return "None";
  }
}

td_api::object_ptr<td_api::PassportElementType> Client::get_passport_element_type(Slice type) {
  if (type == "personal_details") {
    return make_object<td_api::passportElementTypePersonalDetails>();
  }
  if (type == "passport") {
    return make_object<td_api::passportElementTypePassport>();
  }
  if (type == "driver_license") {
    return make_object<td_api::passportElementTypeDriverLicense>();
  }
  if (type == "identity_card") {
    return make_object<td_api::passportElementTypeIdentityCard>();
  }
  if (type == "internal_passport") {
    return make_object<td_api::passportElementTypeInternalPassport>();
  }
  if (type == "address") {
    return make_object<td_api::passportElementTypeAddress>();
  }
  if (type == "utility_bill") {
    return make_object<td_api::passportElementTypeUtilityBill>();
  }
  if (type == "bank_statement") {
    return make_object<td_api::passportElementTypeBankStatement>();
  }
  if (type == "rental_agreement") {
    return make_object<td_api::passportElementTypeRentalAgreement>();
  }
  if (type == "passport_registration") {
    return make_object<td_api::passportElementTypePassportRegistration>();
  }
  if (type == "temporary_registration") {
    return make_object<td_api::passportElementTypeTemporaryRegistration>();
  }
  if (type == "phone_number") {
    return make_object<td_api::passportElementTypePhoneNumber>();
  }
  if (type == "email") {
    return make_object<td_api::passportElementTypeEmailAddress>();
  }
  return nullptr;
}

td::int32 Client::get_unix_time() const {
  CHECK(was_authorized_);
  return parameters_->shared_data_->get_unix_time(td::Time::now());
}

td::int64 Client::as_tdlib_message_id(int32 message_id) {
  return static_cast<int64>(message_id) << 20;
}

td::int32 Client::as_client_message_id(int64 message_id) {
  auto result = static_cast<int32>(message_id >> 20);
  CHECK(as_tdlib_message_id(result) == message_id);
  return result;
}

td::int64 Client::get_supergroup_chat_id(int64 supergroup_id) {
  return static_cast<td::int64>(-1000000000000ll) - supergroup_id;
}

td::int64 Client::get_basic_group_chat_id(int64 basic_group_id) {
  return -basic_group_id;
}

constexpr Client::int64 Client::GREAT_MINDS_SET_ID;
constexpr Client::Slice Client::GREAT_MINDS_SET_NAME;

constexpr Client::Slice Client::MASK_POINTS[MASK_POINTS_SIZE];

constexpr int Client::LOGGING_OUT_ERROR_CODE;
constexpr Client::Slice Client::LOGGING_OUT_ERROR_DESCRIPTION;
constexpr Client::Slice Client::API_ID_INVALID_ERROR_DESCRIPTION;

constexpr int Client::CLOSING_ERROR_CODE;
constexpr Client::Slice Client::CLOSING_ERROR_DESCRIPTION;

td::FlatHashMap<td::string, td::Status (Client::*)(PromisedQueryPtr &query)> Client::methods_;

}  // namespace telegram_bot_api
