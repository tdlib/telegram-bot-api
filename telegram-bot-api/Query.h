//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2024
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "telegram-bot-api/ClientParameters.h"

#include "td/net/HttpFile.h"

#include "td/actor/actor.h"

#include "td/utils/buffer.h"
#include "td/utils/common.h"
#include "td/utils/FlatHashMap.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/List.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/Promise.h"
#include "td/utils/Slice.h"
#include "td/utils/StringBuilder.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

namespace telegram_bot_api {

class BotStatActor;

class Query final : public td::ListNode {
 public:
  enum class State : td::int8 { Query, OK, Error };

  td::Slice token() const {
    return token_;
  }

  bool is_test_dc() const {
    return is_test_dc_;
  }

  td::Slice method() const {
    return method_;
  }

  bool has_arg(td::Slice key) const {
    auto it = std::find_if(args_.begin(), args_.end(),
                           [&key](const std::pair<td::MutableSlice, td::MutableSlice> &s) { return s.first == key; });
    return it != args_.end();
  }

  td::MutableSlice arg(td::Slice key) const {
    auto it = std::find_if(args_.begin(), args_.end(),
                           [&key](const std::pair<td::MutableSlice, td::MutableSlice> &s) { return s.first == key; });
    return it == args_.end() ? td::MutableSlice() : it->second;
  }

  const td::vector<std::pair<td::MutableSlice, td::MutableSlice>> &args() const {
    return args_;
  }

  td::Slice get_header(td::Slice key) const {
    auto it = std::find_if(headers_.begin(), headers_.end(),
                           [&key](const std::pair<td::MutableSlice, td::MutableSlice> &s) { return s.first == key; });
    return it == headers_.end() ? td::Slice() : it->second;
  }

  const td::HttpFile *file(td::Slice key) const {
    auto it = std::find_if(files_.begin(), files_.end(), [&key](const td::HttpFile &f) { return f.field_name == key; });
    return it == files_.end() ? nullptr : &*it;
  }

  const td::vector<td::HttpFile> &files() const {
    return files_;
  }

  td::int64 files_size() const;

  td::string get_peer_ip_address() const;

  td::BufferSlice &answer() {
    return answer_;
  }

  int http_status_code() const {
    return http_status_code_;
  }

  int retry_after() const {
    return retry_after_;
  }

  void set_ok(td::BufferSlice result);

  void set_error(int http_status_code, td::BufferSlice result);

  void set_retry_after_error(int retry_after);

  bool is_ready() const {
    return state_ != State::Query;
  }

  bool is_internal() const {
    return is_internal_;
  }

  Query(td::vector<td::BufferSlice> &&container, td::Slice token, bool is_test_dc, td::MutableSlice method,
        td::vector<std::pair<td::MutableSlice, td::MutableSlice>> &&args,
        td::vector<std::pair<td::MutableSlice, td::MutableSlice>> &&headers, td::vector<td::HttpFile> &&files,
        std::shared_ptr<SharedData> shared_data, const td::IPAddress &peer_ip_address, bool is_internal);
  Query(const Query &) = delete;
  Query &operator=(const Query &) = delete;
  Query(Query &&) = delete;
  Query &operator=(Query &&) = delete;
  ~Query() {
    if (shared_data_) {
      shared_data_->query_count_.fetch_sub(1, std::memory_order_relaxed);
      if (!empty()) {
        shared_data_->query_list_size_.fetch_sub(1, std::memory_order_relaxed);
      }
      td::Scheduler::instance()->destroy_on_scheduler(SharedData::get_file_gc_scheduler_id(), container_, args_,
                                                      headers_, files_, answer_);
    }
  }

  double start_timestamp() const {
    return start_timestamp_;
  }

  void set_stat_actor(td::ActorId<BotStatActor> stat_actor);

 private:
  State state_;
  std::shared_ptr<SharedData> shared_data_;
  double start_timestamp_;
  td::IPAddress peer_ip_address_;
  td::ActorId<BotStatActor> stat_actor_;

  // request
  td::vector<td::BufferSlice> container_;
  td::Slice token_;
  bool is_test_dc_;
  td::MutableSlice method_;
  td::vector<std::pair<td::MutableSlice, td::MutableSlice>> args_;
  td::vector<std::pair<td::MutableSlice, td::MutableSlice>> headers_;
  td::vector<td::HttpFile> files_;
  bool is_internal_ = false;

  // response
  td::BufferSlice answer_;
  int http_status_code_ = 0;
  int retry_after_ = 0;

  // for stats
  td::int32 file_count() const {
    return static_cast<td::int32>(files_.size());
  }

  td::int64 query_size() const;

  td::int64 files_max_size() const;

  void send_request_stat() const;

  void send_response_stat() const;
};

td::StringBuilder &operator<<(td::StringBuilder &sb, const Query &query);

// fix for outdated C++14 libraries
// https://stackoverflow.com/questions/26947704/implicit-conversion-failure-from-initializer-list
extern td::FlatHashMap<td::string, td::unique_ptr<td::VirtuallyJsonable>> empty_parameters;

class JsonParameters final : public td::Jsonable {
 public:
  explicit JsonParameters(const td::FlatHashMap<td::string, td::unique_ptr<td::VirtuallyJsonable>> &parameters)
      : parameters_(parameters) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    for (auto &parameter : parameters_) {
      CHECK(parameter.second != nullptr);
      object(parameter.first, *parameter.second);
    }
  }

 private:
  const td::FlatHashMap<td::string, td::unique_ptr<td::VirtuallyJsonable>> &parameters_;
};

template <class T>
class JsonQueryOk final : public td::Jsonable {
 public:
  JsonQueryOk(const T &result, td::Slice description) : result_(result), description_(description) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("ok", td::JsonTrue());
    object("result", result_);
    if (!description_.empty()) {
      object("description", description_);
    }
  }

 private:
  const T &result_;
  td::Slice description_;
};

class JsonQueryError final : public td::Jsonable {
 public:
  JsonQueryError(
      int error_code, td::Slice description,
      const td::FlatHashMap<td::string, td::unique_ptr<td::VirtuallyJsonable>> &parameters = empty_parameters)
      : error_code_(error_code), description_(description), parameters_(parameters) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("ok", td::JsonFalse());
    object("error_code", error_code_);
    object("description", description_);
    if (!parameters_.empty()) {
      object("parameters", JsonParameters(parameters_));
    }
  }

 private:
  int error_code_;
  td::Slice description_;
  const td::FlatHashMap<td::string, td::unique_ptr<td::VirtuallyJsonable>> &parameters_;
};

class PromiseDeleter {
 public:
  explicit PromiseDeleter(td::Promise<td::unique_ptr<Query>> &&promise) : promise_(std::move(promise)) {
  }
  PromiseDeleter() = default;
  PromiseDeleter(const PromiseDeleter &) = delete;
  PromiseDeleter &operator=(const PromiseDeleter &) = delete;
  PromiseDeleter(PromiseDeleter &&) = default;
  PromiseDeleter &operator=(PromiseDeleter &&) = default;
  void operator()(Query *raw_ptr) {
    td::unique_ptr<Query> query(raw_ptr);  // now I cannot forget to delete this pointer
    if (promise_) {
      if (!query->is_ready()) {
        query->set_retry_after_error(5);
      }

      promise_.set_value(std::move(query));
    }
  }
  ~PromiseDeleter() {
    CHECK(!promise_);
  }

 private:
  td::Promise<td::unique_ptr<Query>> promise_;
};
using PromisedQueryPtr = std::unique_ptr<Query, PromiseDeleter>;

template <class Jsonable>
void answer_query(const Jsonable &result, PromisedQueryPtr query, td::Slice description = td::Slice()) {
  query->set_ok(td::json_encode<td::BufferSlice>(JsonQueryOk<Jsonable>(result, description)));
  query.reset();  // send query into promise explicitly
}

inline void fail_query(
    int http_status_code, td::Slice description, PromisedQueryPtr query,
    const td::FlatHashMap<td::string, td::unique_ptr<td::VirtuallyJsonable>> &parameters = empty_parameters) {
  query->set_error(http_status_code,
                   td::json_encode<td::BufferSlice>(JsonQueryError(http_status_code, description, parameters)));
  query.reset();  // send query into promise explicitly
}

}  // namespace telegram_bot_api
