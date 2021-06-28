//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2021
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/Stats.h"

#include "td/utils/common.h"
#include "td/utils/port/thread.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/StringBuilder.h"

namespace telegram_bot_api {

ServerCpuStat::ServerCpuStat() {
  for (std::size_t i = 1; i < SIZE; i++) {
    stat_[i] = td::TimedStat<CpuStat>(DURATIONS[i], td::Time::now());
  }
}

void ServerCpuStat::add_event(const td::CpuStat &cpu_stat, double now) {
  std::lock_guard<std::mutex> guard(mutex_);
  for (auto &stat : stat_) {
    stat.add_event(cpu_stat, now);
  }
}

td::string ServerCpuStat::get_description() const {
  td::string res = "DURATION";
  for (auto &descr : DESCR) {
    res += "\t";
    res += descr;
  }
  return res;
}

static td::string to_percentage(td::uint64 ticks, td::uint64 total_ticks) {
  static double multiplier = 100.0 * (td::thread::hardware_concurrency() ? td::thread::hardware_concurrency() : 1);
  return PSTRING() << (static_cast<double>(ticks) / static_cast<double>(total_ticks) * multiplier) << "%";
}

td::vector<StatItem> CpuStat::as_vector() const {
  td::vector<StatItem> res;
  if (cnt_ < 2 || first_.total_ticks_ >= last_.total_ticks_) {
    res.push_back({"total_cpu", "UNKNOWN"});
    res.push_back({"user_cpu", "UNKNOWN"});
    res.push_back({"system_cpu", "UNKNOWN"});
  } else {
    auto total_ticks = last_.total_ticks_ - first_.total_ticks_;
    auto user_ticks = last_.process_user_ticks_ - first_.process_user_ticks_;
    auto system_ticks = last_.process_system_ticks_ - first_.process_system_ticks_;
    res.push_back({"total_cpu", to_percentage(user_ticks + system_ticks, total_ticks)});
    res.push_back({"user_cpu", to_percentage(user_ticks, total_ticks)});
    res.push_back({"system_cpu", to_percentage(system_ticks, total_ticks)});
  }
  return res;
}

td::vector<StatItem> ServerCpuStat::as_vector(double now) {
  std::lock_guard<std::mutex> guard(mutex_);

  td::vector<StatItem> res = stat_[0].get_stat(now).as_vector();
  for (std::size_t i = 1; i < SIZE; i++) {
    auto other = stat_[i].get_stat(now).as_vector();
    CHECK(other.size() == res.size());
    for (size_t j = 0; j < res.size(); j++) {
      res[j].value_ += "\t";
      res[j].value_ += other[j].value_;
    }
  }
  return res;
}

td::vector<td::vector<StatItem>> ServerCpuStat::as_json_ready_vector(double now) {
  std::lock_guard<std::mutex> guard(mutex_);

  td::vector<td::vector<StatItem>> res;
  auto first = stat_[0].get_stat(now).as_vector();
  auto first_size = first.size();
  res.push_back(first);
  for (std::size_t i = 1; i < SIZE; i++) {
    auto other = stat_[i].get_stat(now).as_vector();
    CHECK(other.size() == first_size);
    res.push_back(other);
  }
  return res;
}

constexpr int ServerCpuStat::DURATIONS[SIZE];
constexpr const char *ServerCpuStat::DESCR[SIZE];

void ServerBotStat::normalize(double duration) {
  if (duration == 0) {
    return;
  }
  request_count_ /= duration;
  request_bytes_ /= duration;
  request_file_count_ /= duration;
  request_files_bytes_ /= duration;
  response_count_ /= duration;
  response_count_ok_ /= duration;
  response_count_error_ /= duration;
  response_bytes_ /= duration;
  update_count_ /= duration;
}

void ServerBotStat::add(const ServerBotStat &stat) {
  request_count_ += stat.request_count_;
  request_bytes_ += stat.request_bytes_;
  request_file_count_ += stat.request_file_count_;
  request_files_bytes_ += stat.request_files_bytes_;
  request_files_max_bytes_ = td::max(request_files_max_bytes_, stat.request_files_max_bytes_);

  response_count_ += stat.response_count_;
  response_count_ok_ += stat.response_count_ok_;
  response_count_error_ += stat.response_count_error_;
  response_bytes_ += stat.response_bytes_;

  update_count_ += stat.update_count_;
}

td::vector<StatItem> ServerBotStat::as_vector() const {
  td::vector<StatItem> res;
  auto add_item = [&res](td::string name, auto value) {
    res.push_back({std::move(name), td::to_string(value)});
  };
  add_item("request_count", request_count_);
  add_item("request_bytes", request_bytes_);
  add_item("request_file_count", request_file_count_);
  add_item("request_files_bytes", request_files_bytes_);
  add_item("request_max_bytes", request_files_max_bytes_);
  add_item("response_count", response_count_);
  add_item("response_count_ok", response_count_ok_);
  add_item("response_count_error", response_count_error_);
  add_item("response_bytes", response_bytes_);
  add_item("update_count", update_count_);
  return res;
}

td::vector<StatItem> BotStatActor::as_vector(double now) {
  auto first_sd = stat_[0].stat_duration(now);
  first_sd.first.normalize(first_sd.second);
  td::vector<StatItem> res = first_sd.first.as_vector();
  for (std::size_t i = 1; i < SIZE; i++) {
    auto next_sd = stat_[i].stat_duration(now);
    next_sd.first.normalize(next_sd.second);
    auto other = next_sd.first.as_vector();
    CHECK(other.size() == res.size());
    for (size_t j = 0; j < res.size(); j++) {
      res[j].value_ += "\t";
      res[j].value_ += other[j].value_;
    }
  }
  return res;
}

td::vector<ServerBotStat> BotStatActor::as_json_ready_vector(double now) {
  std::pair<ServerBotStat, double> first_sd = stat_[0].stat_duration(now);
  first_sd.first.normalize(first_sd.second);
  td::vector<ServerBotStat> res;
  for (auto & single_stat : stat_) {
    auto next_sd = single_stat.stat_duration(now);
    next_sd.first.normalize(next_sd.second);
    res.push_back(next_sd.first);
  }
  return res;
}

td::string BotStatActor::get_description() const {
  td::string res = "DURATION";
  for (auto &descr : DESCR) {
    res += "\t";
    res += descr;
  }
  return res;
}
td::vector<td::string> BotStatActor::get_jsonable_description() const {
  td::vector<td::string> strings;
  strings.push_back("duration");
  for (auto &descr : DESCR) {
    strings.push_back(descr);
  }
  return strings;
}


bool BotStatActor::is_active(double now) const {
  return last_activity_timestamp_ > now - 86400;
}

constexpr int BotStatActor::DURATIONS[SIZE];
constexpr const char *BotStatActor::DESCR[SIZE];

}  // namespace telegram_bot_api
