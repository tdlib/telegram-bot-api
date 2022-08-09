//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2021
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/net/HttpInboundConnection.h"
#include "td/net/TcpListener.h"

#include "td/actor/actor.h"

#include "td/utils/BufferedFd.h"
#include "td/utils/FloodControlFast.h"
#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/port/SocketFd.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/Time.h"

#include <functional>

namespace telegram_bot_api {

class HttpServer final : public td::TcpListener::Callback {
 public:
  HttpServer(td::string ip_address, int port,
             std::function<td::ActorOwn<td::HttpInboundConnection::Callback>()> creator)
      : ip_address_(std::move(ip_address)), port_(port), creator_(std::move(creator)) {
    flood_control_.add_limit(1, 1);    // 1 in a second
    flood_control_.add_limit(60, 10);  // 10 in a minute
  }

 private:
  td::string ip_address_;
  td::int32 port_;
  std::function<td::ActorOwn<td::HttpInboundConnection::Callback>()> creator_;
  td::ActorOwn<td::TcpListener> listener_;
  td::FloodControlFast flood_control_;

  void start_up() final {
    auto now = td::Time::now();
    auto wakeup_at = flood_control_.get_wakeup_at();
    if (wakeup_at > now) {
      set_timeout_at(wakeup_at);
      return;
    }
    flood_control_.add_event(static_cast<td::int32>(now));
    LOG(INFO) << "Create tcp listener " << td::tag("address", ip_address_) << td::tag("port", port_);
    listener_ = td::create_actor<td::TcpListener>(
        PSLICE() << "TcpListener" << td::tag("address", ip_address_) << td::tag("port", port_), port_,
        actor_shared(this, 1), ip_address_);
  }

  void hangup_shared() final {
    LOG(ERROR) << "TCP listener was closed";
    listener_.release();
    yield();
  }

  void accept(td::SocketFd fd) final {
    auto scheduler_count = td::Scheduler::instance()->sched_count();
    auto scheduler_id = scheduler_count - 1;
    if (scheduler_id > 0) {
      scheduler_id--;
    }
    td::create_actor<td::HttpInboundConnection>("HttpInboundConnection", td::BufferedFd<td::SocketFd>(std::move(fd)), 0,
                                                20, 500, creator_(), scheduler_id)
        .release();
  }

  void loop() final {
    if (listener_.empty()) {
      start_up();
    }
  }
};

}  // namespace telegram_bot_api
