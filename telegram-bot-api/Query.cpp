//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2024
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/Query.h"

#include "telegram-bot-api/Stats.h"

#include "td/actor/actor.h"

#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/Time.h"

#include <numeric>

namespace telegram_bot_api {

td::FlatHashMap<td::string, td::unique_ptr<td::VirtuallyJsonable>> empty_parameters;

Query::Query(td::vector<td::BufferSlice> &&container, td::Slice token, bool is_test_dc, td::MutableSlice method,
             td::vector<std::pair<td::MutableSlice, td::MutableSlice>> &&args,
             td::vector<std::pair<td::MutableSlice, td::MutableSlice>> &&headers, td::vector<td::HttpFile> &&files,
             std::shared_ptr<SharedData> shared_data, const td::IPAddress &peer_ip_address, bool is_internal)
    : state_(State::Query)
    , shared_data_(shared_data)
    , peer_ip_address_(peer_ip_address)
    , container_(std::move(container))
    , token_(token)
    , is_test_dc_(is_test_dc)
    , method_(method)
    , args_(std::move(args))
    , headers_(std::move(headers))
    , files_(std::move(files))
    , is_internal_(is_internal) {
  if (method_.empty()) {
    method_ = arg("method");
  }
  td::to_lower_inplace(method_);
  start_timestamp_ = td::Time::now();
  LOG(INFO) << "Query " << this << ": " << *this;
  if (shared_data_) {
    shared_data_->query_count_.fetch_add(1, std::memory_order_relaxed);
    if (method_ != "getupdates") {
      shared_data_->query_list_size_.fetch_add(1, std::memory_order_relaxed);
      shared_data_->query_list_.put(this);
    }
  }
}

td::string Query::get_peer_ip_address() const {
  if (peer_ip_address_.is_valid() && !peer_ip_address_.is_reserved()) {  // external connection
    return peer_ip_address_.get_ip_str().str();
  } else {
    // invalid peer IP address or connection from the local network
    return get_header("x-real-ip").str();
  }
}

td::int64 Query::query_size() const {
  return std::accumulate(
      container_.begin(), container_.end(), td::int64{0},
      [](td::int64 acc, const td::BufferSlice &slice) { return static_cast<td::int64>(acc + slice.size()); });
}

td::int64 Query::files_size() const {
  return std::accumulate(files_.begin(), files_.end(), td::int64{0},
                         [](td::int64 acc, const td::HttpFile &file) { return acc + file.size; });
}

td::int64 Query::files_max_size() const {
  return std::accumulate(files_.begin(), files_.end(), td::int64{0},
                         [](td::int64 acc, const td::HttpFile &file) { return td::max(acc, file.size); });
}

void Query::set_stat_actor(td::ActorId<BotStatActor> stat_actor) {
  stat_actor_ = stat_actor;
  send_request_stat();
}

void Query::set_ok(td::BufferSlice result) {
  CHECK(state_ == State::Query);
  LOG(INFO) << "Query " << this << ": " << td::tag("method", method_) << td::tag("text", result.as_slice());
  answer_ = std::move(result);
  state_ = State::OK;
  http_status_code_ = 200;
  send_response_stat();
}

void Query::set_error(int http_status_code, td::BufferSlice result) {
  LOG(INFO) << "Query " << this << ": " << td::tag("method", method_) << td::tag("code", http_status_code)
            << td::tag("text", result.as_slice());
  CHECK(state_ == State::Query);
  answer_ = std::move(result);
  state_ = State::Error;
  http_status_code_ = http_status_code;
  send_response_stat();
}

void Query::set_retry_after_error(int retry_after) {
  retry_after_ = retry_after;

  td::FlatHashMap<td::string, td::unique_ptr<td::VirtuallyJsonable>> parameters;
  parameters.emplace("retry_after", td::make_unique<td::VirtuallyJsonableLong>(retry_after));
  set_error(429, td::json_encode<td::BufferSlice>(
                     JsonQueryError(429, PSLICE() << "Too Many Requests: retry after " << retry_after, parameters)));
}

td::StringBuilder &operator<<(td::StringBuilder &sb, const Query &query) {
  auto padded_time =
      td::lpad(PSTRING() << td::format::as_time(td::Time::now_cached() - query.start_timestamp()), 10, ' ');
  sb << "[bot" << td::rpad(query.token().str(), 46, ' ') << "][time:" << padded_time << ']'
     << td::tag("method", td::lpad(query.method().str(), 25, ' '));
  if (!query.args().empty()) {
    sb << '{';
    for (const auto &arg : query.args()) {
      sb << '[';
      if (arg.first.size() > 128) {
        sb << '<' << arg.first.size() << '>' << td::oneline(arg.first.substr(0, 128)) << "...";
      } else {
        sb << td::oneline(arg.first);
      }
      sb << ':';
      if (arg.second.size() > 4096) {
        sb << '<' << arg.second.size() << '>' << td::oneline(arg.second.substr(0, 4096)) << "...";
      } else {
        sb << td::oneline(arg.second);
      }
      sb << ']';
    }
    sb << '}';
  }
  if (!query.files().empty()) {
    sb << query.files();
  }
  return sb;
}

void Query::send_request_stat() const {
  if (stat_actor_.empty()) {
    return;
  }
  send_closure(stat_actor_, &BotStatActor::add_event<ServerBotStat::Request>,
               ServerBotStat::Request{query_size(), file_count(), files_size(), files_max_size()}, td::Time::now());
}

void Query::send_response_stat() const {
  auto now = td::Time::now();
  if (now - start_timestamp_ >= 100.0 && !is_internal_) {
    LOG(WARNING) << "Answer too old query with code " << http_status_code_ << " and answer size " << answer_.size()
                 << ": " << *this;
  }

  if (stat_actor_.empty()) {
    return;
  }
  send_closure(stat_actor_, &BotStatActor::add_event<ServerBotStat::Response>,
               ServerBotStat::Response{state_ == State::OK, answer_.size(), file_count(), files_size()}, now);
}

}  // namespace telegram_bot_api
