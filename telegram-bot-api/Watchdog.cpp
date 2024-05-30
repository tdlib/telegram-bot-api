//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2024
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/Watchdog.h"

#include "td/utils/logging.h"
#include "td/utils/Time.h"

namespace telegram_bot_api {

void Watchdog::kick() {
  auto now = td::Time::now();
  if (now >= last_kick_time_ + timeout_ && last_kick_time_ > 0 && GET_VERBOSITY_LEVEL() >= VERBOSITY_NAME(ERROR)) {
    LOG(ERROR) << get_name() << " timeout expired after " << now - last_kick_time_ << " seconds";
    td::thread::send_real_time_signal(main_thread_id_, 2);
  }
  last_kick_time_ = now;
  set_timeout_in(timeout_);
}

void Watchdog::timeout_expired() {
  kick();
}

}  // namespace telegram_bot_api
