//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2021
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/ClientManager.h"
#include "telegram-bot-api/ClientParameters.h"
#include "telegram-bot-api/HttpConnection.h"
#include "telegram-bot-api/HttpServer.h"
#include "telegram-bot-api/HttpStatConnection.h"
#include "telegram-bot-api/Query.h"
#include "telegram-bot-api/Stats.h"

#include "td/telegram/ClientActor.h"

#include "td/db/binlog/Binlog.h"
#include "td/db/TQueue.h"

#include "td/net/GetHostByNameActor.h"
#include "td/net/HttpInboundConnection.h"

#include "td/actor/actor.h"
#include "td/actor/ConcurrentScheduler.h"
#include "td/actor/PromiseFuture.h"

#include "td/utils/buffer.h"
#include "td/utils/CombinedLog.h"
#include "td/utils/common.h"
#include "td/utils/crypto.h"
#include "td/utils/ExitGuard.h"
#include "td/utils/FileLog.h"
#include "td/utils/format.h"
//#include "td/utils/GitInfo.h"
#include "td/utils/logging.h"
#include "td/utils/MemoryLog.h"
#include "td/utils/misc.h"
#include "td/utils/OptionParser.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/path.h"
#include "td/utils/port/rlimit.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/stacktrace.h"
#include "td/utils/port/Stat.h"
#include "td/utils/port/user.h"
#include "td/utils/Slice.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/Status.h"
#include "td/utils/Time.h"
#include "td/utils/TsLog.h"

#include "memprof/memprof.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <tuple>

namespace telegram_bot_api {

static std::atomic_flag need_reopen_log;

static void after_log_rotation_signal_handler(int sig) {
  need_reopen_log.clear();
}

static std::atomic_flag need_quit;

static void quit_signal_handler(int sig) {
  need_quit.clear();
}

static td::MemoryLog<1 << 20> memory_log;

void print_log() {
  auto buf = memory_log.get_buffer();
  auto pos = memory_log.get_pos();
  td::signal_safe_write("------- Log dump -------\n");
  td::signal_safe_write(buf.substr(pos), false);
  td::signal_safe_write(buf.substr(0, pos), false);
  td::signal_safe_write("\n", false);
  td::signal_safe_write("------------------------\n");
}

static void fail_signal_handler(int sig) {
  td::signal_safe_write_signal_number(sig);
  td::Stacktrace::PrintOptions options;
  options.use_gdb = true;
  td::Stacktrace::print_to_stderr(options);
  print_log();
  _Exit(EXIT_FAILURE);
}

static std::atomic_flag need_change_verbosity_level;

static void change_verbosity_level_signal_handler(int sig) {
  need_change_verbosity_level.clear();
}

static std::atomic_flag need_dump_log;

static void dump_log_signal_handler(int sig) {
  need_dump_log.clear();
}

static void sigsegv_signal_handler(int signum, void *addr) {
  td::signal_safe_write_pointer(addr);
  fail_signal_handler(signum);
}

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL));
  td::ExitGuard exit_guard;

  need_reopen_log.test_and_set();
  need_quit.test_and_set();
  need_change_verbosity_level.test_and_set();
  need_dump_log.test_and_set();

  td::Stacktrace::init();

  td::setup_signals_alt_stack().ensure();
  td::set_signal_handler(td::SignalType::User, after_log_rotation_signal_handler).ensure();
  td::ignore_signal(td::SignalType::HangUp).ensure();
  td::ignore_signal(td::SignalType::Pipe).ensure();
  td::set_signal_handler(td::SignalType::Quit, quit_signal_handler).ensure();
  td::set_signal_handler(td::SignalType::Abort, fail_signal_handler).ensure();
  td::set_signal_handler(td::SignalType::Other, fail_signal_handler).ensure();
  td::set_extended_signal_handler(td::SignalType::Error, sigsegv_signal_handler).ensure();

  td::set_runtime_signal_handler(0, change_verbosity_level_signal_handler).ensure();
  td::set_runtime_signal_handler(1, dump_log_signal_handler).ensure();

  td::init_openssl_threads();

  auto start_time = td::Time::now();
  auto shared_data = std::make_shared<SharedData>();
  auto parameters = std::make_unique<ClientParameters>();
  parameters->version_ = "5.2";
  parameters->shared_data_ = shared_data;
  parameters->start_time_ = start_time;
  auto net_query_stats = td::create_net_query_stats();
  parameters->net_query_stats_ = net_query_stats;

  td::OptionParser options;
  bool need_print_usage = false;
  bool need_print_version = false;
  int http_port = 8081;
  int http_stat_port = 0;
  td::string http_ip_address = "0.0.0.0";
  td::string http_stat_ip_address = "0.0.0.0";
  td::string log_file_path;
  int default_verbosity_level = 0;
  int memory_verbosity_level = VERBOSITY_NAME(INFO);
  td::int64 log_max_file_size = 2000000000;
  td::string working_directory;
  td::string temporary_directory;
  td::string username;
  td::string groupname;
  td::uint64 max_connections = 0;
  ClientManager::TokenRange token_range{0, 1};

  parameters->api_id_ = [](auto x) -> td::int32 {
    if (x) {
      return td::to_integer<td::int32>(td::Slice(x));
    }
    return 0;
  }(std::getenv("TELEGRAM_API_ID"));
  parameters->api_hash_ = [](auto x) -> std::string {
    if (x) {
      return x;
    }
    return std::string();
  }(std::getenv("TELEGRAM_API_HASH"));

  options.set_usage(td::Slice(argv[0]), "--api-id=<arg> --api-hash=<arg> [--local] [OPTION]...");
  options.set_description("Telegram Bot API server");
  options.add_option('h', "help", "display this help text and exit", [&] { need_print_usage = true; });
  options.add_option('\0', "version", "display version number and exit", [&] { need_print_version = true; });
  options.add_option('\0', "local", "allow the Bot API server to serve local requests",
                     [&] { parameters->local_mode_ = true; });
  options.add_checked_option(
      '\0', "api-id",
      "application identifier for Telegram API access, which can be obtained at https://my.telegram.org (defaults to "
      "the value of the TELEGRAM_API_ID environment variable)",
      td::OptionParser::parse_integer(parameters->api_id_));
  options.add_option('\0', "api-hash",
                     "application identifier hash for Telegram API access, which can be obtained at "
                     "https://my.telegram.org (defaults to the value of the TELEGRAM_API_HASH environment variable)",
                     td::OptionParser::parse_string(parameters->api_hash_));
  options.add_checked_option('p', "http-port", PSLICE() << "HTTP listening port (default is " << http_port << ")",
                             td::OptionParser::parse_integer(http_port));
  options.add_checked_option('s', "http-stat-port", "HTTP statistics port",
                             td::OptionParser::parse_integer(http_stat_port));
  options.add_option('d', "dir", "server working directory", td::OptionParser::parse_string(working_directory));
  options.add_option('t', "temp-dir", "directory for storing HTTP server temporary files",
                     td::OptionParser::parse_string(temporary_directory));
  options.add_checked_option('\0', "filter",
                             "\"<remainder>/<modulo>\". Allow only bots with 'bot_user_id % modulo == remainder'",
                             [&](td::Slice rem_mod) {
                               td::Slice rem;
                               td::Slice mod;
                               std::tie(rem, mod) = td::split(rem_mod, '/');
                               TRY_RESULT(rem_i, td::to_integer_safe<td::uint64>(rem));
                               TRY_RESULT(mod_i, td::to_integer_safe<td::uint64>(mod));
                               if (rem_i >= mod_i) {
                                 return td::Status::Error("Wrong argument specified: ensure that remainder < modulo");
                               }
                               token_range = {rem_i, mod_i};
                               return td::Status::OK();
                             });
  options.add_checked_option('\0', "max-webhook-connections",
                             "default value of the maximum webhook connections per bot",
                             td::OptionParser::parse_integer(parameters->default_max_webhook_connections_));
  options.add_checked_option('\0', "http-ip-address",
                             "local IP address, HTTP connections to which will be accepted. By default, connections to "
                             "any local IPv4 address are accepted",
                             [&](td::Slice ip_address) {
                               TRY_STATUS(td::IPAddress::get_ip_address(ip_address.str()));
                               http_ip_address = ip_address.str();
                               return td::Status::OK();
                             });
  options.add_checked_option('\0', "http-stat-ip-address",
                             "local IP address, HTTP statistics connections to which will be accepted. By default, "
                             "statistics connections to any local IPv4 address are accepted",
                             [&](td::Slice ip_address) {
                               TRY_STATUS(td::IPAddress::get_ip_address(ip_address.str()));
                               http_stat_ip_address = ip_address.str();
                               return td::Status::OK();
                             });

  options.add_option('l', "log", "path to the file where the log will be written",
                     td::OptionParser::parse_string(log_file_path));
  options.add_checked_option('v', "verbosity", "log verbosity level",
                             td::OptionParser::parse_integer(default_verbosity_level));
  options.add_checked_option('\0', "memory-verbosity", "memory log verbosity level; defaults to 3",
                             td::OptionParser::parse_integer(memory_verbosity_level));
  options.add_checked_option(
      '\0', "log-max-file-size",
      PSLICE() << "maximum size of the log file in bytes before it will be auto-rotated (default is "
               << log_max_file_size << ")",
      td::OptionParser::parse_integer(log_max_file_size));

  options.add_option('u', "username", "effective user name to switch to", td::OptionParser::parse_string(username));
  options.add_option('g', "groupname", "effective group name to switch to", td::OptionParser::parse_string(groupname));
  options.add_checked_option('c', "max-connections", "maximum number of open file descriptors",
                             td::OptionParser::parse_integer(max_connections));

  options.add_checked_option(
      '\0', "proxy", PSLICE() << "HTTP proxy server for outgoing webhook requests in the format http://host:port",
      [&](td::Slice address) {
        if (td::begins_with(address, "http://")) {
          address.remove_prefix(7);
        } else if (td::begins_with(address, "https://")) {
          address.remove_prefix(8);
        }
        return parameters->webhook_proxy_ip_address_.init_host_port(address.str());
      });
  options.add_check([&] {
    if (parameters->api_id_ <= 0 || parameters->api_hash_.empty()) {
      return td::Status::Error("You must provide valid api-id and api-hash obtained at https://my.telegram.org");
    }
    return td::Status::OK();
  });
  options.add_check([&] {
    if (default_verbosity_level < 0) {
      return td::Status::Error("Wrong verbosity level specified");
    }
    return td::Status::OK();
  });
  options.add_check([&] {
    if (memory_verbosity_level < 0) {
      return td::Status::Error("Wrong memory verbosity level specified");
    }
    return td::Status::OK();
  });
  auto r_non_options = options.run(argc, argv, 0);
  if (need_print_usage) {
    LOG(PLAIN) << options;
    return 0;
  }
  if (need_print_version) {
    LOG(PLAIN) << "Bot API " << parameters->version_;
    return 0;
  }
  if (r_non_options.is_error()) {
    LOG(PLAIN) << argv[0] << ": " << r_non_options.error();
    LOG(PLAIN) << options;
    return 1;
  }

  td::CombinedLog log;
  log.set_first(td::default_log_interface);
  log.set_second(&memory_log);
  td::log_interface = &log;

  td::FileLog file_log;
  td::TsLog ts_log(&file_log);

  auto init_status = [&] {
    if (max_connections != 0) {
      TRY_STATUS_PREFIX(td::set_resource_limit(td::ResourceLimitType::NoFile, max_connections),
                        "Can't set file descriptor limit: ");
    }

    if (!username.empty()) {
      TRY_STATUS_PREFIX(td::change_user(username, groupname), "Can't change effective user: ");
    }

    if (!working_directory.empty()) {
      TRY_STATUS_PREFIX(td::chdir(working_directory), "Can't set working directory: ");
    }

    if (!temporary_directory.empty()) {
      TRY_STATUS_PREFIX(td::set_temporary_dir(temporary_directory), "Can't set temporary directory: ");
    }

    if (!log_file_path.empty()) {
      TRY_STATUS_PREFIX(file_log.init(log_file_path, log_max_file_size), "Can't open log file: ");
      log.set_first(&ts_log);
    }

    return td::Status::OK();
  }();
  if (init_status.is_error()) {
    LOG(PLAIN) << init_status.error();
    LOG(PLAIN) << options;
    return 1;
  }

  if (parameters->default_max_webhook_connections_ <= 0) {
    parameters->default_max_webhook_connections_ = parameters->local_mode_ ? 100 : 40;
  }

  ::td::VERBOSITY_NAME(dns_resolver) = VERBOSITY_NAME(WARNING);

  log.set_second_verbosity_level(memory_verbosity_level);

  auto set_verbosity_level = [&log, memory_verbosity_level](int new_verbosity_level) {
    SET_VERBOSITY_LEVEL(td::max(memory_verbosity_level, new_verbosity_level));
    log.set_first_verbosity_level(new_verbosity_level);
  };
  set_verbosity_level(default_verbosity_level);

  // LOG(WARNING) << "Bot API server with commit " << td::GitInfo::commit() << ' '
  //              << (td::GitInfo::is_dirty() ? "(dirty)" : "") << " started";
  LOG(WARNING) << "Bot API " << parameters->version_ << " server started";

  const int threads_n = 5;  // +3 for Td, one for slow HTTP connections and one for DNS resolving
  td::ConcurrentScheduler sched;
  sched.init(threads_n);

  td::GetHostByNameActor::Options get_host_by_name_options;
  get_host_by_name_options.scheduler_id = threads_n;
  parameters->get_host_by_name_actor_id_ =
      sched.create_actor_unsafe<td::GetHostByNameActor>(0, "GetHostByName", std::move(get_host_by_name_options))
          .release();

  auto client_manager =
      sched.create_actor_unsafe<ClientManager>(0, "ClientManager", std::move(parameters), token_range).release();
  sched
      .create_actor_unsafe<HttpServer>(
          0, "HttpServer", http_ip_address, http_port,
          [client_manager, shared_data] {
            return td::ActorOwn<td::HttpInboundConnection::Callback>(
                td::create_actor<HttpConnection>("HttpConnection", client_manager, shared_data));
          })
      .release();
  if (http_stat_port != 0) {
    sched
        .create_actor_unsafe<HttpServer>(
            0, "HttpStatsServer", http_stat_ip_address, http_stat_port,
            [client_manager] {
              return td::ActorOwn<td::HttpInboundConnection::Callback>(
                  td::create_actor<HttpStatConnection>("HttpStatConnection", client_manager));
            })
        .release();
  }
  sched.start();

  double next_cron_time = start_time;
  double last_dump_time = start_time - 1000.0;
  double last_tqueue_gc_time = start_time - 1000.0;
  td::int64 tqueue_deleted_events = 0;
  td::int64 last_tqueue_deleted_events = 0;
  bool close_flag = false;
  std::atomic_bool can_quit{false};
  ServerCpuStat::instance();  // create ServerCpuStat instance
  while (true) {
    sched.run_main(next_cron_time - td::Time::now());

    if (!need_reopen_log.test_and_set()) {
      td::log_interface->after_rotation();
    }

    if (!need_quit.test_and_set()) {
      if (close_flag) {
        LOG(WARNING) << "Receive stop signal again. Exit immediately...";
        std::_Exit(0);
      }

      LOG(WARNING) << "Stopping engine with uptime " << (td::Time::now() - start_time) << " seconds by a signal";
      last_dump_time = td::Time::now() - 1e6;
      close_flag = true;
      auto guard = sched.get_main_guard();
      send_closure(client_manager, &ClientManager::close, td::PromiseCreator::lambda([&can_quit](td::Unit) {
                     can_quit.store(true);
                     td::Scheduler::instance()->yield();
                   }));
    }
    if (can_quit.exchange(false)) {
      break;
    }

    if (!need_change_verbosity_level.test_and_set()) {
      if (log.get_first_verbosity_level() == default_verbosity_level) {
        // increase default log verbosity level
        set_verbosity_level(100);
      } else {
        // return back verbosity level
        set_verbosity_level(default_verbosity_level);
      }
    }

    auto next_verbosity_level = shared_data->next_verbosity_level_.exchange(-1);
    if (next_verbosity_level != -1) {
      set_verbosity_level(next_verbosity_level);
    }

    if (!need_dump_log.test_and_set()) {
      print_log();
    }

    double now = td::Time::now();
    if (now >= next_cron_time) {
      if (now >= next_cron_time + 1.0) {
        next_cron_time = now;
      }
      next_cron_time += 1.0;
      ServerCpuStat::update(now);
    }

    if (now > last_tqueue_gc_time + 60.0) {
      auto unix_time = shared_data->get_unix_time(now);
      LOG(INFO) << "Run TQueue GC at " << unix_time;
      last_tqueue_gc_time = now;
      auto guard = sched.get_main_guard();
      auto deleted_events = shared_data->tqueue_->run_gc(unix_time);
      LOG(INFO) << "TQueue GC deleted " << deleted_events << " events";

      tqueue_deleted_events += deleted_events;
      if (tqueue_deleted_events > last_tqueue_deleted_events + 10000) {
        LOG(WARNING) << "TQueue GC already deleted " << tqueue_deleted_events << " events since the start";
        last_tqueue_deleted_events = tqueue_deleted_events;
      }
    }

    if (now > last_dump_time + 300.0) {
      last_dump_time = now;
      if (is_memprof_on()) {
        LOG(WARNING) << "Memory dump:";
        td::vector<AllocInfo> v;
        dump_alloc([&](const AllocInfo &info) { v.push_back(info); });
        std::sort(v.begin(), v.end(), [](const AllocInfo &a, const AllocInfo &b) { return a.size > b.size; });
        size_t total_size = 0;
        size_t other_size = 0;
        int count = 0;
        for (auto &info : v) {
          if (count++ < 50) {
            LOG(WARNING) << td::format::as_size(info.size) << td::format::as_array(info.backtrace);
          } else {
            other_size += info.size;
          }
          total_size += info.size;
        }
        LOG(WARNING) << td::tag("other", td::format::as_size(other_size));
        LOG(WARNING) << td::tag("total size", td::format::as_size(total_size));
        LOG(WARNING) << td::tag("total traces", get_ht_size());
        LOG(WARNING) << td::tag("fast_backtrace_success_rate", get_fast_backtrace_success_rate());
      }
      auto r_mem_stat = td::mem_stat();
      if (r_mem_stat.is_ok()) {
        auto mem_stat = r_mem_stat.move_as_ok();
        LOG(WARNING) << td::tag("rss", td::format::as_size(mem_stat.resident_size_));
        LOG(WARNING) << td::tag("vm", td::format::as_size(mem_stat.virtual_size_));
        LOG(WARNING) << td::tag("rss_peak", td::format::as_size(mem_stat.resident_size_peak_));
        LOG(WARNING) << td::tag("vm_peak", td::format::as_size(mem_stat.virtual_size_peak_));
      }
      LOG(WARNING) << td::tag("buffer_mem", td::format::as_size(td::BufferAllocator::get_buffer_mem()));
      LOG(WARNING) << td::tag("buffer_slice_size", td::format::as_size(td::BufferAllocator::get_buffer_slice_size()));

      auto query_count = shared_data->query_count_.load();
      LOG(WARNING) << td::tag("pending queries", query_count);

      td::uint64 i = 0;
      bool was_gap = false;
      for (auto end = &shared_data->query_list_, cur = end->prev; cur != end; cur = cur->prev, i++) {
        if (i < 20 || i > query_count - 20 || i % (query_count / 50 + 1) == 0) {
          if (was_gap) {
            LOG(WARNING) << "...";
            was_gap = false;
          }
          LOG(WARNING) << static_cast<Query &>(*cur);
        } else {
          was_gap = true;
        }
      }

      td::dump_pending_network_queries(*net_query_stats);
    }
  }

  LOG(WARNING) << "--------------------FINISH ENGINE--------------------";
  CHECK(net_query_stats.use_count() == 1);
  net_query_stats = nullptr;
  sched.finish();
  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL));
  td::log_interface = td::default_log_interface;
  return 0;
}

}  // namespace telegram_bot_api

int main(int argc, char *argv[]) {
  return telegram_bot_api::main(argc, argv);
}
