//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2021
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "telegram-bot-api/Query.h"
#include "telegram-bot-api/Stats.h"
#include "telegram-bot-api/WebhookActor.h"

#include "td/telegram/ClientActor.h"
#include "td/telegram/td_api.h"

#include "td/net/HttpFile.h"

#include "td/actor/actor.h"
#include "td/actor/PromiseFuture.h"
#include "td/actor/SignalSlot.h"

#include "td/utils/common.h"
#include "td/utils/Container.h"
#include "td/utils/FlatHashMap.h"
#include "td/utils/FlatHashSet.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"

#include <functional>
#include <limits>
#include <memory>
#include <queue>

namespace telegram_bot_api {

struct ClientParameters;

namespace td_api = td::td_api;

class Client final : public WebhookActor::Callback {
 public:
  Client(td::ActorShared<> parent, const td::string &bot_token, bool is_test_dc, td::int64 tqueue_id,
         std::shared_ptr<const ClientParameters> parameters, td::ActorId<BotStatActor> stat_actor);

  void send(PromisedQueryPtr query) final;

  void close();

  // for stats
  ServerBotInfo get_bot_info() const;

 private:
  using int32 = td::int32;
  using int64 = td::int64;

  using Slice = td::Slice;
  using Status = td::Status;

  template <class T>
  using object_ptr = td_api::object_ptr<T>;

  static constexpr bool USE_MESSAGE_DATABASE = false;

  static constexpr int32 MAX_CERTIFICATE_FILE_SIZE = 3 << 20;
  static constexpr int32 MAX_DOWNLOAD_FILE_SIZE = 20 << 20;

  static constexpr int32 MAX_CONCURRENTLY_SENT_CHAT_MESSAGES = 1000;  // some unreasonably big value

  static constexpr std::size_t MIN_PENDING_UPDATES_WARNING = 200;

  static constexpr int64 GREAT_MINDS_SET_ID = 1842540969984001;
  static constexpr Slice GREAT_MINDS_SET_NAME = "TelegramGreatMinds";

  static constexpr int32 MASK_POINTS_SIZE = 4;
  static constexpr Slice MASK_POINTS[MASK_POINTS_SIZE] = {"forehead", "eyes", "mouth", "chin"};

  static constexpr int32 MAX_LENGTH = 10000;  // max width or height
  static constexpr int32 MAX_DURATION = 24 * 60 * 60;

  static constexpr int LOGGING_OUT_ERROR_CODE = 401;
  static constexpr Slice LOGGING_OUT_ERROR_DESCRIPTION = "Unauthorized";
  static constexpr Slice API_ID_INVALID_ERROR_DESCRIPTION = "Unauthorized: invalid api-id/api-hash";

  static constexpr int CLOSING_ERROR_CODE = 500;
  static constexpr Slice CLOSING_ERROR_DESCRIPTION = "Internal Server Error: restart";

  class JsonFile;
  class JsonDatedFile;
  class JsonDatedFiles;
  class JsonUser;
  class JsonUsers;
  class JsonChatPermissions;
  class JsonChatPhotoInfo;
  class JsonChatLocation;
  class JsonChatInviteLink;
  class JsonChat;
  class JsonMessageSender;
  class JsonAnimation;
  class JsonAudio;
  class JsonDocument;
  class JsonPhotoSize;
  class JsonPhoto;
  class JsonChatPhoto;
  class JsonThumbnail;
  class JsonMaskPosition;
  class JsonSticker;
  class JsonStickers;
  class JsonVideo;
  class JsonVideoNote;
  class JsonVoiceNote;
  class JsonContact;
  class JsonDice;
  class JsonGame;
  class JsonInvoice;
  class JsonLocation;
  class JsonVenue;
  class JsonPollOption;
  class JsonPoll;
  class JsonPollAnswer;
  class JsonEntity;
  class JsonVectorEntities;
  class JsonCallbackGame;
  class JsonWebAppInfo;
  class JsonInlineKeyboardButton;
  class JsonInlineKeyboard;
  class JsonReplyMarkup;
  class JsonMessage;
  class JsonMessages;
  class JsonDeletedMessage;
  class JsonMessageId;
  class JsonInlineQuery;
  class JsonChosenInlineResult;
  class JsonCallbackQuery;
  class JsonInlineCallbackQuery;
  class JsonShippingQuery;
  class JsonPreCheckoutQuery;
  class JsonBotCommand;
  class JsonBotMenuButton;
  class JsonChatAdministratorRights;
  class JsonChatPhotos;
  class JsonChatMember;
  class JsonChatMembers;
  class JsonChatMemberUpdated;
  class JsonChatJoinRequest;
  class JsonGameHighScore;
  class JsonAddress;
  class JsonOrderInfo;
  class JsonSuccessfulPaymentBot;
  class JsonEncryptedPassportElement;
  class JsonEncryptedCredentials;
  class JsonPassportData;
  class JsonWebAppData;
  class JsonProximityAlertTriggered;
  class JsonVideoChatScheduled;
  class JsonVideoChatStarted;
  class JsonVideoChatEnded;
  class JsonInviteVideoChatParticipants;
  class JsonChatSetTtl;
  class JsonUpdateTypes;
  class JsonWebhookInfo;
  class JsonStickerSet;
  class JsonSentWebAppMessage;
  class JsonCustomJson;

  class TdOnOkCallback;
  class TdOnAuthorizationCallback;
  class TdOnInitCallback;
  class TdOnGetUserProfilePhotosCallback;
  class TdOnSendMessageCallback;
  class TdOnSendMessageAlbumCallback;
  class TdOnDeleteFailedToSendMessageCallback;
  class TdOnEditMessageCallback;
  class TdOnEditInlineMessageCallback;
  class TdOnStopPollCallback;
  class TdOnOkQueryCallback;
  class TdOnGetReplyMessageCallback;
  class TdOnGetEditedMessageCallback;
  class TdOnGetCallbackQueryMessageCallback;
  class TdOnGetStickerSetCallback;
  class TdOnGetMenuButtonCallback;
  class TdOnGetMyCommandsCallback;
  class TdOnGetMyDefaultAdministratorRightsCallback;
  class TdOnGetChatFullInfoCallback;
  class TdOnGetChatStickerSetCallback;
  class TdOnGetChatPinnedMessageCallback;
  class TdOnGetChatPinnedMessageToUnpinCallback;
  class TdOnGetGroupMembersCallback;
  class TdOnGetSupergroupMembersCallback;
  class TdOnGetSupergroupMembersCountCallback;
  class TdOnCreateInvoiceLinkCallback;
  class TdOnReplacePrimaryChatInviteLinkCallback;
  class TdOnGetChatInviteLinkCallback;
  class TdOnGetGameHighScoresCallback;
  class TdOnAnswerWebAppQueryCallback;
  class TdOnReturnFileCallback;
  class TdOnReturnStickerSetCallback;
  class TdOnDownloadFileCallback;
  class TdOnCancelDownloadFileCallback;
  class TdOnSendCustomRequestCallback;

  void on_get_reply_message(int64 chat_id, object_ptr<td_api::message> reply_to_message);

  void on_get_edited_message(object_ptr<td_api::message> edited_message);

  void on_get_callback_query_message(object_ptr<td_api::message> message, int64 user_id, int state);

  void on_get_sticker_set(int64 set_id, int64 new_callback_query_user_id, int64 new_message_chat_id,
                          object_ptr<td_api::stickerSet> sticker_set);

  void on_get_sticker_set_name(int64 set_id, const td::string &name);

  class TdQueryCallback {
   public:
    virtual void on_result(object_ptr<td_api::Object> result) = 0;
    TdQueryCallback() = default;
    TdQueryCallback(const TdQueryCallback &) = delete;
    TdQueryCallback &operator=(const TdQueryCallback &) = delete;
    TdQueryCallback(TdQueryCallback &&) = delete;
    TdQueryCallback &operator=(TdQueryCallback &&) = delete;
    virtual ~TdQueryCallback() = default;
  };

  struct UserInfo;
  struct ChatInfo;
  struct BotCommandScope;

  enum class AccessRights { Read, ReadMembers, Edit, Write };

  template <class OnSuccess>
  class TdOnCheckUserCallback;
  template <class OnSuccess>
  class TdOnCheckUserNoFailCallback;
  template <class OnSuccess>
  class TdOnCheckChatCallback;
  template <class OnSuccess>
  class TdOnCheckChatNoFailCallback;
  template <class OnSuccess>
  class TdOnCheckMessageCallback;
  template <class OnSuccess>
  class TdOnCheckRemoteFileIdCallback;
  template <class OnSuccess>
  class TdOnGetChatMemberCallback;

  template <class OnSuccess>
  class TdOnSearchStickerSetCallback;

  class TdOnResolveBotUsernameCallback;

  template <class OnSuccess>
  void check_user(int64 user_id, PromisedQueryPtr query, OnSuccess on_success);

  template <class OnSuccess>
  void check_user_no_fail(int64 user_id, PromisedQueryPtr query, OnSuccess on_success);

  template <class OnSuccess>
  static void check_user_read_access(const UserInfo *user_info, PromisedQueryPtr query, OnSuccess on_success);

  template <class OnSuccess>
  void check_chat_access(int64 chat_id, AccessRights access_rights, const ChatInfo *chat_info, PromisedQueryPtr query,
                         OnSuccess on_success) const;

  template <class OnSuccess>
  void check_chat(Slice chat_id_str, AccessRights access_rights, PromisedQueryPtr query, OnSuccess on_success);

  template <class OnSuccess>
  void check_chat_no_fail(Slice chat_id_str, PromisedQueryPtr query, OnSuccess on_success);

  template <class OnSuccess>
  void check_bot_command_scope(BotCommandScope &&scope, PromisedQueryPtr query, OnSuccess on_success);

  template <class OnSuccess>
  void check_remote_file_id(td::string file_id, PromisedQueryPtr query, OnSuccess on_success);

  template <class OnSuccess>
  void check_message(Slice chat_id_str, int64 message_id, bool allow_empty, AccessRights access_rights,
                     Slice message_type, PromisedQueryPtr query, OnSuccess on_success);

  template <class OnSuccess>
  void resolve_sticker_set(const td::string &sticker_set_name, PromisedQueryPtr query, OnSuccess on_success);

  template <class OnSuccess>
  void resolve_reply_markup_bot_usernames(object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query,
                                          OnSuccess on_success);

  template <class OnSuccess>
  void resolve_inline_query_results_bot_usernames(td::vector<object_ptr<td_api::InputInlineQueryResult>> results,
                                                  PromisedQueryPtr query, OnSuccess on_success);

  template <class OnSuccess>
  void get_chat_member(int64 chat_id, int64 user_id, PromisedQueryPtr query, OnSuccess on_success);

  void send_request(object_ptr<td_api::Function> &&f, td::unique_ptr<TdQueryCallback> handler);
  void do_send_request(object_ptr<td_api::Function> &&f, td::unique_ptr<TdQueryCallback> handler);
  static object_ptr<td_api::Object> execute(object_ptr<td_api::Function> &&f);
  void on_update(object_ptr<td_api::Object> result);
  void on_result(td::uint64 id, object_ptr<td_api::Object> result);

  void on_update_authorization_state();
  void log_out(bool is_api_id_invalid);
  Slice get_logging_out_error_description() const;
  void on_closed();
  void finish_closing();

  void clear_tqueue();

  bool allow_update_before_authorization(const td_api::Object *update) const;
  void update_shared_unix_time_difference();

  void on_update_file(object_ptr<td_api::file> file);

  static bool to_bool(td::MutableSlice value);

  static td::Result<object_ptr<td_api::keyboardButton>> get_keyboard_button(td::JsonValue &button);

  td::Result<object_ptr<td_api::inlineKeyboardButton>> get_inline_keyboard_button(td::JsonValue &button);

  td::Result<object_ptr<td_api::ReplyMarkup>> get_reply_markup(const Query *query);

  td::Result<object_ptr<td_api::ReplyMarkup>> get_reply_markup(td::JsonValue &&value);

  static td::Result<object_ptr<td_api::labeledPricePart>> get_labeled_price_part(td::JsonValue &value);

  static td::Result<td::vector<object_ptr<td_api::labeledPricePart>>> get_labeled_price_parts(td::JsonValue &value);

  static td::Result<td::vector<int64>> get_suggested_tip_amounts(td::JsonValue &value);

  static td::Result<object_ptr<td_api::shippingOption>> get_shipping_option(td::JsonValue &option);

  static td::Result<td::vector<object_ptr<td_api::shippingOption>>> get_shipping_options(const Query *query);

  static td::Result<td::vector<object_ptr<td_api::shippingOption>>> get_shipping_options(td::JsonValue &&value);

  static td::Result<object_ptr<td_api::InputMessageContent>> get_input_message_content(
      td::JsonValue &input_message_content, bool is_input_message_content_required);

  static object_ptr<td_api::ChatAction> get_chat_action(const Query *query);

  static td::string get_local_file_path(Slice file_uri);

  object_ptr<td_api::InputFile> get_input_file(const Query *query, Slice field_name, bool force_file = false) const;

  object_ptr<td_api::InputFile> get_input_file(const Query *query, Slice field_name, Slice file_id,
                                               bool force_file) const;

  object_ptr<td_api::inputThumbnail> get_input_thumbnail(const Query *query, Slice field_name) const;

  td::Result<object_ptr<td_api::InputInlineQueryResult>> get_inline_query_result(const Query *query);

  td::Result<object_ptr<td_api::InputInlineQueryResult>> get_inline_query_result(td::JsonValue &&value);

  td::Result<td::vector<object_ptr<td_api::InputInlineQueryResult>>> get_inline_query_results(const Query *query);

  td::Result<td::vector<object_ptr<td_api::InputInlineQueryResult>>> get_inline_query_results(td::JsonValue &&value);

  struct BotCommandScope {
    object_ptr<td_api::BotCommandScope> scope_;
    td::string chat_id_;
    td::int64 user_id_ = 0;

    explicit BotCommandScope(object_ptr<td_api::BotCommandScope> scope, td::string chat_id = td::string(),
                             td::int64 user_id = 0)
        : scope_(std::move(scope)), chat_id_(std::move(chat_id)), user_id_(user_id) {
    }
  };

  static td::Result<BotCommandScope> get_bot_command_scope(const Query *query);

  static td::Result<BotCommandScope> get_bot_command_scope(td::JsonValue &&value);

  static td::Result<object_ptr<td_api::botCommand>> get_bot_command(td::JsonValue &&value);

  static td::Result<td::vector<object_ptr<td_api::botCommand>>> get_bot_commands(const Query *query);

  static td::Result<object_ptr<td_api::botMenuButton>> get_bot_menu_button(const Query *query);

  static td::Result<object_ptr<td_api::botMenuButton>> get_bot_menu_button(td::JsonValue &&value);

  static td::Result<object_ptr<td_api::chatAdministratorRights>> get_chat_administrator_rights(td::JsonValue &&value);

  static td::Result<object_ptr<td_api::chatAdministratorRights>> get_chat_administrator_rights(const Query *query);

  static td::Result<object_ptr<td_api::maskPosition>> get_mask_position(const Query *query, Slice field_name);

  static td::Result<object_ptr<td_api::maskPosition>> get_mask_position(td::JsonValue &&value);

  static int32 mask_point_to_index(const object_ptr<td_api::MaskPoint> &mask_point);

  static object_ptr<td_api::MaskPoint> mask_index_to_point(int32 index);

  td::Result<td::vector<object_ptr<td_api::inputSticker>>> get_input_stickers(const Query *query, bool is_masks) const;

  static td::Result<td::string> get_passport_element_hash(Slice encoded_hash);

  static td::Result<object_ptr<td_api::InputPassportElementErrorSource>> get_passport_element_error_source(
      td::JsonObject &object);

  static td::Result<object_ptr<td_api::inputPassportElementError>> get_passport_element_error(td::JsonValue &&value);

  static td::Result<td::vector<object_ptr<td_api::inputPassportElementError>>> get_passport_element_errors(
      const Query *query);

  static td::JsonValue get_input_entities(const Query *query, Slice field_name);

  static td::Result<object_ptr<td_api::formattedText>> get_caption(const Query *query);

  static td::Result<object_ptr<td_api::TextEntityType>> get_text_entity_type(td::JsonObject &object);

  static td::Result<object_ptr<td_api::textEntity>> get_text_entity(td::JsonValue &&value);

  static td::Result<object_ptr<td_api::formattedText>> get_formatted_text(td::string text, td::string parse_mode,
                                                                          td::JsonValue &&input_entities);

  static td::Result<object_ptr<td_api::inputMessageText>> get_input_message_text(const Query *query);

  static td::Result<object_ptr<td_api::inputMessageText>> get_input_message_text(td::string text,
                                                                                 bool disable_web_page_preview,
                                                                                 td::string parse_mode,
                                                                                 td::JsonValue &&input_entities);

  static td::Result<object_ptr<td_api::location>> get_location(const Query *query);

  static td::Result<object_ptr<td_api::chatPermissions>> get_chat_permissions(const Query *query, bool &allow_legacy);

  td::Result<object_ptr<td_api::InputMessageContent>> get_input_media(const Query *query, td::JsonValue &&input_media,
                                                                      bool for_album) const;

  td::Result<object_ptr<td_api::InputMessageContent>> get_input_media(const Query *query, Slice field_name,
                                                                      bool for_album) const;

  td::Result<td::vector<object_ptr<td_api::InputMessageContent>>> get_input_message_contents(const Query *query,
                                                                                             Slice field_name) const;

  td::Result<td::vector<object_ptr<td_api::InputMessageContent>>> get_input_message_contents(
      const Query *query, td::JsonValue &&value) const;

  static td::Result<object_ptr<td_api::inputMessageInvoice>> get_input_message_invoice(const Query *query);

  static object_ptr<td_api::messageSendOptions> get_message_send_options(bool disable_notification,
                                                                         bool protect_content);

  static td::Result<td::vector<td::string>> get_poll_options(const Query *query);

  static int32 get_integer_arg(const Query *query, Slice field_name, int32 default_value,
                               int32 min_value = std::numeric_limits<int32>::min(),
                               int32 max_value = std::numeric_limits<int32>::max());

  static td::Result<td::MutableSlice> get_required_string_arg(const Query *query, Slice field_name);

  static int64 get_message_id(const Query *query, Slice field_name = Slice("message_id"));

  static td::Result<Slice> get_inline_message_id(const Query *query, Slice field_name = Slice("inline_message_id"));

  static td::Result<int64> get_user_id(const Query *query, Slice field_name = Slice("user_id"));

  int64 extract_yet_unsent_message_query_id(int64 chat_id, int64 message_id, bool *is_reply_to_message_deleted);

  void on_message_send_succeeded(object_ptr<td_api::message> &&message, int64 old_message_id);
  void on_message_send_failed(int64 chat_id, int64 old_message_id, int64 new_message_id, Status result);

  static bool init_methods();

  void on_cmd(PromisedQueryPtr query);

  Status process_get_me_query(PromisedQueryPtr &query);
  Status process_get_my_commands_query(PromisedQueryPtr &query);
  Status process_set_my_commands_query(PromisedQueryPtr &query);
  Status process_delete_my_commands_query(PromisedQueryPtr &query);
  Status process_get_my_default_administrator_rights_query(PromisedQueryPtr &query);
  Status process_set_my_default_administrator_rights_query(PromisedQueryPtr &query);
  Status process_get_chat_menu_button_query(PromisedQueryPtr &query);
  Status process_set_chat_menu_button_query(PromisedQueryPtr &query);
  Status process_get_user_profile_photos_query(PromisedQueryPtr &query);
  Status process_send_message_query(PromisedQueryPtr &query);
  Status process_send_animation_query(PromisedQueryPtr &query);
  Status process_send_audio_query(PromisedQueryPtr &query);
  Status process_send_dice_query(PromisedQueryPtr &query);
  Status process_send_document_query(PromisedQueryPtr &query);
  Status process_send_photo_query(PromisedQueryPtr &query);
  Status process_send_sticker_query(PromisedQueryPtr &query);
  Status process_send_video_query(PromisedQueryPtr &query);
  Status process_send_video_note_query(PromisedQueryPtr &query);
  Status process_send_voice_query(PromisedQueryPtr &query);
  Status process_send_game_query(PromisedQueryPtr &query);
  Status process_send_invoice_query(PromisedQueryPtr &query);
  Status process_send_location_query(PromisedQueryPtr &query);
  Status process_send_venue_query(PromisedQueryPtr &query);
  Status process_send_contact_query(PromisedQueryPtr &query);
  Status process_send_poll_query(PromisedQueryPtr &query);
  Status process_stop_poll_query(PromisedQueryPtr &query);
  Status process_copy_message_query(PromisedQueryPtr &query);
  Status process_forward_message_query(PromisedQueryPtr &query);
  Status process_send_media_group_query(PromisedQueryPtr &query);
  Status process_send_chat_action_query(PromisedQueryPtr &query);
  Status process_edit_message_text_query(PromisedQueryPtr &query);
  Status process_edit_message_live_location_query(PromisedQueryPtr &query);
  Status process_edit_message_media_query(PromisedQueryPtr &query);
  Status process_edit_message_caption_query(PromisedQueryPtr &query);
  Status process_edit_message_reply_markup_query(PromisedQueryPtr &query);
  Status process_delete_message_query(PromisedQueryPtr &query);
  Status process_create_invoice_link_query(PromisedQueryPtr &query);
  Status process_set_game_score_query(PromisedQueryPtr &query);
  Status process_get_game_high_scores_query(PromisedQueryPtr &query);
  Status process_answer_web_app_query_query(PromisedQueryPtr &query);
  Status process_answer_inline_query_query(PromisedQueryPtr &query);
  Status process_answer_callback_query_query(PromisedQueryPtr &query);
  Status process_answer_shipping_query_query(PromisedQueryPtr &query);
  Status process_answer_pre_checkout_query_query(PromisedQueryPtr &query);
  Status process_export_chat_invite_link_query(PromisedQueryPtr &query);
  Status process_create_chat_invite_link_query(PromisedQueryPtr &query);
  Status process_edit_chat_invite_link_query(PromisedQueryPtr &query);
  Status process_revoke_chat_invite_link_query(PromisedQueryPtr &query);
  Status process_get_chat_query(PromisedQueryPtr &query);
  Status process_set_chat_photo_query(PromisedQueryPtr &query);
  Status process_delete_chat_photo_query(PromisedQueryPtr &query);
  Status process_set_chat_title_query(PromisedQueryPtr &query);
  Status process_set_chat_permissions_query(PromisedQueryPtr &query);
  Status process_set_chat_description_query(PromisedQueryPtr &query);
  Status process_pin_chat_message_query(PromisedQueryPtr &query);
  Status process_unpin_chat_message_query(PromisedQueryPtr &query);
  Status process_unpin_all_chat_messages_query(PromisedQueryPtr &query);
  Status process_set_chat_sticker_set_query(PromisedQueryPtr &query);
  Status process_delete_chat_sticker_set_query(PromisedQueryPtr &query);
  Status process_get_chat_member_query(PromisedQueryPtr &query);
  Status process_get_chat_administrators_query(PromisedQueryPtr &query);
  Status process_get_chat_member_count_query(PromisedQueryPtr &query);
  Status process_leave_chat_query(PromisedQueryPtr &query);
  Status process_promote_chat_member_query(PromisedQueryPtr &query);
  Status process_set_chat_administrator_custom_title_query(PromisedQueryPtr &query);
  Status process_ban_chat_member_query(PromisedQueryPtr &query);
  Status process_restrict_chat_member_query(PromisedQueryPtr &query);
  Status process_unban_chat_member_query(PromisedQueryPtr &query);
  Status process_ban_chat_sender_chat_query(PromisedQueryPtr &query);
  Status process_unban_chat_sender_chat_query(PromisedQueryPtr &query);
  Status process_approve_chat_join_request_query(PromisedQueryPtr &query);
  Status process_decline_chat_join_request_query(PromisedQueryPtr &query);
  Status process_get_sticker_set_query(PromisedQueryPtr &query);
  Status process_upload_sticker_file_query(PromisedQueryPtr &query);
  Status process_create_new_sticker_set_query(PromisedQueryPtr &query);
  Status process_add_sticker_to_set_query(PromisedQueryPtr &query);
  Status process_set_sticker_set_thumb_query(PromisedQueryPtr &query);
  Status process_set_sticker_position_in_set_query(PromisedQueryPtr &query);
  Status process_delete_sticker_from_set_query(PromisedQueryPtr &query);
  Status process_set_passport_data_errors_query(PromisedQueryPtr &query);
  Status process_send_custom_request_query(PromisedQueryPtr &query);
  Status process_answer_custom_query_query(PromisedQueryPtr &query);
  Status process_get_updates_query(PromisedQueryPtr &query);
  Status process_set_webhook_query(PromisedQueryPtr &query);
  Status process_get_webhook_info_query(PromisedQueryPtr &query);
  Status process_get_file_query(PromisedQueryPtr &query);

  void webhook_verified(td::string cached_ip_address) final;
  void webhook_success() final;
  void webhook_error(Status status) final;
  void webhook_closed(Status status) final;
  void hangup_shared() final;
  const td::HttpFile *get_webhook_certificate(const Query *query) const;
  int32 get_webhook_max_connections(const Query *query) const;
  static bool get_webhook_fix_ip_address(const Query *query);
  void do_set_webhook(PromisedQueryPtr query, bool was_deleted);
  void save_webhook() const;
  td::string get_webhook_certificate_path() const;

  void do_send_message(object_ptr<td_api::InputMessageContent> input_message_content, PromisedQueryPtr query);

  int64 get_send_message_query_id(PromisedQueryPtr query, bool is_multisend);

  void on_sent_message(object_ptr<td_api::message> &&message, int64 query_id);

  void do_get_file(object_ptr<td_api::file> file, PromisedQueryPtr query);

  bool is_file_being_downloaded(int32 file_id) const;
  void on_file_download(int32 file_id, td::Result<object_ptr<td_api::file>> r_file);

  void fix_reply_markup_bot_user_ids(object_ptr<td_api::ReplyMarkup> &reply_markup) const;
  void fix_inline_query_results_bot_user_ids(td::vector<object_ptr<td_api::InputInlineQueryResult>> &results) const;

  void resolve_bot_usernames(PromisedQueryPtr query, td::Promise<PromisedQueryPtr> on_success);
  void on_resolve_bot_username(const td::string &username, int64 user_id);

  void abort_long_poll(bool from_set_webhook);

  void fail_query_conflict(Slice message, PromisedQueryPtr &&query);

  static void fail_query_with_error(PromisedQueryPtr query, int32 error_code, Slice error_message,
                                    Slice default_message = Slice());

  static void fail_query_with_error(PromisedQueryPtr &&query, object_ptr<td_api::error> error,
                                    Slice default_message = Slice());

  class JsonUpdates;
  void do_get_updates(int32 offset, int32 limit, int32 timeout, PromisedQueryPtr query);

  void long_poll_wakeup(bool force_flag);

  void start_up() final;

  void raw_event(const td::Event::Raw &event) final;

  void loop() final;

  void timeout_expired() final;

  struct UserInfo {
    enum class Type { Regular, Deleted, Bot, Unknown };
    Type type = Type::Unknown;

    td::string first_name;
    td::string last_name;
    td::string username;
    td::string language_code;

    object_ptr<td_api::chatPhoto> photo;
    td::string bio;

    bool have_access = false;
    bool can_join_groups = false;
    bool can_read_all_group_messages = false;
    bool is_inline_bot = false;
    bool has_private_forwards = false;
    bool is_premium = false;
    bool added_to_attachment_menu = false;
  };
  static void add_user(UserInfo *user_info, object_ptr<td_api::user> &&user);
  void set_user_photo(int64 user_id, object_ptr<td_api::chatPhoto> &&photo);
  void set_user_bio(int64 user_id, td::string &&bio);
  void set_user_has_private_forwards(int64 user_id, bool has_private_forwards);
  UserInfo *add_user_info(int64 user_id);
  const UserInfo *get_user_info(int64 user_id) const;

  struct GroupInfo {
    object_ptr<td_api::chatPhoto> photo;
    td::string description;
    td::string invite_link;
    int32 member_count = 0;
    bool left = false;
    bool kicked = false;
    bool is_active = false;
    int64 upgraded_to_supergroup_id = 0;
  };
  static void add_group(GroupInfo *group_info, object_ptr<td_api::basicGroup> &&group);
  void set_group_photo(int64 group_id, object_ptr<td_api::chatPhoto> &&photo);
  void set_group_description(int64 group_id, td::string &&descripton);
  void set_group_invite_link(int64 group_id, td::string &&invite_link);
  GroupInfo *add_group_info(int64 group_id);
  const GroupInfo *get_group_info(int64 group_id) const;

  struct SupergroupInfo {
    td::string username;
    object_ptr<td_api::chatPhoto> photo;
    td::string description;
    td::string invite_link;
    int64 sticker_set_id = 0;
    int32 date = 0;
    int32 slow_mode_delay = 0;
    int64 linked_chat_id = 0;
    object_ptr<td_api::chatLocation> location;
    object_ptr<td_api::ChatMemberStatus> status;
    bool is_supergroup = false;
    bool can_set_sticker_set = false;
    bool has_location = false;
    bool join_to_send_messages = false;
    bool join_by_request = false;
  };
  static void add_supergroup(SupergroupInfo *supergroup_info, object_ptr<td_api::supergroup> &&supergroup);
  void set_supergroup_photo(int64 supergroup_id, object_ptr<td_api::chatPhoto> &&photo);
  void set_supergroup_description(int64 supergroup_id, td::string &&descripton);
  void set_supergroup_invite_link(int64 supergroup_id, td::string &&invite_link);
  void set_supergroup_sticker_set_id(int64 supergroup_id, int64 sticker_set_id);
  void set_supergroup_can_set_sticker_set(int64 supergroup_id, bool can_set_sticker_set);
  void set_supergroup_slow_mode_delay(int64 supergroup_id, int32 slow_mode_delay);
  void set_supergroup_linked_chat_id(int64 supergroup_id, int64 linked_chat_id);
  void set_supergroup_location(int64 supergroup_id, object_ptr<td_api::chatLocation> location);
  SupergroupInfo *add_supergroup_info(int64 supergroup_id);
  const SupergroupInfo *get_supergroup_info(int64 supergroup_id) const;

  struct ChatInfo {
    enum class Type { Private, Group, Supergroup, Unknown };
    Type type = Type::Unknown;
    td::string title;
    int32 message_auto_delete_time = 0;
    bool has_protected_content = false;
    object_ptr<td_api::chatPhotoInfo> photo_info;
    object_ptr<td_api::chatPermissions> permissions;
    union {
      int64 user_id;
      int64 group_id;
      int64 supergroup_id;
    };
  };
  ChatInfo *add_chat(int64 chat_id);
  const ChatInfo *get_chat(int64 chat_id) const;

  enum class ChatType { Private, Group, Supergroup, Channel, Unknown };

  ChatType get_chat_type(int64 chat_id) const;

  td::string get_chat_description(int64 chat_id) const;

  struct MessageInfo {
    int64 id = 0;
    int64 sender_user_id = 0;
    int64 sender_chat_id = 0;
    int64 chat_id = 0;
    int32 date = 0;
    int32 edit_date = 0;
    int64 initial_chat_id = 0;
    int64 initial_sender_user_id = 0;
    int64 initial_sender_chat_id = 0;
    int32 initial_send_date = 0;
    int64 initial_message_id = 0;
    td::string initial_author_signature;
    td::string initial_sender_name;
    td::string author_signature;
    int64 reply_to_message_id = 0;
    int64 media_album_id = 0;
    int64 via_bot_user_id = 0;
    object_ptr<td_api::MessageContent> content;
    object_ptr<td_api::ReplyMarkup> reply_markup;

    bool can_be_saved = false;
    bool is_automatic_forward = false;
    mutable bool is_reply_to_message_deleted = false;
    mutable bool is_content_changed = false;
  };

  static int64 &get_reply_to_message_id(object_ptr<td_api::message> &message);

  void set_message_reply_to_message_id(MessageInfo *message_info, int64 reply_to_message_id);

  static td::CSlice get_callback_data(const object_ptr<td_api::InlineKeyboardButtonType> &type);

  static bool are_equal_inline_keyboard_buttons(const td_api::inlineKeyboardButton *lhs,
                                                const td_api::inlineKeyboardButton *rhs);

  static bool are_equal_inline_keyboards(const td_api::replyMarkupInlineKeyboard *lhs,
                                         const td_api::replyMarkupInlineKeyboard *rhs);

  void set_message_reply_markup(MessageInfo *message_info, object_ptr<td_api::ReplyMarkup> &&reply_markup);

  static int64 get_sticker_set_id(const object_ptr<td_api::MessageContent> &content);

  bool have_sticker_set_name(int64 sticker_set_id) const;

  Slice get_sticker_set_name(int64 sticker_set_id) const;

  int64 choose_added_member_id(const td_api::messageChatAddMembers *message_add_members) const;

  bool need_skip_update_message(int64 chat_id, const object_ptr<td_api::message> &message, bool is_edited) const;

  void json_store_file(td::JsonObjectScope &object, const td_api::file *file, bool with_path = false) const;

  void json_store_thumbnail(td::JsonObjectScope &object, const td_api::thumbnail *thumbnail) const;

  static void json_store_callback_query_payload(td::JsonObjectScope &object,
                                                const td_api::CallbackQueryPayload *payload);

  static void json_store_administrator_rights(td::JsonObjectScope &object,
                                              const td_api::chatAdministratorRights *rights, ChatType chat_type);

  static void json_store_permissions(td::JsonObjectScope &object, const td_api::chatPermissions *permissions);

  void remove_replies_to_message(int64 chat_id, int64 reply_to_message_id, bool only_from_cache);
  void delete_message(int64 chat_id, int64 message_id, bool only_from_cache);

  void add_new_message(object_ptr<td_api::message> &&message, bool is_edited);
  void process_new_message_queue(int64 chat_id);

  struct FullMessageId {
    int64 chat_id;
    int64 message_id;

    FullMessageId() : chat_id(0), message_id(0) {
    }
    FullMessageId(int64 chat_id, int64 message_id) : chat_id(chat_id), message_id(message_id) {
    }

    bool operator==(const FullMessageId &other) const {
      return chat_id == other.chat_id && message_id == other.message_id;
    }
  };

  struct FullMessageIdHash {
    std::size_t operator()(FullMessageId full_message_id) const {
      return std::hash<td::int64>()(full_message_id.chat_id) * 2023654985u +
             std::hash<td::int64>()(full_message_id.message_id);
    }
  };

  FullMessageId add_message(object_ptr<td_api::message> &&message, bool force_update_content = false);
  const MessageInfo *get_message(int64 chat_id, int64 message_id) const;
  MessageInfo *get_message_editable(int64 chat_id, int64 message_id);

  void update_message_content(int64 chat_id, int64 message_id, object_ptr<td_api::MessageContent> &&content);

  void on_update_message_edited(int64 chat_id, int64 message_id, int32 edit_date,
                                object_ptr<td_api::ReplyMarkup> &&reply_markup);

  int32 get_unix_time() const;

  static int64 as_tdlib_message_id(int32 message_id);

  static int32 as_client_message_id(int64 message_id);

  static int64 get_supergroup_chat_id(int64 supergroup_id);

  static int64 get_basic_group_chat_id(int64 basic_group_id);

  void add_update_poll(object_ptr<td_api::updatePoll> &&update);

  void add_update_poll_answer(object_ptr<td_api::updatePollAnswer> &&update);

  void add_new_inline_query(int64 inline_query_id, int64 sender_user_id, object_ptr<td_api::location> location,
                            object_ptr<td_api::ChatType> chat_type, const td::string &query, const td::string &offset);

  void add_new_chosen_inline_result(int64 sender_user_id, object_ptr<td_api::location> location,
                                    const td::string &query, const td::string &result_id,
                                    const td::string &inline_message_id);

  void add_new_callback_query(object_ptr<td_api::updateNewCallbackQuery> &&query);
  void process_new_callback_query_queue(int64 user_id, int state);

  void add_new_inline_callback_query(object_ptr<td_api::updateNewInlineCallbackQuery> &&query);

  void add_new_shipping_query(object_ptr<td_api::updateNewShippingQuery> &&query);

  void add_new_pre_checkout_query(object_ptr<td_api::updateNewPreCheckoutQuery> &&query);

  void add_new_custom_event(object_ptr<td_api::updateNewCustomEvent> &&event);

  void add_new_custom_query(object_ptr<td_api::updateNewCustomQuery> &&query);

  void add_update_chat_member(object_ptr<td_api::updateChatMember> &&update);

  void add_update_chat_join_request(object_ptr<td_api::updateNewChatJoinRequest> &&update);

  // append only before Size
  enum class UpdateType : int32 {
    Message,
    EditedMessage,
    ChannelPost,
    EditedChannelPost,
    InlineQuery,
    ChosenInlineResult,
    CallbackQuery,
    CustomEvent,
    CustomQuery,
    ShippingQuery,
    PreCheckoutQuery,
    Poll,
    PollAnswer,
    MyChatMember,
    ChatMember,
    ChatJoinRequest,
    Size
  };

  static Slice get_update_type_name(UpdateType update_type);

  static td::uint32 get_allowed_update_types(td::MutableSlice allowed_updates, bool is_internal);

  bool update_allowed_update_types(const Query *query);

  template <class T>
  void add_update(UpdateType update_type, const T &update, int32 timeout, int64 webhook_queue_id);

  void add_update_impl(UpdateType update_type, const td::VirtuallyJsonable &update, int32 timeout,
                       int64 webhook_queue_id);

  std::size_t get_pending_update_count() const;

  void update_last_synchronization_error_date();

  static bool is_chat_member(const object_ptr<td_api::ChatMemberStatus> &status);

  static td::string get_chat_member_status(const object_ptr<td_api::ChatMemberStatus> &status);

  static td::string get_passport_element_type(int32 id);

  static object_ptr<td_api::PassportElementType> get_passport_element_type(Slice type);

  bool have_message_access(int64 chat_id) const;

  // by default ChatMember updates are disabled
  static constexpr td::uint32 DEFAULT_ALLOWED_UPDATE_TYPES =
      (1 << static_cast<int32>(UpdateType::Size)) - 1 - (1 << static_cast<int32>(UpdateType::ChatMember));

  object_ptr<td_api::AuthorizationState> authorization_state_;
  bool was_authorized_ = false;
  bool closing_ = false;
  bool logging_out_ = false;
  bool is_api_id_invalid_ = false;
  bool need_close_ = false;
  bool clear_tqueue_ = false;

  td::ActorShared<> parent_;
  td::string bot_token_;
  td::string bot_token_with_dc_;
  td::string bot_token_id_;
  bool is_test_dc_;
  int64 tqueue_id_;
  double start_time_ = 0;

  int64 my_id_ = -1;
  int32 authorization_date_ = -1;

  int64 group_anonymous_bot_user_id_ = 0;
  int64 channel_bot_user_id_ = 0;
  int64 service_notifications_user_id_ = 0;

  static td::FlatHashMap<td::string, Status (Client::*)(PromisedQueryPtr &query)> methods_;

  td::FlatHashMap<FullMessageId, td::unique_ptr<MessageInfo>, FullMessageIdHash> messages_;  // message cache
  td::FlatHashMap<int64, td::unique_ptr<UserInfo>> users_;                                   // user info cache
  td::FlatHashMap<int64, td::unique_ptr<GroupInfo>> groups_;                                 // group info cache
  td::FlatHashMap<int64, td::unique_ptr<SupergroupInfo>> supergroups_;                       // supergroup info cache
  td::FlatHashMap<int64, td::unique_ptr<ChatInfo>> chats_;                                   // chat info cache

  td::FlatHashMap<FullMessageId, td::FlatHashSet<int64>, FullMessageIdHash>
      reply_message_ids_;  // message -> replies to it
  td::FlatHashMap<FullMessageId, td::FlatHashSet<int64>, FullMessageIdHash>
      yet_unsent_reply_message_ids_;  // message -> replies to it

  td::FlatHashMap<int32, td::vector<PromisedQueryPtr>> file_download_listeners_;
  td::FlatHashSet<int32> download_started_file_ids_;

  struct YetUnsentMessage {
    int64 reply_to_message_id = 0;
    bool is_reply_to_message_deleted = false;
    int64 send_message_query_id = 0;
  };
  td::FlatHashMap<FullMessageId, YetUnsentMessage, FullMessageIdHash> yet_unsent_messages_;

  td::FlatHashMap<int64, int32> yet_unsent_message_count_;  // chat_id -> count

  struct PendingSendMessageQuery {
    PromisedQueryPtr query;
    bool is_multisend = false;
    int32 total_message_count = 0;
    int32 awaited_message_count = 0;
    td::vector<td::string> messages;
    object_ptr<td_api::error> error;
  };
  td::FlatHashMap<int64, td::unique_ptr<PendingSendMessageQuery>>
      pending_send_message_queries_;  // query_id -> PendingSendMessageQuery
  int64 current_send_message_query_id_ = 1;

  struct NewMessage {
    object_ptr<td_api::message> message;
    bool is_edited = false;

    NewMessage(object_ptr<td_api::message> &&message, bool is_edited)
        : message(std::move(message)), is_edited(is_edited) {
    }
  };
  struct NewMessageQueue {
    std::queue<NewMessage> queue_;
    bool has_active_request_ = false;
  };
  td::FlatHashMap<int64, NewMessageQueue> new_message_queues_;  // chat_id -> queue

  struct NewCallbackQueryQueue {
    std::queue<object_ptr<td_api::updateNewCallbackQuery>> queue_;
    bool has_active_request_ = false;
  };
  td::FlatHashMap<int64, NewCallbackQueryQueue> new_callback_query_queues_;  // sender_user_id -> queue

  td::FlatHashMap<int64, td::string> sticker_set_names_;

  int64 cur_temp_bot_user_id_ = 1;
  td::FlatHashMap<td::string, int64> bot_user_ids_;
  td::FlatHashSet<td::string> unresolved_bot_usernames_;
  td::FlatHashMap<int64, int64> temp_to_real_bot_user_id_;
  td::FlatHashMap<td::string, td::vector<int64>> awaiting_bot_resolve_queries_;

  struct PendingBotResolveQuery {
    std::size_t pending_resolve_count = 0;
    PromisedQueryPtr query;
    td::Promise<PromisedQueryPtr> on_success;
  };
  td::FlatHashMap<int64, PendingBotResolveQuery> pending_bot_resolve_queries_;
  int64 current_bot_resolve_query_id_ = 1;

  td::string dir_;
  td::ActorOwn<td::ClientActor> td_client_;
  td::ActorContext context_;
  std::queue<PromisedQueryPtr> cmd_queue_;
  td::vector<object_ptr<td_api::Object>> pending_updates_;
  td::Container<td::unique_ptr<TdQueryCallback>> handlers_;

  static constexpr int32 LONG_POLL_MAX_TIMEOUT = 50;
  static constexpr double LONG_POLL_MAX_DELAY = 0.002;
  static constexpr double LONG_POLL_WAIT_AFTER = 0.001;
  int32 long_poll_limit_ = 0;
  int32 long_poll_offset_ = 0;
  bool long_poll_was_wakeup_ = false;
  double long_poll_hard_timeout_ = 0;
  td::Slot long_poll_slot_;
  PromisedQueryPtr long_poll_query_;

  static constexpr int32 BOT_UPDATES_WARNING_DELAY = 30;
  double next_bot_updates_warning_time_ = 0;
  bool was_bot_updates_warning_ = false;

  td::uint32 allowed_update_types_ = DEFAULT_ALLOWED_UPDATE_TYPES;

  bool has_webhook_certificate_ = false;
  enum class WebhookQueryType { Cancel, Verify };
  WebhookQueryType webhook_query_type_ = WebhookQueryType::Cancel;
  td::ActorOwn<WebhookActor> webhook_id_;
  PromisedQueryPtr webhook_set_query_;
  td::string webhook_url_;
  double webhook_set_time_ = 0;
  int32 webhook_max_connections_ = 0;
  td::string webhook_ip_address_;
  bool webhook_fix_ip_address_ = false;
  td::string webhook_secret_token_;
  int32 last_webhook_error_date_ = 0;
  Status last_webhook_error_;
  double next_allowed_set_webhook_time_ = 0;
  double next_set_webhook_logging_time_ = 0;
  double next_webhook_is_not_modified_warning_time_ = 0;
  std::size_t last_pending_update_count_ = MIN_PENDING_UPDATES_WARNING;

  double local_unix_time_difference_ = 0;  // Unix time - now()

  double disconnection_time_ = 0;         // the time when Connection state changed from "Ready", or 0 if it is "Ready"
  double last_update_creation_time_ = 0;  // the time when the last update was added
  int32 last_synchronization_error_date_ = 0;  // the date of the last connection error

  int32 previous_get_updates_offset_ = -1;
  double previous_get_updates_start_time_ = 0;
  double previous_get_updates_finish_time_ = 0;
  double next_get_updates_conflict_time_ = 0;

  td::uint64 webhook_generation_ = 1;

  std::shared_ptr<const ClientParameters> parameters_;

  td::ActorId<BotStatActor> stat_actor_;
};

}  // namespace telegram_bot_api
