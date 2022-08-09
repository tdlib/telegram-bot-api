//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2021
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "telegram-bot-api/ClientManager.h"

#include "td/net/HttpInboundConnection.h"
#include "td/net/HttpQuery.h"

#include "td/actor/actor.h"
#include "td/actor/PromiseFuture.h"

#include "td/utils/buffer.h"

namespace telegram_bot_api {

class HttpStatConnection final : public td::HttpInboundConnection::Callback {
 public:
  explicit HttpStatConnection(td::ActorId<ClientManager> client_manager) : client_manager_(client_manager) {
  }
  void handle(td::unique_ptr<td::HttpQuery> http_query, td::ActorOwn<td::HttpInboundConnection> connection) final;

  void wakeup() final;

 private:
  td::FutureActor<td::BufferSlice> result_;
  td::ActorId<ClientManager> client_manager_;
  td::ActorOwn<td::HttpInboundConnection> connection_;

  void hangup() final {
    connection_.release();
    stop();
  }
};

}  // namespace telegram_bot_api
