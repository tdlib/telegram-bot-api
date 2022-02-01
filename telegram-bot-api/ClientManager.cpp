//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2021, Luckydonald (tdlight-telegram-bot-api+code@luckydonald.de) 2020
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/ClientManager.h"

#include "telegram-bot-api/Client.h"
#include "telegram-bot-api/ClientParameters.h"
#include "telegram-bot-api/WebhookActor.h"
#include "telegram-bot-api/StatsJson.h"

#include "td/telegram/ClientActor.h"
#include "td/telegram/td_api.h"

#include "td/db/binlog/Binlog.h"
#include "td/db/binlog/ConcurrentBinlog.h"
#include "td/db/BinlogKeyValue.h"
#include "td/db/DbKey.h"
#include "td/db/TQueue.h"

#include "td/net/HttpFile.h"

#include "td/actor/MultiPromise.h"

#include "td/utils/common.h"
#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/Parser.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/Stat.h"
#include "td/utils/Slice.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/StackAllocator.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/Time.h"
#include "td/utils/Random.h"
#include "td/utils/base64.h"

#include <map>
#include <tuple>

namespace telegram_bot_api {

void ClientManager::close(td::Promise<td::Unit> &&promise) {
  close_promises_.push_back(std::move(promise));
  if (close_flag_) {
    return;
  }

  close_flag_ = true;
  auto ids = clients_.ids();
  for (auto id : ids) {
    auto *client_info = clients_.get(id);
    CHECK(client_info);
    send_closure(client_info->client_, &Client::close);
  }
  if (ids.empty()) {
    close_db();
  }
}

void ClientManager::send(PromisedQueryPtr query) {
  if (close_flag_) {
    // automatically send 429
    return;
  }
  if (!parameters_->allow_users_ && query->is_user()) {
    return fail_query(405, "Method Not Allowed: Users are not allowed to use the api", std::move(query));
  }
  td::string token = query->token().str();
  if (token[0] == '0' || token.size() > 80u || token.find('/') != td::string::npos ||
      token.find(':') == td::string::npos) {
    return fail_query(401, "Unauthorized: invalid token specified", std::move(query));
  }
  auto r_user_id = td::to_integer_safe<td::int64>(query->token().substr(0, token.find(':')));
  if (r_user_id.is_error() || r_user_id.ok() < 0 || !token_range_(r_user_id.ok())) {
    return fail_query(421, "Misdirected Request: unallowed token specified", std::move(query));
  }

  if (query->is_test_dc()) {
    token += "/test";
  }

  auto bot_token_with_dc = PSTRING() << query->token() << (query->is_test_dc() ? ":T" : "");
  if (parameters_->shared_data_->user_db_->isset(bot_token_with_dc) != query->is_user()) {
    return fail_query(400, "Bad Request: Please use the correct api endpoint for bots or users", std::move(query));
  }

  auto id_it = token_to_id_.find(token);
  if (id_it == token_to_id_.end()) {
    if (!check_flood_limits(query)) {
      return;
    }

    auto tqueue_id = get_tqueue_id(r_user_id.ok(), query->is_test_dc());
    if (active_client_count_.find(tqueue_id) != active_client_count_.end()) {
      // return query->set_retry_after_error(1);
    }

    auto id =
        clients_.create(ClientInfo{BotStatActor(stat_.actor_id(&stat_)), token, tqueue_id, td::ActorOwn<Client>()});
    auto *client_info = clients_.get(id);
    client_info->client_ = td::create_actor<Client>(PSLICE() << "Client/" << token, actor_shared(this, id),
                                                    query->token().str(),  query->is_user(), query->is_test_dc(),
                                                    tqueue_id, parameters_, client_info->stat_.actor_id(&client_info->stat_));

    auto method = query->method();
    if (method != "deletewebhook" && method != "setwebhook") {
      auto webhook_info = parameters_->shared_data_->webhook_db_->get(bot_token_with_dc);
      if (!webhook_info.empty()) {
        send_closure(client_info->client_, &Client::send,
                     get_webhook_restore_query(bot_token_with_dc, query->is_user(), webhook_info, parameters_->shared_data_));
      }
    }

    std::tie(id_it, std::ignore) = token_to_id_.emplace(token, id);
  }
  send_closure(clients_.get(id_it->second)->client_, &Client::send,
               std::move(query));  // will send 429 if the client is already closed
}

void ClientManager::user_login(PromisedQueryPtr query) {
  if (!check_flood_limits(query, true)) {
    return;
  }
  if (!parameters_->allow_users_) {
    return fail_query(405, "Method Not Allowed: Users are not allowed to use the api", std::move(query));
  }
  td::MutableSlice r_phone_number = query->arg("phone_number");
  if (r_phone_number.size() < 5 || r_phone_number.size() > 15) {
    return fail_query(401, "Unauthorized: invalid phone number specified", std::move(query));
  }
  td::int64 phone_number = 0;
  for (char const &c: r_phone_number) {
    if (isdigit(c)) {
      phone_number = phone_number * 10 + (c - 48);
    }
  }
  td::UInt256 token_data;
  td::Random::secure_bytes(token_data.raw, sizeof(token_data));
  td::string user_token = td::to_string(phone_number) + ":" + td::base64url_encode(token_data.as_slice());
  auto user_token_with_dc = PSTRING() << user_token << (query->is_test_dc() ? ":T" : "");

  long token_hash = std::hash<td::string>{}(user_token);

  auto tqueue_id = get_tqueue_id(token_hash, query->is_test_dc());
  if (active_client_count_.find(tqueue_id) != active_client_count_.end()) {
    // return query->set_retry_after_error(1);
  }

  auto id = clients_.create(ClientInfo{BotStatActor(stat_.actor_id(&stat_)), user_token, tqueue_id, td::ActorOwn<Client>()});
  auto *client_info = clients_.get(id);
  auto stat_actor = client_info->stat_.actor_id(&client_info->stat_);
  auto client_id = td::create_actor<Client>(PSLICE() << "Client/" << user_token, actor_shared(this, id), user_token,
                                            true, query->is_test_dc(), get_tqueue_id(token_hash, query->is_test_dc()),
                                            parameters_, std::move(stat_actor));

  clients_.get(id)->client_ = std::move(client_id);
  auto id_it = token_to_id_.end();
  std::tie(id_it, std::ignore) = token_to_id_.emplace(user_token, id);
  send_closure(client_info->client_, &Client::send, std::move(query));  // will send 429 if the client is already closed

  parameters_->shared_data_->user_db_->set(user_token_with_dc, "1");
  // answer_query(td::VirtuallyJsonableString(user_token), std::move(query));
}

bool ClientManager::check_flood_limits(PromisedQueryPtr &query, bool is_user_login) {
  td::string ip_address;
  if (query->peer_address().is_valid() && !query->peer_address().is_reserved()) {  // external connection
    ip_address = query->peer_address().get_ip_str().str();
  } else {
    // invalid peer address or connection from the local network
    ip_address = query->get_header("x-real-ip").str();
  }
  if (!ip_address.empty()) {
    td::IPAddress tmp;
    tmp.init_host_port(ip_address, 0).ignore();
    tmp.clear_ipv6_interface();
    if (tmp.is_valid()) {
      ip_address = tmp.get_ip_str().str();
    }
  }
  LOG(DEBUG) << "Receive incoming query for new bot " << query->token() << " from " << query->peer_address();
  if (!ip_address.empty()) {
    LOG(DEBUG) << "Check Client creation flood control for IP address " << ip_address;
    if (is_user_login) {
      ip_address += "/user";
    }
    auto res = flood_controls_.emplace(std::move(ip_address), td::FloodControlFast());
    auto &flood_control = res.first->second;
    if (res.second) {
      if (is_user_login) {
        flood_control.add_limit(60, 5);         // 5 in a minute
        flood_control.add_limit(60 * 60, 15);   // 15 in an hour
      } else {
        flood_control.add_limit(60, 20);        // 20 in a minute
        flood_control.add_limit(60 * 60, 600);  // 600 in an hour
      }
    }
    auto now = static_cast<td::uint32>(td::Time::now());
    td::uint32 wakeup_at = flood_control.get_wakeup_at();
    if (wakeup_at > now) {
      LOG(INFO) << "Failed to create Client from IP address " << ip_address;
      query->set_retry_after_error(static_cast<int>(wakeup_at - now) + 1);
      return false;
    }
    flood_control.add_event(static_cast<td::int32>(now));
  }
  return true;
}

void ClientManager::get_stats(td::PromiseActor<td::BufferSlice> promise,
                              td::vector<std::pair<td::string, td::string>> args,
                              bool as_json) {
  if (close_flag_) {
    promise.set_value(td::BufferSlice("Closing"));
    return;
  }
  size_t buf_size = 1 << 18;
  auto buf = td::StackAllocator::alloc(buf_size);
  td::JsonBuilder jb(td::StringBuilder(buf.as_slice(), true), -1);
  td::StringBuilder sb = std::move(jb.string_builder());
  auto jb_root = jb.enter_object();
  if (!as_json) {
    jb_root.leave();
    sb.clear();
  } else {
    jb_root("⚠️ WARNING", "The json representation is still a work in progress and will be changed later!");
  }

  td::Slice id_filter;
  int new_verbosity_level = -1;
  td::string tag;
  for (auto &arg : args) {
    if (arg.first == "id") {
      id_filter = arg.second;
    }
    if (arg.first == "v") {
      auto r_new_verbosity_level = td::to_integer_safe<int>(arg.second);
      if (r_new_verbosity_level.is_ok()) {
        new_verbosity_level = r_new_verbosity_level.ok();
      }
    }
    if (arg.first == "tag") {
      tag = arg.second;
    }
  }
  if (new_verbosity_level > 0) {
    if (tag.empty()) {
      parameters_->shared_data_->next_verbosity_level_ = new_verbosity_level;
    } else {
      td::ClientActor::execute(td::td_api::make_object<td::td_api::setLogTagVerbosityLevel>(tag, new_verbosity_level));
    }
  }

  auto now = td::Time::now();
  td::int32 active_bot_count = 0;
  std::multimap<td::int64, td::uint64> top_bot_ids;
  for (auto id : clients_.ids()) {
    auto *client_info = clients_.get(id);
    CHECK(client_info);

    if (client_info->stat_.is_active(now)) {
      active_bot_count++;
    }

    if (!td::begins_with(client_info->token_, id_filter)) {
      continue;
    }

    auto stats = client_info->stat_.as_vector(now);
    double score = 0.0;
    for (auto &stat : stats) {
      if (stat.key_ == "update_count" || stat.key_ == "request_count") {
        score -= td::to_double(stat.value_);
      }
    }
    top_bot_ids.emplace(static_cast<td::int64>(score * 1e9), id);
  }

  if(!as_json) {
    sb << stat_.get_description() << '\n';
  }
  if (id_filter.empty()) {
    if(as_json) {
      jb_root("uptime", td::JsonFloat(now - parameters_->start_time_));
    } else {
      sb << "uptime\t" << now - parameters_->start_time_ << '\n';
    }
    if(as_json) {
      jb_root("bot_count", td::JsonLong(clients_.size()));
    } else {
      sb << "bot_count\t" << clients_.size() << '\n';
    }
    if(as_json) {
      jb_root("active_bot_count", td::JsonInt(active_bot_count));
    } else {
      sb << "active_bot_count\t" << active_bot_count << '\n';
    }
    auto r_mem_stat = td::mem_stat();
    if (r_mem_stat.is_ok()) {
      auto mem_stat = r_mem_stat.move_as_ok();
      if(as_json) {
        jb_root("memory", JsonStatsMem(mem_stat));
      } else {
        sb << "rss\t" << td::format::as_size(mem_stat.resident_size_) << '\n';
        sb << "vm\t" << td::format::as_size(mem_stat.virtual_size_) << '\n';
        sb << "rss_peak\t" << td::format::as_size(mem_stat.resident_size_peak_) << '\n';
        sb << "vm_peak\t" << td::format::as_size(mem_stat.virtual_size_peak_) << '\n';
      }
    } else {
      if(as_json) {
        jb_root("memory", td::JsonNull());
      } else {
        LOG(INFO) << "Failed to get memory statistics: " << r_mem_stat.error();
      }
    }

    ServerCpuStat::update(td::Time::now());
    if(as_json) {
      auto cpu_stats = ServerCpuStat::instance().as_json_ready_vector(td::Time::now());
      jb_root("cpu", JsonStatsCpu(std::move(cpu_stats)));
    } else {
      auto cpu_stats = ServerCpuStat::instance().as_vector(td::Time::now());
      for (auto &stat : cpu_stats) {
        sb << stat.key_ << "\t" << stat.value_ << '\n';
      }
    }

    if(as_json) {
      jb_root("buffer_memory", JsonStatsSize(td::BufferAllocator::get_buffer_mem()));
      jb_root("active_webhook_connections", td::JsonLong(WebhookActor::get_total_connections_count()));
      jb_root("active_requests", td::JsonLong(parameters_->shared_data_->query_count_.load()));
      jb_root("active_network_queries", td::JsonLong(td::get_pending_network_query_count(*parameters_->net_query_stats_)));
    } else {
      sb << "buffer_memory\t" << td::format::as_size(td::BufferAllocator::get_buffer_mem()) << '\n';
      sb << "active_webhook_connections\t" << WebhookActor::get_total_connections_count() << '\n';
      sb << "active_requests\t" << parameters_->shared_data_->query_count_.load() << '\n';
      sb << "active_network_queries\t" << td::get_pending_network_query_count(*parameters_->net_query_stats_) << '\n';
    }
    if(as_json) {
    } else {
      auto stats = stat_.as_vector(now);
      for (auto &stat : stats) {
        sb << stat.key_ << "\t" << stat.value_ << '\n';
      }
    }
  }

  if(as_json) {
    td::vector<JsonStatsBotAdvanced> bots;
    for (std::pair<td::int64, td::uint64>  top_bot_id : top_bot_ids) {
      auto client_info = clients_.get(top_bot_id.second);
      CHECK(client_info);
      ServerBotInfo bot_info = client_info->client_->get_actor_unsafe()->get_bot_info();
      auto stats = client_info->stat_.as_json_ready_vector(now);
      JsonStatsBotAdvanced bot(
          std::move(top_bot_id), std::move(bot_info), std::move(stats), parameters_->stats_hide_sensible_data_, now
        );
      bots.push_back(bot);
    }
    auto bot_count = bots.size();
    jb_root("bots", JsonStatsBots(std::move(bots), bot_count > 100));
  } else {
    for (auto top_bot_id : top_bot_ids) {
      auto *client_info = clients_.get(top_bot_id.second);
      CHECK(client_info);
      auto bot_info = client_info->client_->get_actor_unsafe()->get_bot_info();

      sb << '\n';
      sb << "id\t" << bot_info.id_ << '\n';
      sb << "uptime\t" << now - bot_info.start_time_ << '\n';
      if (!parameters_->stats_hide_sensible_data_) {
        sb << "token\t" << bot_info.token_ << '\n';
      }
      sb << "username\t" << bot_info.username_ << '\n';
      if (!parameters_->stats_hide_sensible_data_) {
        sb << "webhook\t" << bot_info.webhook_ << '\n';
      } else if (bot_info.webhook_.empty()) {
        sb << "webhook disabled" << '\n';
      } else {
        sb << "webhook enabled" << '\n';
      }
      sb << "has_custom_certificate\t" << bot_info.has_webhook_certificate_ << '\n';
      sb << "head_update_id\t" << bot_info.head_update_id_ << '\n';
      sb << "tail_update_id\t" << bot_info.tail_update_id_ << '\n';
      sb << "pending_update_count\t" << bot_info.pending_update_count_ << '\n';
      sb << "webhook_max_connections\t" << bot_info.webhook_max_connections_ << '\n';

      auto stats = client_info->stat_.as_vector(now);
      for (auto &stat : stats) {
        if (stat.key_ == "update_count" || stat.key_ == "request_count") {
          sb << stat.key_ << "/sec\t" << stat.value_ << '\n';
        }
      }
      if (sb.is_error()) {
        break;
      }
    }
  }
  if(as_json) {
    jb_root.leave();
  }

  // ignore sb overflow
  promise.set_value(td::BufferSlice((as_json ? jb.string_builder() : sb).as_cslice()));
}

td::int64 ClientManager::get_tqueue_id(td::int64 user_id, bool is_test_dc) {
  return user_id + (static_cast<td::int64>(is_test_dc) << 54);
}

void ClientManager::start_up() {
  //NB: the same scheduler as for database in Td
  auto current_scheduler_id = td::Scheduler::instance()->sched_id();
  auto scheduler_count = td::Scheduler::instance()->sched_count();
  auto scheduler_id = td::min(current_scheduler_id + 1, scheduler_count - 1);

  // init tqueue
  {
    auto load_start_time = td::Time::now();
    auto tqueue_binlog = td::make_unique<td::TQueueBinlog<td::Binlog>>();
    auto binlog = td::make_unique<td::Binlog>();
    auto tqueue = td::TQueue::create();
    td::vector<td::uint64> failed_to_replay_log_event_ids;
    td::int64 loaded_event_count = 0;
    binlog
        ->init(parameters_->working_directory_ + "tqueue.binlog",
               [&](const td::BinlogEvent &event) {
                 if (tqueue_binlog->replay(event, *tqueue).is_error()) {
                   failed_to_replay_log_event_ids.push_back(event.id_);
                 } else {
                   loaded_event_count++;
                 }
               })
        .ensure();
    tqueue_binlog.reset();

    if (!failed_to_replay_log_event_ids.empty()) {
      LOG(ERROR) << "Failed to replay " << failed_to_replay_log_event_ids.size() << " TQueue events";
      for (auto &log_event_id : failed_to_replay_log_event_ids) {
        binlog->erase(log_event_id);
      }
    }

    auto concurrent_binlog = std::make_shared<td::ConcurrentBinlog>(std::move(binlog), scheduler_id);
    auto concurrent_tqueue_binlog = td::make_unique<td::TQueueBinlog<td::BinlogInterface>>();
    concurrent_tqueue_binlog->set_binlog(std::move(concurrent_binlog));
    tqueue->set_callback(std::move(concurrent_tqueue_binlog));

    parameters_->shared_data_->tqueue_ = std::move(tqueue);

    LOG(WARNING) << "Loaded " << loaded_event_count << " TQueue events in " << (td::Time::now() - load_start_time)
                 << " seconds";
  }

  // init webhook_db and user_db
  auto concurrent_webhook_db = td::make_unique<td::BinlogKeyValue<td::ConcurrentBinlog>>();
  auto status = concurrent_webhook_db->init(parameters_->working_directory_ + "webhooks_db.binlog", td::DbKey::empty(),
                                            scheduler_id);
  LOG_IF(FATAL, status.is_error()) << "Can't open webhooks_db.binlog " << status.error();
  parameters_->shared_data_->webhook_db_ = std::move(concurrent_webhook_db);

  auto concurrent_user_db = td::make_unique<td::BinlogKeyValue<td::ConcurrentBinlog>>();
  status = concurrent_user_db->init(parameters_->working_directory_ + "user_db.binlog", td::DbKey::empty(), scheduler_id);
  LOG_IF(FATAL, status.is_error()) << "Can't open user_db.binlog " << status.error();
  parameters_->shared_data_->user_db_ = std::move(concurrent_user_db);

  auto &webhook_db = *parameters_->shared_data_->webhook_db_;
  auto &user_db = *parameters_->shared_data_->user_db_;
  for (auto key_value : webhook_db.get_all()) {
    if (!token_range_(td::to_integer<td::uint64>(key_value.first))) {
      LOG(WARNING) << "DROP WEBHOOK: " << key_value.first << " ---> " << key_value.second;
      webhook_db.erase(key_value.first);
      continue;
    }

    auto query = get_webhook_restore_query(key_value.first, user_db.isset(key_value.first), key_value.second, parameters_->shared_data_);
    send_closure_later(actor_id(this), &ClientManager::send, std::move(query));
  }

}

PromisedQueryPtr ClientManager::get_webhook_restore_query(td::Slice token, bool is_user, td::Slice webhook_info,
                                                          std::shared_ptr<SharedData> shared_data) {
  // create Query with empty promise
  td::vector<td::BufferSlice> containers;
  auto add_string = [&containers](td::Slice str) {
    containers.emplace_back(str);
    return containers.back().as_slice();
  };

  token = add_string(token);

  LOG(WARNING) << "WEBHOOK: " << token << " ---> " << webhook_info;

  bool is_test_dc = false;
  if (td::ends_with(token, ":T")) {
    token.remove_suffix(2);
    is_test_dc = true;
  }

  td::ConstParser parser{webhook_info};
  td::vector<std::pair<td::MutableSlice, td::MutableSlice>> args;
  if (parser.try_skip("cert/")) {
    args.emplace_back(add_string("certificate"), add_string("previous"));
  }

  if (parser.try_skip("#maxc")) {
    args.emplace_back(add_string("max_connections"), add_string(parser.read_till('/')));
    parser.skip('/');
  }

  if (parser.try_skip("#ip")) {
    args.emplace_back(add_string("ip_address"), add_string(parser.read_till('/')));
    parser.skip('/');
  }

  if (parser.try_skip("#fix_ip")) {
    args.emplace_back(add_string("fix_ip_address"), add_string("1"));
    parser.skip('/');
  }

  if (parser.try_skip("#allow")) {
    args.emplace_back(add_string("allowed_updates"), add_string(parser.read_till('/')));
    parser.skip('/');
  }

  args.emplace_back(add_string("url"), add_string(parser.read_all()));

  const auto method = add_string("setwebhook");
  auto query = std::make_unique<Query>(std::move(containers), token, is_user, is_test_dc, method, std::move(args),
                                       td::vector<std::pair<td::MutableSlice, td::MutableSlice>>(),
                                       td::vector<td::HttpFile>(), std::move(shared_data), td::IPAddress(), true);
  return PromisedQueryPtr(query.release(), PromiseDeleter(td::PromiseActor<td::unique_ptr<Query>>()));
}

void ClientManager::raw_event(const td::Event::Raw &event) {
  auto id = get_link_token();
  auto *info = clients_.get(id);
  CHECK(info != nullptr);
  auto &value = active_client_count_[info->tqueue_id_];
  if (event.ptr != nullptr) {
    value++;
  } else {
    CHECK(value > 0);
    if (--value == 0) {
      active_client_count_.erase(info->tqueue_id_);
    }
  }
}

void ClientManager::hangup_shared() {
  auto id = get_link_token();
  auto *info = clients_.get(id);
  CHECK(info != nullptr);
  info->client_.release();
  token_to_id_.erase(info->token_);
  clients_.erase(id);

  if (close_flag_ && clients_.empty()) {
    CHECK(active_client_count_.empty());
    close_db();
  }
}

void ClientManager::close_db() {
  LOG(WARNING) << "Closing databases";
  td::MultiPromiseActorSafe mpas("close binlogs");
  mpas.add_promise(td::PromiseCreator::lambda(
      [actor_id = actor_id(this)](td::Unit) { send_closure(actor_id, &ClientManager::finish_close); }));
  mpas.set_ignore_errors(true);

  auto lock = mpas.get_promise();
  parameters_->shared_data_->tqueue_->close(mpas.get_promise());
  parameters_->shared_data_->webhook_db_->close(mpas.get_promise());
  parameters_->shared_data_->user_db_->close(mpas.get_promise());
  lock.set_value(td::Unit());
}

void ClientManager::finish_close() {
  LOG(WARNING) << "Stop ClientManager";
  auto promises = std::move(close_promises_);
  for (auto &promise : promises) {
    promise.set_value(td::Unit());
  }
  stop();
}

}  // namespace telegram_bot_api
