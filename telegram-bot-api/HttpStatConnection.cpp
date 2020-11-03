//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2020
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/HttpStatConnection.h"

#include "td/net/HttpHeaderCreator.h"

#include "td/utils/common.h"
#include "td/utils/logging.h"

namespace telegram_bot_api {

void HttpStatConnection::handle(td::unique_ptr<td::HttpQuery> http_query,
                                td::ActorOwn<td::HttpInboundConnection> connection) {
  CHECK(connection_->empty());
  connection_ = std::move(connection);

  td::PromiseActor<td::BufferSlice> promise;
  td::FutureActor<td::BufferSlice> future;
  init_promise_future(&promise, &future);
  future.set_event(td::EventCreator::yield(actor_id()));
  LOG(DEBUG) << "SEND";
  send_closure(client_manager_, &ClientManager::get_stats, std::move(promise), http_query->get_args());
  result_ = std::move(future);
}

void HttpStatConnection::wakeup() {
  if (result_.empty()) {
    return;
  }
  LOG_CHECK(result_.is_ok()) << result_.move_as_error();

  auto content = result_.move_as_ok();
  td::HttpHeaderCreator hc;
  hc.init_status_line(200);
  hc.set_keep_alive();
  hc.set_content_type("text/plain");
  hc.set_content_size(content.size());

  auto r_header = hc.finish();
  if (r_header.is_error()) {
    send_closure(connection_.release(), &td::HttpInboundConnection::write_error, r_header.move_as_error());
    return;
  }
  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, td::BufferSlice(r_header.ok()));
  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, std::move(content));
  send_closure(connection_.release(), &td::HttpInboundConnection::write_ok);
}

}  // namespace telegram_bot_api
