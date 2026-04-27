#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>

#include <hiredis/hiredis.h>

// ─────────────────────────────────────────────────────────────────
//  RedisSubscriber
//  Owns one blocking SUBSCRIBE connection on a background thread.
//  Calls cb(message_bytes) for every message on the channel,
//  except messages whose session_id matches own_session_id.
// ─────────────────────────────────────────────────────────────────
class RedisSubscriber {
public:
  RedisSubscriber(const std::string& host, int port,
                  const std::string& channel,
                  const std::string& own_session_id,
                  std::function<void(const std::string&)> cb);
  ~RedisSubscriber();

  RedisSubscriber(const RedisSubscriber&) = delete;
  RedisSubscriber& operator=(const RedisSubscriber&) = delete;

private:
  redisContext*     ctx_  = nullptr;
  std::thread       thread_;
  // Isolated on its own cache line: writer sets stop_=true, subscriber
  // thread reads it in a tight loop — false sharing would cause line
  // invalidation on every iteration across both cores.
  alignas(64) std::atomic<bool> stop_{ false };
};

// ─────────────────────────────────────────────────────────────────
//  RedisStore
//  Thread-safe command client (SET/GET/DEL/PUBLISH + OT Lua script).
//  One shared command connection; subscribe() creates a new connection
//  per subscriber (hiredis subscribe mode blocks the connection).
// ─────────────────────────────────────────────────────────────────
class RedisStore {
public:
  RedisStore(const std::string& host, int port);
  ~RedisStore();

  RedisStore(const RedisStore&) = delete;
  RedisStore& operator=(const RedisStore&) = delete;

  bool ok() const;

  // ── General KV ──────────────────────────────────────────────
  bool                     set(const std::string& key,
                                const std::string& val,
                                int ttl_seconds = 0);
  std::optional<std::string> get(const std::string& key) const;
  bool                     del(const std::string& key);

  // ── Pub/Sub ──────────────────────────────────────────────────
  bool publish(const std::string& channel, const std::string& msg);

  std::unique_ptr<RedisSubscriber> subscribe(
      const std::string& channel,
      const std::string& own_session_id,
      std::function<void(const std::string&)> cb);

  // ── OT op log ────────────────────────────────────────────────
  // Atomically: LRANGE oplog[base_revision..-1] then RPUSH new_op.
  // Returns {ops_concurrent_with_incoming, server_assigned_revision}.
  struct OTResult {
    std::vector<std::string> concurrent_ops;
    int64_t                  new_revision = 0;
  };
  OTResult ot_commit(const std::string& project_id,
                      int64_t            base_revision,
                      const std::string& op_bytes);

private:
  // Cold fields: set at construction, rarely touched after that.
  std::string   host_;
  int           port_;
  // Hot fields on their own cache line: mu_ is acquired on every command;
  // ctx_ is read under mu_ so co-locating them avoids a second line fetch.
  alignas(64) mutable std::mutex mu_;
  redisContext* ctx_ = nullptr;

  // Lua script: atomic LRANGE + RPUSH for OT
  // KEYS[1]=oplog  ARGV[1]=base_revision  ARGV[2]=op_bytes
  // Returns: { array_of_concurrent_ops, new_list_length }
  static constexpr const char* OT_SCRIPT = R"(
local ops = redis.call('LRANGE', KEYS[1], ARGV[1], -1)
local rev = redis.call('RPUSH',  KEYS[1], ARGV[2])
return {ops, rev}
)";
};
