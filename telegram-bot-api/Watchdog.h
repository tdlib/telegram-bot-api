//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2024
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/actor/actor.h"

#include "td/utils/port/thread.h"

namespace telegram_bot_api {

class Watchdog final : public td::Actor {
 public:
  Watchdog(td::thread::id main_thread_id, double timeout) : main_thread_id_(main_thread_id), timeout_(timeout) {
    // watchdog is disabled until it is kicked for the first time
  }

  void kick();

 private:
  void timeout_expired() final;

  td::thread::id main_thread_id_;
  double timeout_;
  double last_kick_time_ = 0.0;
};

}  // namespace telegram_bot_api
