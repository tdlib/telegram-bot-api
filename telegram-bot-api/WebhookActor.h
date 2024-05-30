//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2024
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "telegram-bot-api/Query.h"

#include "td/db/TQueue.h"

#include "td/net/HttpOutboundConnection.h"
#include "td/net/HttpQuery.h"
#include "td/net/SslCtx.h"
#include "td/net/SslStream.h"

#include "td/actor/actor.h"

#include "td/utils/BufferedFd.h"
#include "td/utils/common.h"
#include "td/utils/Container.h"
#include "td/utils/FlatHashMap.h"
#include "td/utils/FloodControlFast.h"
#include "td/utils/HashTableUtils.h"
#include "td/utils/HttpUrl.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/List.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/SocketFd.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"
#include "td/utils/VectorQueue.h"

#include <atomic>
#include <memory>
#include <set>
#include <tuple>

namespace telegram_bot_api {

struct ClientParameters;

class WebhookActor final : public td::HttpOutboundConnection::Callback {
 public:
  class Callback : public td::Actor {
   public:
    virtual void webhook_verified(td::string cached_ip) = 0;
    virtual void webhook_success() = 0;
    virtual void webhook_error(td::Status status) = 0;
    virtual void webhook_closed(td::Status status) = 0;
    virtual void send(PromisedQueryPtr query) = 0;
  };

  WebhookActor(td::ActorShared<Callback> callback, td::int64 tqueue_id, td::HttpUrl url, td::string cert_path,
               td::int32 max_connections, bool from_db_flag, td::string cached_ip_address, bool fix_ip_address,
               td::string secret_token, std::shared_ptr<const ClientParameters> parameters);
  WebhookActor(const WebhookActor &) = delete;
  WebhookActor &operator=(const WebhookActor &) = delete;
  WebhookActor(WebhookActor &&) = delete;
  WebhookActor &operator=(WebhookActor &&) = delete;
  ~WebhookActor();

  void update();

  void close();

  static td::int64 get_total_connection_count() {
    return total_connection_count_;
  }

 private:
  static constexpr std::size_t MIN_PENDING_UPDATES_WARNING = 50;
  static constexpr int IP_ADDRESS_CACHE_TIME = 30 * 60;  // 30 minutes
  static constexpr int WEBHOOK_MAX_RESEND_TIMEOUT = 60;
  static constexpr int WEBHOOK_DROP_TIMEOUT = 60 * 60 * 23;

  static std::atomic<td::uint64> total_connection_count_;

  td::ActorShared<Callback> callback_;
  td::int64 tqueue_id_;
  bool tqueue_empty_ = false;
  std::size_t last_pending_update_count_ = MIN_PENDING_UPDATES_WARNING;
  td::HttpUrl url_;
  const td::string cert_path_;
  std::shared_ptr<const ClientParameters> parameters_;

  double last_error_time_ = 0;
  td::string last_error_message_ = "<none>";

  bool fix_ip_address_ = false;

  bool stop_flag_ = false;

  bool was_checked_ = false;
  bool from_db_flag_ = false;

  class Update {
   public:
    td::TQueue::EventId id_;
    td::string json_;
    td::int32 expires_at_ = 0;
    double last_send_time_ = 0;
    double wakeup_at_ = 0;
    int delay_ = 0;
    int fail_count_ = 0;
    td::int64 queue_id_ = 0;
  };

  struct QueueUpdates {
    td::VectorQueue<td::TQueue::EventId> event_ids;
  };

  struct Queue {
    Queue() = default;
    Queue(double wakeup_at, td::int64 id)
        : wakeup_at(wakeup_at), integer_wakeup_at(static_cast<td::int64>(wakeup_at * 1e9)), id(id) {
    }
    double wakeup_at{0};
    td::int64 integer_wakeup_at{0};
    td::int64 id{0};

    bool operator<(const Queue &other) const {
      return std::tie(integer_wakeup_at, id) < std::tie(other.integer_wakeup_at, other.id);
    }
  };

  td::TQueue::EventId tqueue_offset_;
  std::size_t max_loaded_updates_ = 0;
  struct EventIdHash {
    td::uint32 operator()(td::TQueue::EventId event_id) const {
      return td::Hash<td::int32>()(event_id.value());
    }
  };
  td::FlatHashMap<td::TQueue::EventId, td::unique_ptr<Update>, EventIdHash> update_map_;
  td::FlatHashMap<td::int64, QueueUpdates> queue_updates_;
  std::set<Queue> queues_;
  td::int64 unique_queue_id_ = static_cast<td::int64>(1) << 60;

  double first_error_410_time_ = 0;

  td::SslCtx ssl_ctx_;
  td::IPAddress ip_address_;
  td::int32 ip_generation_ = 0;
  double next_ip_address_resolve_time_ = 0;
  bool is_ip_address_being_resolved_ = false;

  class Connection final : public td::ListNode {
   public:
    Connection() = default;
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    Connection(Connection &&) = default;
    Connection &operator=(Connection &&) = default;
    ~Connection() = default;

    td::ActorOwn<td::HttpOutboundConnection> actor_id_;
    td::uint64 id_ = 0;
    td::TQueue::EventId event_id_;
    td::int32 ip_generation_ = -1;
    static Connection *from_list_node(ListNode *node) {
      return static_cast<Connection *>(node);
    }
    ListNode *to_list_node() {
      return this;
    }
  };
  td::Container<td::ActorOwn<>> pending_sockets_;
  td::vector<td::BufferedFd<td::SocketFd>> ready_sockets_;

  td::int32 max_connections_ = 0;
  td::string secret_token_;
  td::Container<Connection> connections_;
  td::ListNode ready_connections_;
  td::FloodControlFast active_new_connection_flood_;
  td::FloodControlFast pending_new_connection_flood_;
  double last_success_time_ = 0;
  double wakeup_at_ = 0;
  bool last_update_was_successful_ = true;

  void relax_wakeup_at(double wakeup_at, const char *source);

  void resolve_ip_address();
  void on_resolved_ip_address(td::Result<td::IPAddress> r_ip_address);

  void on_ssl_context_created(td::Result<td::SslCtx> r_ssl_ctx);

  td::Status create_webhook_error(td::Slice error_message, td::Status &&result, bool is_public);

  td::Result<td::SslStream> create_ssl_stream();
  td::Status create_connection() TD_WARN_UNUSED_RESULT;
  td::Status create_connection(td::BufferedFd<td::SocketFd> fd) TD_WARN_UNUSED_RESULT;
  void on_socket_ready_async(td::Result<td::BufferedFd<td::SocketFd>> r_fd, td::int64 id);

  void create_new_connections();

  void drop_event(td::TQueue::EventId event_id);

  void load_updates();
  void on_update_ok(td::TQueue::EventId event_id);
  void on_update_error(td::TQueue::EventId event_id, td::Slice error, int retry_after);
  td::Status send_update() TD_WARN_UNUSED_RESULT;
  void send_updates();

  void loop() final;
  void handle(td::unique_ptr<td::HttpQuery> response) final;

  void hangup_shared() final;

  void hangup() final;

  void tear_down() final;

  void start_up() final;

  td::Status check_ip_address(const td::IPAddress &addr) const;

  void on_error(td::Status status);
  void on_connection_error(td::Status error) final;
  void on_webhook_error(td::Slice error);
  void on_webhook_verified();
};

class JsonUpdate final : public td::Jsonable {
 public:
  JsonUpdate(td::int32 id, td::Slice update) : id_(id), update_(update) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("update_id", id_);
    object << td::JsonRaw(",\n");
    CHECK(!update_.empty());
    object << td::JsonRaw(update_);
  }

 private:
  td::int32 id_;
  td::Slice update_;
};

}  // namespace telegram_bot_api
