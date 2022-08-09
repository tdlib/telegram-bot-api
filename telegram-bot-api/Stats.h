//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2021
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/actor/actor.h"

#include "td/utils/common.h"
#include "td/utils/logging.h"
#include "td/utils/port/Stat.h"
#include "td/utils/Time.h"
#include "td/utils/TimedStat.h"

#include <mutex>

namespace telegram_bot_api {

struct StatItem {
  td::string key_;
  td::string value_;
};

class CpuStat {
 public:
  void on_event(const td::CpuStat &event) {
    if (cnt_ == 0) {
      first_ = event;
      cnt_ = 1;
    } else {
      cnt_ = 2;
      last_ = event;
    }
  }

  td::vector<StatItem> as_vector() const;

 private:
  int cnt_ = 0;
  td::CpuStat first_;
  td::CpuStat last_;
};

class ServerCpuStat {
 public:
  static ServerCpuStat &instance() {
    static ServerCpuStat stat;
    return stat;
  }
  static void update(double now) {
    auto r_event = td::cpu_stat();
    if (r_event.is_error()) {
      return;
    }
    instance().add_event(r_event.ok(), now);
    LOG(WARNING) << "CPU usage: " << instance().stat_[1].get_stat(now).as_vector()[0].value_;
  }

  td::string get_description() const;

  td::vector<StatItem> as_vector(double now);

 private:
  static constexpr std::size_t SIZE = 4;
  static constexpr const char *DESCR[SIZE] = {"inf", "5sec", "1min", "1hour"};
  static constexpr int DURATIONS[SIZE] = {0, 5, 60, 60 * 60};

  std::mutex mutex_;
  td::TimedStat<CpuStat> stat_[SIZE];

  ServerCpuStat();

  void add_event(const td::CpuStat &stat, double now);
};

class ServerBotInfo {
 public:
  td::string id_;
  td::string token_;
  td::string username_;
  td::string webhook_;
  bool has_webhook_certificate_ = false;
  td::int32 head_update_id_ = 0;
  td::int32 tail_update_id_ = 0;
  td::int32 webhook_max_connections_ = 0;
  std::size_t pending_update_count_ = 0;
  double start_time_ = 0;
};

struct ServerBotStat {
  double request_count_ = 0;
  double request_bytes_ = 0;
  double request_file_count_ = 0;
  double request_files_bytes_ = 0;
  td::int64 request_files_max_bytes_ = 0;

  double response_count_ = 0;
  double response_count_ok_ = 0;
  double response_count_error_ = 0;
  double response_bytes_ = 0;

  double update_count_ = 0;

  void normalize(double duration);

  void add(const ServerBotStat &stat);

  struct Update {};
  void on_event(const Update &update) {
    update_count_ += 1;
  }

  struct Response {
    bool ok_;
    size_t size_;
  };
  void on_event(const Response &answer) {
    response_count_++;
    if (answer.ok_) {
      response_count_ok_++;
    } else {
      response_count_error_++;
    }
    response_bytes_ += static_cast<double>(answer.size_);
  }

  struct Request {
    td::int64 size_;
    td::int64 file_count_;
    td::int64 files_size_;
    td::int64 files_max_size_;
  };
  void on_event(const Request &request) {
    request_count_++;
    request_bytes_ += static_cast<double>(request.size_);
    request_file_count_ += static_cast<double>(request.file_count_);
    request_files_bytes_ += static_cast<double>(request.files_size_);
    request_files_max_bytes_ = td::max(request_files_max_bytes_, request.files_max_size_);
  }

  td::vector<StatItem> as_vector() const;
};

class BotStatActor final : public td::Actor {
 public:
  BotStatActor() = default;
  explicit BotStatActor(td::ActorId<BotStatActor> parent) : parent_(parent) {
    for (std::size_t i = 0; i < SIZE; i++) {
      stat_[i] = td::TimedStat<ServerBotStat>(DURATIONS[i], td::Time::now());
    }
    register_actor("ServerBotStat", this).release();
  }

  BotStatActor(const BotStatActor &) = delete;
  BotStatActor &operator=(const BotStatActor &other) = delete;
  BotStatActor(BotStatActor &&) = default;
  BotStatActor &operator=(BotStatActor &&other) noexcept {
    if (!empty()) {
      do_stop();
    }
    this->Actor::operator=(std::move(other));
    std::move(other.stat_, other.stat_ + SIZE, stat_);
    parent_ = other.parent_;
    return *this;
  }
  ~BotStatActor() final = default;

  template <class EventT>
  void add_event(const EventT &event, double now) {
    last_activity_timestamp_ = now;
    for (auto &stat : stat_) {
      stat.add_event(event, now);
    }
    if (!parent_.empty()) {
      send_closure(parent_, &BotStatActor::add_event<EventT>, event, now);
    }
  }

  td::vector<StatItem> as_vector(double now);
  td::string get_description() const;

  bool is_active(double now) const;

 private:
  static constexpr std::size_t SIZE = 4;
  static constexpr const char *DESCR[SIZE] = {"inf", "5sec", "1min", "1hour"};
  static constexpr int DURATIONS[SIZE] = {0, 5, 60, 60 * 60};

  td::TimedStat<ServerBotStat> stat_[SIZE];
  td::ActorId<BotStatActor> parent_;
  double last_activity_timestamp_ = -1e9;
};

}  // namespace telegram_bot_api
