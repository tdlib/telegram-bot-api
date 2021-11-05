//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2021
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "telegram-bot-api/Client.h"
#include "telegram-bot-api/Query.h"
#include "telegram-bot-api/Stats.h"

#include "td/actor/actor.h"
#include "td/actor/PromiseFuture.h"

#include "td/utils/buffer.h"
#include "td/utils/common.h"
#include "td/utils/Container.h"
#include "td/utils/FloodControlFast.h"
#include "td/utils/Slice.h"

#include <memory>
#include <unordered_map>
#include <utility>

namespace telegram_bot_api {

struct ClientParameters;
struct SharedData;

class ClientManager final : public td::Actor {
 public:
  struct TokenRange {
    td::uint64 rem;
    td::uint64 mod;
    bool operator()(td::uint64 x) {
      return x % mod == rem;
    }
  };
  ClientManager(std::shared_ptr<const ClientParameters> parameters, TokenRange token_range)
      : parameters_(std::move(parameters)), token_range_(token_range) {
  }

  void send(PromisedQueryPtr query);
  void user_login(PromisedQueryPtr query);

  bool check_flood_limits(PromisedQueryPtr &query, bool is_user_login=false);

  void get_stats(td::PromiseActor<td::BufferSlice> promise, td::vector<std::pair<td::string, td::string>> args, bool as_json);

  void close(td::Promise<td::Unit> &&promise);

 private:
  class ClientInfo {
   public:
    BotStatActor stat_;
    td::string token_;
    td::int64 tqueue_id_;
    td::ActorOwn<Client> client_;
  };
  td::Container<ClientInfo> clients_;
  BotStatActor stat_{td::ActorId<BotStatActor>()};

  std::shared_ptr<const ClientParameters> parameters_;
  TokenRange token_range_;

  std::unordered_map<td::string, td::uint64> token_to_id_;
  std::unordered_map<td::string, td::FloodControlFast> flood_controls_;
  std::unordered_map<td::int64, td::uint64> active_client_count_;

  bool close_flag_ = false;
  td::vector<td::Promise<td::Unit>> close_promises_;

  static td::int64 get_tqueue_id(td::int64 user_id, bool is_test_dc);

  static PromisedQueryPtr get_webhook_restore_query(td::Slice token, bool is_user, td::Slice webhook_info,
                                                    std::shared_ptr<SharedData> shared_data);

  void start_up() override;
  void raw_event(const td::Event::Raw &event) override;
  void hangup_shared() override;
  void close_db();
  void finish_close();
};

}  // namespace telegram_bot_api
