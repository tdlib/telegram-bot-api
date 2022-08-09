//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2021
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "telegram-bot-api/ClientManager.h"
#include "telegram-bot-api/Query.h"

#include "td/net/HttpInboundConnection.h"
#include "td/net/HttpQuery.h"

#include "td/actor/actor.h"
#include "td/actor/PromiseFuture.h"

#include "td/utils/buffer.h"
#include "td/utils/Slice.h"

#include <memory>

namespace telegram_bot_api {

struct SharedData;

class HttpConnection final : public td::HttpInboundConnection::Callback {
 public:
  explicit HttpConnection(td::ActorId<ClientManager> client_manager, std::shared_ptr<SharedData> shared_data)
      : client_manager_(client_manager), shared_data_(std::move(shared_data)) {
  }

  void handle(td::unique_ptr<td::HttpQuery> http_query, td::ActorOwn<td::HttpInboundConnection> connection) final;

  void wakeup() final;

 private:
  td::FutureActor<td::unique_ptr<Query>> result_;
  td::ActorId<ClientManager> client_manager_;
  td::ActorOwn<td::HttpInboundConnection> connection_;
  std::shared_ptr<SharedData> shared_data_;

  void hangup() final {
    connection_.release();
    stop();
  }

  void send_response(int http_status_code, td::BufferSlice &&content, int retry_after);

  void send_http_error(int http_status_code, td::Slice description);
};

}  // namespace telegram_bot_api
