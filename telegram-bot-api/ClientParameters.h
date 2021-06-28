//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2021
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/actor/actor.h"

#include "td/db/KeyValueSyncInterface.h"
#include "td/db/TQueue.h"

#include "td/net/GetHostByNameActor.h"

#include "td/utils/common.h"
#include "td/utils/List.h"
#include "td/utils/port/IPAddress.h"

#include <atomic>
#include <limits>
#include <memory>

namespace td {
class NetQueryStats;
}

namespace telegram_bot_api {

struct SharedData {
  std::atomic<td::uint64> query_count_{0};
  std::atomic<int> next_verbosity_level_{-1};

  // not thread-safe
  td::ListNode query_list_;
  td::unique_ptr<td::KeyValueSyncInterface> webhook_db_;
  td::unique_ptr<td::KeyValueSyncInterface> user_db_;
  td::unique_ptr<td::TQueue> tqueue_;

  double unix_time_difference_{-1e100};

  static constexpr size_t TQUEUE_EVENT_BUFFER_SIZE = 1000;
  td::TQueue::Event event_buffer_[TQUEUE_EVENT_BUFFER_SIZE];

  td::int32 get_unix_time(double now) const {
    auto result = unix_time_difference_ + now;
    if (result <= 0) {
      return 0;
    }
    if (result >= std::numeric_limits<td::int32>::max()) {
      return std::numeric_limits<td::int32>::max();
    }
    return static_cast<td::int32>(result);
  }
};

struct ClientParameters {
  td::string working_directory_;
  bool allow_colon_in_filenames_ = true;

  bool local_mode_ = false;
  bool allow_http_ = false;
  bool use_relative_path_ = false;
  bool no_file_limit_ = true;
  bool allow_users_ = false;
  bool allow_users_registration_ = false;
  bool stats_hide_sensible_data_ = false;

  td::int32 api_id_ = 0;
  td::string api_hash_;

  td::string version_;

  td::int32 default_max_webhook_connections_ = 0;
  td::IPAddress webhook_proxy_ip_address_;

  td::uint32 max_batch_operations = 10000;
  double start_time_ = 0;

  td::ActorId<td::GetHostByNameActor> get_host_by_name_actor_id_;

  std::shared_ptr<SharedData> shared_data_;

  std::shared_ptr<td::NetQueryStats> net_query_stats_;
};

}  // namespace telegram_bot_api
