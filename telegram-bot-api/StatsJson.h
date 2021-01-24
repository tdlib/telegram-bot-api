//
// Copyright Luckydonald (tdlight-telegram-bot-api+code@luckydonald.de) 2020
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "telegram-bot-api/Query.h"

#include "td/db/TQueue.h"

#include "td/net/HttpOutboundConnection.h"
#include "td/net/HttpQuery.h"
#include "td/net/SslStream.h"

#include "td/actor/actor.h"
#include "td/actor/PromiseFuture.h"

#include "td/utils/common.h"
#include "td/utils/Container.h"
#include "td/utils/FloodControlFast.h"
#include "td/utils/HttpUrl.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/List.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/SocketFd.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"
#include "td/utils/VectorQueue.h"
#include "td/utils/utf8.h"

#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>

namespace telegram_bot_api {

class JsonStatsSize : public td::Jsonable {
 public:
  explicit JsonStatsSize(td::uint64 size) : size_(size) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("bytes", td::JsonLong(size_));

    // Now basically td::format::as_size(...), but without need for an StringBuilder.
    struct NamedValue {
      const char *name;
      td::uint64 value;
    };

    static constexpr NamedValue sizes[] = {{"B", 1}, {"KB", 1 << 10}, {"MB", 1 << 20}, {"GB", 1 << 30}};
    static constexpr size_t sizes_n = sizeof(sizes) / sizeof(NamedValue);

    size_t i = 0;
    while (i + 1 < sizes_n && size_ > 10 * sizes[i + 1].value) {
      i++;
    }
    object("human_readable", td::to_string(size_ / sizes[i].value) + sizes[i].name);
  }

 private:
  const td::uint64 size_;
};

class JsonStatsMem : public td::Jsonable {
 public:
  explicit JsonStatsMem(const td::MemStat mem_stat) : mem_stat_(mem_stat) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("resident_size", JsonStatsSize(mem_stat_.resident_size_));
    object("resident_size_peak", JsonStatsSize(mem_stat_.resident_size_peak_));
    object("virtual_size", JsonStatsSize(mem_stat_.virtual_size_));
    object("virtual_size_peak", JsonStatsSize(mem_stat_.virtual_size_peak_));
  }

 private:
  const td::MemStat mem_stat_;
};

class JsonStatsCpuStat : public td::Jsonable {
 public:
  explicit JsonStatsCpuStat(const StatItem& inf, const StatItem& five_sec, const StatItem& one_min, const StatItem& one_hour) :
      inf_(inf), five_sec_(five_sec), one_min_(one_min), one_hour_(one_hour) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object(td::Slice(ServerCpuStat::DESCR[0]), td::JsonString(td::Slice(inf_.value_)));
    object(td::Slice(ServerCpuStat::DESCR[1]), td::JsonString(td::Slice(five_sec_.value_)));
    object(td::Slice(ServerCpuStat::DESCR[2]), td::JsonString(td::Slice(one_min_.value_)));
    object(td::Slice(ServerCpuStat::DESCR[3]), td::JsonString(td::Slice(one_hour_.value_)));
  }
 private:
  const StatItem& inf_;
  const StatItem& five_sec_;
  const StatItem& one_min_;
  const StatItem& one_hour_;
};

class JsonStatsCpu : public td::Jsonable {
 public:
  explicit JsonStatsCpu(td::vector<td::vector<StatItem>> cpu_stats) : cpu_stats_(std::move(cpu_stats)) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    CHECK(cpu_stats_.size() == ServerCpuStat::SIZE);
    CHECK(cpu_stats_[0].size() == 3);
    CHECK(cpu_stats_[1].size() == 3);
    CHECK(cpu_stats_[2].size() == 3);
    object("total_cpu", JsonStatsCpuStat(cpu_stats_[0][0], cpu_stats_[1][0], cpu_stats_[2][0], cpu_stats_[3][0]));
    object("user_cpu", JsonStatsCpuStat(cpu_stats_[0][1], cpu_stats_[1][1], cpu_stats_[2][1], cpu_stats_[3][1]));
    object("system_cpu", JsonStatsCpuStat(cpu_stats_[0][2], cpu_stats_[1][2], cpu_stats_[2][2], cpu_stats_[3][2]));
  }

 private:
  const td::vector<td::vector<StatItem>> cpu_stats_;
};

class JsonStatsBot : public td::Jsonable {
 public:
  explicit JsonStatsBot(std::pair<td::int64, td::uint64> score_id_pair) : score_id_pair_(std::move(score_id_pair)) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("score", td::JsonLong(score_id_pair_.first));
    object("internal_id", td::JsonLong(score_id_pair_.second));
  }

 protected:
  const std::pair<td::int64, td::uint64> score_id_pair_;
};

class JsonStatsBotStatDouble : public td::Jsonable {
 public:
  explicit JsonStatsBotStatDouble(const double inf, const double five_sec, const double one_min, const double one_hour) :
      inf_(inf), five_sec_(five_sec), one_min_(one_min), one_hour_(one_hour) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object(td::Slice(BotStatActor::DESCR[0]), td::JsonFloat(inf_));
    object(td::Slice(BotStatActor::DESCR[1]), td::JsonFloat(five_sec_));
    object(td::Slice(BotStatActor::DESCR[2]), td::JsonFloat(one_min_));
    object(td::Slice(BotStatActor::DESCR[3]), td::JsonFloat(one_hour_));
  }
 private:
  const double inf_;
  const double five_sec_;
  const double one_min_;
  const double one_hour_;
};

class JsonStatsBotStatLong : public td::Jsonable {
 public:
  explicit JsonStatsBotStatLong(const td::int64 inf, const td::int64 five_sec, const td::int64 one_min, const td::int64 one_hour) :
      inf_(inf), five_sec_(five_sec), one_min_(one_min), one_hour_(one_hour) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object(td::Slice(BotStatActor::DESCR[0]), td::JsonLong(inf_));
    object(td::Slice(BotStatActor::DESCR[1]), td::JsonLong(five_sec_));
    object(td::Slice(BotStatActor::DESCR[2]), td::JsonLong(one_min_));
    object(td::Slice(BotStatActor::DESCR[3]), td::JsonLong(one_hour_));
  }
 private:
  const td::int64 inf_;
  const td::int64 five_sec_;
  const td::int64 one_min_;
  const td::int64 one_hour_;
};

class JsonStatsBotStats : public td::Jsonable {
 public:
  explicit JsonStatsBotStats(td::vector<ServerBotStat> stats) : stats_(std::move(stats)) {
    CHECK(BotStatActor::SIZE == 4 && "Check that we have 4 fields.");
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("request_count", JsonStatsBotStatDouble(stats_[0].request_count_, stats_[1].request_count_, stats_[2].request_count_, stats_[3].request_count_));
    object("request_bytes", JsonStatsBotStatDouble(stats_[0].request_bytes_, stats_[1].request_bytes_, stats_[2].request_bytes_, stats_[3].request_bytes_));
    object("request_file_count", JsonStatsBotStatDouble(stats_[0].request_file_count_, stats_[1].request_file_count_, stats_[2].request_file_count_, stats_[3].request_file_count_));
    object("request_files_bytes", JsonStatsBotStatDouble(stats_[0].request_files_bytes_, stats_[1].request_files_bytes_, stats_[2].request_files_bytes_, stats_[3].request_files_bytes_));
    object("request_files_max_bytes", JsonStatsBotStatLong(stats_[0].request_files_max_bytes_, stats_[1].request_files_max_bytes_, stats_[2].request_files_max_bytes_, stats_[3].request_files_max_bytes_));
    object("response_count", JsonStatsBotStatDouble(stats_[0].response_count_, stats_[1].response_count_, stats_[2].response_count_, stats_[3].response_count_));
    object("response_count_ok", JsonStatsBotStatDouble(stats_[0].response_count_ok_, stats_[1].response_count_ok_, stats_[2].response_count_ok_, stats_[3].response_count_ok_));
    object("response_count_error", JsonStatsBotStatDouble(stats_[0].response_count_error_, stats_[1].response_count_error_, stats_[2].response_count_error_, stats_[3].response_count_error_));
    object("response_bytes", JsonStatsBotStatDouble(stats_[0].response_bytes_, stats_[1].response_bytes_, stats_[2].response_bytes_, stats_[3].response_bytes_));
    object("update_count", JsonStatsBotStatDouble(stats_[0].update_count_, stats_[1].update_count_, stats_[2].update_count_, stats_[3].update_count_));
  }

 protected:
  const td::vector<ServerBotStat> stats_;
};

class JsonStatsBotAdvanced : public JsonStatsBot {
 public:
  explicit JsonStatsBotAdvanced(std::pair<td::int64, td::uint64> score_id_pair,
                                ServerBotInfo bot,
                                td::vector<ServerBotStat> stats,
                                const bool hide_sensible_data,
                                const double now)
      : JsonStatsBot(std::move(score_id_pair)), bot_(std::move(bot)), stats_(std::move(stats)),
        hide_sensible_data_(hide_sensible_data), now_(now) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", td::JsonLong(td::to_integer<td::int64>(bot_.id_)));
    object("uptime", now_ - bot_.start_time_);
    object("score", td::JsonLong(score_id_pair_.first));
    object("internal_id", td::JsonLong(score_id_pair_.second));
    if (!hide_sensible_data_) {
      object("token", td::JsonString(bot_.token_));
    }
    object("username", bot_.username_);
    td::CSlice url = bot_.webhook_;
    object("webhook_set", td::JsonBool(!url.empty()));
    if (!hide_sensible_data_) {
      if (td::check_utf8(url)) {
        object("webhook_url", url);
      } else {
        object("webhook_url", td::JsonRawString(url));
      }
    }

    object("has_custom_certificate", td::JsonBool(bot_.has_webhook_certificate_));
    object("head_update_id", td::JsonInt(bot_.head_update_id_));
    object("tail_update_id", td::JsonInt(bot_.tail_update_id_));
    object("pending_update_count", td::narrow_cast<td::int32>(bot_.pending_update_count_));
    object("webhook_max_connections", td::JsonInt(bot_.webhook_max_connections_));
    object("stats", JsonStatsBotStats(std::move(stats_)));
  }
 private:
  ServerBotInfo bot_;
  td::vector<ServerBotStat> stats_;
  const bool hide_sensible_data_;
  const double now_;
};

class JsonStatsBots : public td::Jsonable {
 public:
  JsonStatsBots(td::vector<JsonStatsBotAdvanced> bots, bool no_metadata)
      : bots_(std::move(bots)), no_metadata_(no_metadata) {
  }
  void store(td::JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (const auto &bot : bots_) {
      if (no_metadata_) {
        array << static_cast<const JsonStatsBot &>(bot);
      } else {
        array << bot;
      }
    }
  }

 private:
  const td::vector<JsonStatsBotAdvanced> bots_;
  bool no_metadata_;
};

}  // namespace telegram_bot_api
