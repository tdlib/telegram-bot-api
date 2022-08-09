//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2021
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/WebhookActor.h"

#include "telegram-bot-api/ClientParameters.h"

#include "td/net/GetHostByNameActor.h"
#include "td/net/HttpHeaderCreator.h"
#include "td/net/HttpProxy.h"
#include "td/net/SslStream.h"
#include "td/net/TransparentProxy.h"

#include "td/actor/actor.h"
#include "td/actor/PromiseFuture.h"

#include "td/utils/base64.h"
#include "td/utils/buffer.h"
#include "td/utils/common.h"
#include "td/utils/format.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/SocketFd.h"
#include "td/utils/Random.h"
#include "td/utils/ScopeGuard.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/Span.h"
#include "td/utils/Time.h"

#include <limits>

namespace telegram_bot_api {

static int VERBOSITY_NAME(webhook) = VERBOSITY_NAME(DEBUG);

std::atomic<td::uint64> WebhookActor::total_connections_count_{0};

WebhookActor::WebhookActor(td::ActorShared<Callback> callback, td::int64 tqueue_id, td::HttpUrl url,
                           td::string cert_path, td::int32 max_connections, bool from_db_flag,
                           td::string cached_ip_address, bool fix_ip_address, td::string secret_token,
                           std::shared_ptr<const ClientParameters> parameters)
    : callback_(std::move(callback))
    , tqueue_id_(tqueue_id)
    , url_(std::move(url))
    , cert_path_(std::move(cert_path))
    , parameters_(std::move(parameters))
    , fix_ip_address_(fix_ip_address)
    , from_db_flag_(from_db_flag)
    , max_connections_(max_connections)
    , secret_token_(std::move(secret_token)) {
  CHECK(max_connections_ > 0);

  if (!cached_ip_address.empty()) {
    auto r_ip_address = td::IPAddress::get_ip_address(cached_ip_address);
    if (r_ip_address.is_ok()) {
      ip_address_ = r_ip_address.move_as_ok();
      ip_address_.set_port(url_.port_);
    }
  }

  auto r_ascii_host = td::idn_to_ascii(url_.host_);
  if (r_ascii_host.is_ok()) {
    url_.host_ = r_ascii_host.move_as_ok();
  }

  LOG(INFO) << "Set webhook for " << tqueue_id << " with certificate = \"" << cert_path_
            << "\", protocol = " << (url_.protocol_ == td::HttpUrl::Protocol::Http ? "http" : "https")
            << ", host = " << url_.host_ << ", port = " << url_.port_ << ", query = " << url_.query_
            << ", max_connections = " << max_connections_;
}

void WebhookActor::relax_wakeup_at(double wakeup_at, const char *source) {
  if (wakeup_at_ == 0 || wakeup_at < wakeup_at_) {
    VLOG(webhook) << "Wake up in " << wakeup_at - td::Time::now() << " from " << source;
    wakeup_at_ = wakeup_at;
  }
}

void WebhookActor::resolve_ip_address() {
  if (fix_ip_address_) {
    return;
  }
  if (td::Time::now() < next_ip_address_resolve_time_) {
    relax_wakeup_at(next_ip_address_resolve_time_, "resolve_ip_address");
    return;
  }

  bool future_created = false;
  if (future_ip_address_.empty()) {
    td::PromiseActor<td::IPAddress> promise;
    init_promise_future(&promise, &future_ip_address_);
    future_created = true;
    send_closure(parameters_->get_host_by_name_actor_id_, &td::GetHostByNameActor::run, url_.host_, url_.port_, false,
                 td::PromiseCreator::from_promise_actor(std::move(promise)));
  }

  if (future_ip_address_.is_ready()) {
    next_ip_address_resolve_time_ =
        td::Time::now() + IP_ADDRESS_CACHE_TIME + td::Random::fast(0, IP_ADDRESS_CACHE_TIME / 10);
    relax_wakeup_at(next_ip_address_resolve_time_, "resolve_ip_address");

    auto r_ip_address = future_ip_address_.move_as_result();
    if (r_ip_address.is_error()) {
      CHECK(!(r_ip_address.error() == td::Status::Error<td::FutureActor<td::IPAddress>::HANGUP_ERROR_CODE>()));
      return on_error(r_ip_address.move_as_error());
    }
    auto new_ip_address = r_ip_address.move_as_ok();
    if (!check_ip_address(new_ip_address)) {
      return on_error(td::Status::Error(PSLICE() << "IP address " << new_ip_address.get_ip_str() << " is reserved"));
    }
    if (!(ip_address_ == new_ip_address)) {
      VLOG(webhook) << "IP address has changed: " << ip_address_ << " --> " << new_ip_address;
      ip_address_ = new_ip_address;
      ip_generation_++;
      if (was_checked_) {
        on_webhook_verified();
      }
    }
    VLOG(webhook) << "IP address was verified";
  } else {
    if (future_created) {
      future_ip_address_.set_event(td::EventCreator::yield(actor_id()));
    }
  }
}

td::Status WebhookActor::create_connection() {
  if (!ip_address_.is_valid()) {
    VLOG(webhook) << "Can't create connection: IP address is not ready";
    return td::Status::Error("IP address is not ready");
  }
  if (parameters_->webhook_proxy_ip_address_.is_valid()) {
    auto r_proxy_socket_fd = td::SocketFd::open(parameters_->webhook_proxy_ip_address_);
    if (r_proxy_socket_fd.is_error()) {
      td::Slice error_message = "Can't connect to the webhook proxy";
      auto error = td::Status::Error(PSLICE() << error_message << ": " << r_proxy_socket_fd.error());
      VLOG(webhook) << error;
      on_webhook_error(error_message);
      on_error(td::Status::Error(error_message));
      return error;
    }
    if (!was_checked_) {
      TRY_STATUS(create_ssl_stream());  // check certificate

      // verify webhook even we can't establish connection to the webhook
      was_checked_ = true;
      on_webhook_verified();
    }

    VLOG(webhook) << "Create connection through proxy " << parameters_->webhook_proxy_ip_address_;
    class Callback final : public td::TransparentProxy::Callback {
     public:
      Callback(td::ActorId<WebhookActor> actor, td::int64 id) : actor_(actor), id_(id) {
      }
      void set_result(td::Result<td::BufferedFd<td::SocketFd>> result) final {
        send_closure(std::move(actor_), &WebhookActor::on_socket_ready_async, std::move(result), id_);
        CHECK(actor_.empty());
      }
      Callback(const Callback &) = delete;
      Callback &operator=(const Callback &) = delete;
      Callback(Callback &&) = delete;
      Callback &operator=(Callback &&) = delete;
      ~Callback() {
        if (!actor_.empty()) {
          send_closure(std::move(actor_), &WebhookActor::on_socket_ready_async, td::Status::Error("Canceled"), id_);
        }
      }
      void on_connected() final {
        // nothing to do
      }

     private:
      td::ActorId<WebhookActor> actor_;
      td::int64 id_;
    };

    auto id = pending_sockets_.create(td::ActorOwn<>());
    VLOG(webhook) << "Creating socket " << id;
    *pending_sockets_.get(id) = td::create_actor<td::HttpProxy>(
        "HttpProxy", r_proxy_socket_fd.move_as_ok(), ip_address_, td::string(), td::string(),
        td::make_unique<Callback>(actor_id(this), id), td::ActorShared<>());
    return td::Status::Error("Proxy connection is not ready");
  }

  auto r_fd = td::SocketFd::open(ip_address_);
  if (r_fd.is_error()) {
    td::Slice error_message = "Can't connect to the webhook";
    auto error = td::Status::Error(PSLICE() << error_message << ": " << r_fd.error());
    VLOG(webhook) << error;
    on_webhook_error(error_message);
    on_error(r_fd.move_as_error());
    return error;
  }
  return create_connection(td::BufferedFd<td::SocketFd>(r_fd.move_as_ok()));
}

td::Result<td::SslStream> WebhookActor::create_ssl_stream() {
  if (url_.protocol_ == td::HttpUrl::Protocol::Http) {
    return td::SslStream();
  }

  auto r_ssl_stream = td::SslStream::create(url_.host_, cert_path_, td::SslStream::VerifyPeer::On, !cert_path_.empty());
  if (r_ssl_stream.is_error()) {
    td::Slice error_message = "Can't create an SSL connection";
    auto error = td::Status::Error(PSLICE() << error_message << ": " << r_ssl_stream.error());
    VLOG(webhook) << error;
    on_webhook_error(PSLICE() << error_message << ": " << r_ssl_stream.error().public_message());
    on_error(r_ssl_stream.move_as_error());
    return std::move(error);
  }
  return r_ssl_stream.move_as_ok();
}

td::Status WebhookActor::create_connection(td::BufferedFd<td::SocketFd> fd) {
  TRY_RESULT(ssl_stream, create_ssl_stream());

  auto id = connections_.create(Connection());
  auto *conn = connections_.get(id);
  conn->actor_id_ = td::create_actor<td::HttpOutboundConnection>(
      PSLICE() << "Connect:" << id, std::move(fd), std::move(ssl_stream), std::numeric_limits<size_t>::max(), 20, 60,
      td::ActorShared<td::HttpOutboundConnection::Callback>(actor_id(this), id));
  conn->ip_generation_ = ip_generation_;
  conn->event_id_ = {};
  conn->id_ = id;
  ready_connections_.put(conn->to_list_node());
  total_connections_count_.fetch_add(1, std::memory_order_relaxed);

  if (!was_checked_) {
    was_checked_ = true;
    on_webhook_verified();
  }
  VLOG(webhook) << "Create connection " << id;
  return td::Status::OK();
}

void WebhookActor::on_socket_ready_async(td::Result<td::BufferedFd<td::SocketFd>> r_fd, td::int64 id) {
  pending_sockets_.erase(id);
  if (r_fd.is_ok()) {
    VLOG(webhook) << "Socket " << id << " is ready";
    ready_sockets_.push_back(r_fd.move_as_ok());
  } else {
    VLOG(webhook) << "Failed to open socket " << id;
    on_webhook_error(r_fd.error().message());
    on_error(r_fd.move_as_error());
  }
  loop();
}

void WebhookActor::create_new_connections() {
  size_t need_connections = queue_updates_.size();
  if (need_connections > static_cast<size_t>(max_connections_)) {
    need_connections = max_connections_;
  }
  if (!was_checked_) {
    need_connections = 1;
  }

  auto now = td::Time::now();
  td::FloodControlFast *flood;
  bool active;
  if (last_success_time_ + 10 < now) {
    flood = &pending_new_connection_flood_;
    if (need_connections > 1) {
      need_connections = 1;
    }
    active = false;
  } else {
    flood = &active_new_connection_flood_;
    if (need_connections == 0) {
      need_connections = 1;
    }
    active = true;
  }
  VLOG_IF(webhook, connections_.size() < need_connections)
      << "Create new connections " << td::tag("have", connections_.size()) << td::tag("need", need_connections)
      << td::tag("pending sockets", pending_sockets_.size()) << td::tag("ready sockets", ready_sockets_.size())
      << td::tag("active", active);
  while (connections_.size() + pending_sockets_.size() + ready_sockets_.size() < need_connections) {
    auto wakeup_at = flood->get_wakeup_at();
    if (now < wakeup_at) {
      relax_wakeup_at(wakeup_at, "create_new_connections");
      VLOG(webhook) << "Create new connection: flood control "
                    << td::tag("after", td::format::as_time(wakeup_at - now));
      break;
    }
    flood->add_event(static_cast<td::int32>(now));
    if (create_connection().is_error()) {
      relax_wakeup_at(now + 1.0, "create_new_connections error");
      return;
    }
  }
  SCOPE_EXIT {
    ready_sockets_.clear();
  };
  while (!ready_sockets_.empty() && connections_.size() + pending_sockets_.size() < need_connections) {
    auto socket_fd = std::move(ready_sockets_.back());
    ready_sockets_.pop_back();
    if (create_connection(std::move(socket_fd)).is_error()) {
      relax_wakeup_at(now + 1.0, "create_new_connections error 2");
      return;
    }
  }
}

void WebhookActor::loop() {
  VLOG(webhook) << "Enter loop";
  wakeup_at_ = 0;
  if (!stop_flag_) {
    load_updates();
  }
  if (!stop_flag_) {
    resolve_ip_address();
  }
  if (!stop_flag_) {
    create_new_connections();
  }
  if (!stop_flag_) {
    send_updates();
  }
  if (!stop_flag_) {
    if (wakeup_at_ != 0) {
      set_timeout_at(wakeup_at_);
    }
  }
  if (stop_flag_) {
    VLOG(webhook) << "Stop";
    stop();
  }
}

void WebhookActor::update() {
  VLOG(webhook) << "New updates in tqueue";
  tqueue_empty_ = false;
  loop();
}

void WebhookActor::load_updates() {
  if (tqueue_empty_) {
    VLOG(webhook) << "Load updates: tqueue is empty";
    return;
  }
  if (queue_updates_.size() >= max_loaded_updates_) {
    CHECK(queue_updates_.size() == max_loaded_updates_);
    VLOG(webhook) << "Load updates: maximum allowed number of updates is already loaded";
    return;
  }
  auto &tqueue = parameters_->shared_data_->tqueue_;
  if (tqueue_offset_.empty()) {
    tqueue_offset_ = tqueue->get_head(tqueue_id_);
  }
  VLOG(webhook) << "Trying to load new updates from offset " << tqueue_offset_;

  auto offset = tqueue_offset_;
  auto limit = td::min(SharedData::TQUEUE_EVENT_BUFFER_SIZE, max_loaded_updates_ - queue_updates_.size());
  td::MutableSpan<td::TQueue::Event> updates(parameters_->shared_data_->event_buffer_, limit);

  auto now = td::Time::now();
  auto unix_time_now = parameters_->shared_data_->get_unix_time(now);
  size_t total_size = 0;
  if (offset.empty()) {
    updates.truncate(0);
  } else {
    auto r_size = tqueue->get(tqueue_id_, offset, false, unix_time_now, updates);
    if (r_size.is_error()) {
      VLOG(webhook) << "Failed to get new updates: " << r_size.error();
      offset = tqueue_offset_ = tqueue->get_head(tqueue_id_);
      r_size = tqueue->get(tqueue_id_, offset, false, unix_time_now, updates);
      r_size.ensure();
    }
    total_size = r_size.ok();
  }
  if (updates.empty()) {
    tqueue_empty_ = true;
  }

  for (auto &update : updates) {
    VLOG(webhook) << "Load update " << update.id;
    CHECK(update.id.is_valid());
    auto &dest_ptr = update_map_[update.id];
    if (dest_ptr != nullptr) {
      LOG(ERROR) << "Receive duplicated event " << update.id << " from TQueue";
      continue;
    }
    dest_ptr = td::make_unique<Update>();
    auto &dest = *dest_ptr;
    dest.id_ = update.id;
    dest.json_ = update.data.str();
    dest.delay_ = 1;
    dest.wakeup_at_ = now;
    CHECK(update.expires_at >= unix_time_now);
    dest.expires_at_ = update.expires_at;
    dest.queue_id_ = update.extra;
    tqueue_offset_ = update.id.next().move_as_ok();

    if (dest.queue_id_ == 0) {
      dest.queue_id_ = unique_queue_id_++;
    }

    auto &queue_updates = queue_updates_[dest.queue_id_];
    if (queue_updates.event_ids.empty()) {
      queues_.emplace(dest.wakeup_at_, dest.queue_id_);
    }
    queue_updates.event_ids.push(dest.id_);
  }

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
    LOG(WARNING) << "Loaded " << updates.size() << " updates out of " << total_size << ". Have " << update_map_.size()
                 << " updates loaded in " << queue_updates_.size() << " queues after last error \""
                 << last_error_message_ << "\" " << (last_error_time_ == 0 ? -1 : td::Time::now() - last_error_time_)
                 << " seconds ago";
  }

  if (updates.size() == total_size && last_update_was_successful_) {
    send_closure(callback_, &Callback::webhook_success);
  }

  if (!updates.empty()) {
    VLOG(webhook) << "Loaded " << updates.size() << " new updates from offset " << offset << " out of requested "
                  << limit << ". Have total of " << update_map_.size() << " updates loaded in " << queue_updates_.size()
                  << " queues";
  }
}

void WebhookActor::drop_event(td::TQueue::EventId event_id) {
  auto it = update_map_.find(event_id);
  CHECK(it != update_map_.end());
  auto queue_id = it->second->queue_id_;
  update_map_.erase(it);

  auto queue_updates_it = queue_updates_.find(queue_id);

  CHECK(!queue_updates_it->second.event_ids.empty());
  CHECK(event_id == queue_updates_it->second.event_ids.front());
  queue_updates_it->second.event_ids.pop();
  if (queue_updates_it->second.event_ids.empty()) {
    queue_updates_.erase(queue_updates_it);
  } else {
    auto update_id = queue_updates_it->second.event_ids.front();
    CHECK(update_id.is_valid());
    auto &update = update_map_[update_id];
    queues_.emplace(update->wakeup_at_, update->queue_id_);
  }

  parameters_->shared_data_->tqueue_->forget(tqueue_id_, event_id);
}

void WebhookActor::on_update_ok(td::TQueue::EventId event_id) {
  last_update_was_successful_ = true;
  last_success_time_ = td::Time::now();

  auto it = update_map_.find(event_id);
  CHECK(it != update_map_.end());

  VLOG(webhook) << "Receive ok for update " << event_id << " in " << (last_success_time_ - it->second->last_send_time_)
                << " seconds";

  drop_event(event_id);
}

void WebhookActor::on_update_error(td::TQueue::EventId event_id, td::Slice error, int retry_after) {
  last_update_was_successful_ = false;
  double now = td::Time::now();

  auto it = update_map_.find(event_id);
  CHECK(it != update_map_.end());
  CHECK(it->second != nullptr);
  auto &update = *it->second;

  const int MAX_RETRY_AFTER = 3600;
  retry_after = td::clamp(retry_after, 0, MAX_RETRY_AFTER);
  int next_delay = update.delay_;
  int next_effective_delay = retry_after;
  if (retry_after == 0 && update.fail_count_ > 0) {
    next_delay = td::min(WEBHOOK_MAX_RESEND_TIMEOUT, next_delay * 2);
    next_effective_delay = next_delay;
  }
  if (parameters_->shared_data_->get_unix_time(now) + next_effective_delay > update.expires_at_) {
    LOG(WARNING) << "Drop update " << event_id << ": " << error;
    drop_event(event_id);
    return;
  }
  update.delay_ = next_delay;
  update.wakeup_at_ = now + next_effective_delay;
  update.fail_count_++;
  queues_.emplace(update.wakeup_at_, update.queue_id_);
  VLOG(webhook) << "Delay update " << event_id << " for " << (update.wakeup_at_ - now) << " seconds because of "
                << error << " after " << update.fail_count_ << " fails received in " << (now - update.last_send_time_)
                << " seconds";
}

td::Status WebhookActor::send_update() {
  if (ready_connections_.empty()) {
    return td::Status::Error("No connection");
  }

  if (queues_.empty()) {
    return td::Status::Error("No pending updates");
  }
  auto it = queues_.begin();
  auto now = td::Time::now();
  if (it->wakeup_at > now) {
    relax_wakeup_at(it->wakeup_at, "send_update");
    return td::Status::Error("No ready updates");
  }

  auto queue_id = it->id;
  CHECK(queue_id != 0);
  queues_.erase(it);
  auto event_id = queue_updates_[queue_id].event_ids.front();
  CHECK(event_id.is_valid());

  auto update_map_it = update_map_.find(event_id);
  CHECK(update_map_it != update_map_.end());
  CHECK(update_map_it->second != nullptr);
  auto &update = *update_map_it->second;
  update.last_send_time_ = now;

  auto body = td::json_encode<td::BufferSlice>(JsonUpdate(update.id_.value(), update.json_));

  td::HttpHeaderCreator hc;
  hc.init_post(url_.query_);
  hc.add_header("Host", url_.host_);
  if (!url_.userinfo_.empty()) {
    hc.add_header("Authorization", PSLICE() << "Basic " << td::base64_encode(url_.userinfo_));
  }
  if (!secret_token_.empty()) {
    hc.add_header("X-Telegram-Bot-Api-Secret-Token", secret_token_);
  }
  hc.set_content_type("application/json");
  hc.set_content_size(body.size());
  hc.set_keep_alive();
  hc.add_header("Accept-Encoding", "gzip, deflate");
  auto r_header = hc.finish();
  if (r_header.is_error()) {
    return td::Status::Error(400, "URL is too long");
  }

  auto &connection = *Connection::from_list_node(ready_connections_.get());
  connection.event_id_ = update.id_;

  VLOG(webhook) << "Send update " << update.id_ << " from queue " << queue_id << " into connection " << connection.id_
                << ": " << update.json_;
  VLOG(webhook) << "Request headers: " << r_header.ok();

  send_closure(connection.actor_id_, &td::HttpOutboundConnection::write_next_noflush, td::BufferSlice(r_header.ok()));
  send_closure(connection.actor_id_, &td::HttpOutboundConnection::write_next_noflush, std::move(body));
  send_closure(connection.actor_id_, &td::HttpOutboundConnection::write_ok);
  return td::Status::OK();
}

void WebhookActor::send_updates() {
  VLOG(webhook) << "Have " << (queues_.size() + update_map_.size() - queue_updates_.size()) << " pending updates in "
                << queues_.size() << " queues to send";
  while (send_update().is_ok()) {
  }
}

void WebhookActor::handle(td::unique_ptr<td::HttpQuery> response) {
  auto connection_id = get_link_token();
  if (response) {
    VLOG(webhook) << "Got response from connection " << connection_id;
  } else {
    VLOG(webhook) << "Got hangup from connection " << connection_id;
  }
  auto *connection_ptr = connections_.get(connection_id);
  if (connection_ptr == nullptr) {
    return;
  }

  bool close_connection = false;
  td::string query_error;
  td::int32 retry_after = 0;
  bool need_close = false;

  if (response) {
    if (response->type_ != td::HttpQuery::Type::Response || !response->keep_alive_ ||
        ip_generation_ != connection_ptr->ip_generation_) {
      close_connection = true;
    }
    if (response->type_ == td::HttpQuery::Type::Response) {
      if (200 <= response->code_ && response->code_ <= 299) {
        auto method = response->get_arg("method");
        td::to_lower_inplace(method);
        if (!method.empty() && method != "deletewebhook" && method != "setwebhook" && method != "close" &&
            method != "logout" && !td::begins_with(method, "get")) {
          VLOG(webhook) << "Receive request " << method << " in response to webhook";
          auto query = td::make_unique<Query>(std::move(response->container_), td::MutableSlice(), false,
                                              td::MutableSlice(), std::move(response->args_),
                                              std::move(response->headers_), std::move(response->files_),
                                              parameters_->shared_data_, response->peer_address_, false);
          auto promised_query =
              PromisedQueryPtr(query.release(), PromiseDeleter(td::PromiseActor<td::unique_ptr<Query>>()));
          send_closure(callback_, &Callback::send, std::move(promised_query));
        }
        first_error_410_time_ = 0;
      } else {
        query_error = PSTRING() << "Wrong response from the webhook: " << response->code_ << " " << response->reason_;
        if (response->code_ == 410) {
          if (first_error_410_time_ == 0) {
            first_error_410_time_ = td::Time::now();
          } else {
            if (td::Time::now() > first_error_410_time_ + WEBHOOK_DROP_TIMEOUT) {
              LOG(WARNING) << "Close webhook because of HTTP 410 errors";
              need_close = true;
            }
          }
        } else {
          first_error_410_time_ = 0;
        }
        retry_after = response->get_retry_after();
        // LOG(WARNING) << query_error;
        on_webhook_error(query_error);
      }
    } else {
      query_error = PSTRING() << "Wrong response from the webhook: " << *response;
      on_webhook_error(query_error);
    }
    VLOG(webhook) << *response;
  } else {
    query_error = "Webhook connection closed";
    connection_ptr->actor_id_.release();
    close_connection = true;
  }

  auto event_id = connection_ptr->event_id_;
  if (!event_id.empty()) {
    if (query_error.empty()) {
      on_update_ok(event_id);
    } else {
      on_update_error(event_id, query_error, retry_after);
    }
  } else {
    CHECK(!query_error.empty());
  }

  connection_ptr->event_id_ = {};
  if (need_close || close_connection) {
    VLOG(webhook) << "Close connection " << connection_id;
    connections_.erase(connection_ptr->id_);
    total_connections_count_.fetch_sub(1, std::memory_order_relaxed);
  } else {
    ready_connections_.put(connection_ptr->to_list_node());
  }

  if (need_close) {
    send_closure_later(actor_id(this), &WebhookActor::close);
  } else {
    loop();
  }
}

void WebhookActor::start_up() {
  max_loaded_updates_ = max_connections_ * 2;

  next_ip_address_resolve_time_ = last_success_time_ = td::Time::now() - 3600;
  active_new_connection_flood_.add_limit(1, 10 * max_connections_);
  active_new_connection_flood_.add_limit(5, 20 * max_connections_);

  pending_new_connection_flood_.add_limit(1, 1);

  if (!parameters_->local_mode_) {
    if (url_.protocol_ == td::HttpUrl::Protocol::Https) {
      if (url_.port_ != 443 && url_.port_ != 88 && url_.port_ != 80 && url_.port_ != 8443) {
        VLOG(webhook) << "Can't create webhook: port " << url_.port_ << " is forbidden";
        on_error(td::Status::Error("Webhook can be set up only on ports 80, 88, 443 or 8443"));
      }
    } else {
      CHECK(url_.protocol_ == td::HttpUrl::Protocol::Http);
      VLOG(webhook) << "Can't create connection: HTTP is forbidden";
      on_error(td::Status::Error("HTTPS url must be provided for webhook"));
    }
  }

  if (fix_ip_address_ && !stop_flag_) {
    if (!ip_address_.is_valid()) {
      on_error(td::Status::Error("Invalid IP address specified"));
    } else if (!check_ip_address(ip_address_)) {
      on_error(td::Status::Error(PSLICE() << "IP address " << ip_address_.get_ip_str() << " is reserved"));
    }
  }

  if (from_db_flag_ && !stop_flag_) {
    was_checked_ = true;
    on_webhook_verified();
  }

  yield();
}

void WebhookActor::hangup_shared() {
  handle(nullptr);
  loop();
}

void WebhookActor::hangup() {
  VLOG(webhook) << "Stop";
  callback_.release();
  stop();
}

void WebhookActor::close() {
  VLOG(webhook) << "Close";
  send_closure(std::move(callback_), &Callback::webhook_closed, td::Status::OK());
  stop();
}

void WebhookActor::tear_down() {
  total_connections_count_.fetch_sub(connections_.size(), std::memory_order_relaxed);
}

void WebhookActor::on_webhook_verified() {
  td::string ip_address_str;
  if (ip_address_.is_valid()) {
    ip_address_str = ip_address_.get_ip_str().str();
  }
  send_closure(callback_, &Callback::webhook_verified, std::move(ip_address_str));
}

bool WebhookActor::check_ip_address(const td::IPAddress &addr) const {
  if (!addr.is_valid()) {
    return false;
  }
  if (parameters_->local_mode_) {
    // allow any valid IP address
    return true;
  }
  if (!addr.is_ipv4()) {
    VLOG(webhook) << "Bad IP address (not IPv4): " << addr;
    return false;
  }
  return !addr.is_reserved();
}

void WebhookActor::on_error(td::Status status) {
  VLOG(webhook) << "Receive webhook error " << status;
  if (!was_checked_) {
    CHECK(!callback_.empty());
    send_closure(std::move(callback_), &Callback::webhook_closed, std::move(status));
    stop_flag_ = true;
  }
}

void WebhookActor::on_connection_error(td::Status error) {
  CHECK(error.is_error());
  on_webhook_error(error.message());
}

void WebhookActor::on_webhook_error(td::Slice error) {
  if (was_checked_) {
    send_closure(callback_, &Callback::webhook_error, td::Status::Error(error));
    last_error_time_ = td::Time::now();
    last_error_message_ = error.str();
  }
}

}  // namespace telegram_bot_api
