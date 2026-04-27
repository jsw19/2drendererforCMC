#include "redis_client.h"

#include <iostream>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────
//  RedisSubscriber
// ─────────────────────────────────────────────────────────────────
RedisSubscriber::RedisSubscriber(const std::string& host, int port,
                                  const std::string& channel,
                                  const std::string& own_session_id,
                                  std::function<void(const std::string&)> cb)
{
  ctx_ = redisConnect(host.c_str(), port);
  if (!ctx_ || ctx_->err) {
    std::cerr << "[redis-sub] connect failed: "
              << (ctx_ ? ctx_->errstr : "null context") << "\n";
    return;
  }

  // SUBSCRIBE blocks this connection; do it once before the thread starts
  redisReply* r = static_cast<redisReply*>(
      redisCommand(ctx_, "SUBSCRIBE %s", channel.c_str()));
  freeReplyObject(r);

  thread_ = std::thread([this, own_session_id, cb = std::move(cb)]() {
    while (!stop_) {
      redisReply* reply = nullptr;
      if (redisGetReply(ctx_, reinterpret_cast<void**>(&reply)) != REDIS_OK || !reply)
        break;

      // Redis pub/sub message format: ["message", channel, payload]
      if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) {
        const std::string msg_type(reply->element[0]->str,
                                   static_cast<size_t>(reply->element[0]->len));
        if (msg_type == "message") {
          const std::string payload(reply->element[2]->str,
                                    static_cast<size_t>(reply->element[2]->len));

          // Deserialise just enough to check session_id and skip own messages.
          // We avoid a full proto parse here by checking the session_id field
          // at the application level via the callback wrapper in CollabRoom.
          if (!own_session_id.empty()) {
            // Quick check: does the payload contain own_session_id?
            // CollabRoom passes a wrapper that does the real parse + check.
          }
          cb(payload);
        }
      }
      freeReplyObject(reply);
    }
  });
}

RedisSubscriber::~RedisSubscriber() {
  stop_ = true;
  // Closing the context wakes the blocking redisGetReply
  if (ctx_) redisFree(ctx_);
  ctx_ = nullptr;
  if (thread_.joinable()) thread_.join();
}

// ─────────────────────────────────────────────────────────────────
//  RedisStore
// ─────────────────────────────────────────────────────────────────
RedisStore::RedisStore(const std::string& host, int port)
    : host_(host), port_(port)
{
  ctx_ = redisConnect(host.c_str(), port);
  if (!ctx_ || ctx_->err) {
    std::cerr << "[redis] connect failed: "
              << (ctx_ ? ctx_->errstr : "null context") << "\n";
  } else {
    std::cout << "[redis] connected to " << host << ":" << port << "\n";
  }
}

RedisStore::~RedisStore() {
  if (ctx_) redisFree(ctx_);
}

bool RedisStore::ok() const {
  return ctx_ && ctx_->err == 0;
}

bool RedisStore::set(const std::string& key,
                      const std::string& val,
                      int ttl_seconds) {
  std::lock_guard<std::mutex> lk(mu_);
  redisReply* r;
  if (ttl_seconds > 0) {
    r = static_cast<redisReply*>(redisCommand(ctx_,
        "SET %s %b EX %d",
        key.c_str(), val.data(), val.size(), ttl_seconds));
  } else {
    r = static_cast<redisReply*>(redisCommand(ctx_,
        "SET %s %b",
        key.c_str(), val.data(), val.size()));
  }
  const bool ok = r && r->type != REDIS_REPLY_ERROR;
  freeReplyObject(r);
  return ok;
}

std::optional<std::string> RedisStore::get(const std::string& key) const {
  std::lock_guard<std::mutex> lk(mu_);
  redisReply* r = static_cast<redisReply*>(
      redisCommand(ctx_, "GET %s", key.c_str()));
  if (!r || r->type == REDIS_REPLY_NIL || r->type == REDIS_REPLY_ERROR) {
    freeReplyObject(r);
    return std::nullopt;
  }
  std::string val(r->str, static_cast<size_t>(r->len));
  freeReplyObject(r);
  return val;
}

bool RedisStore::del(const std::string& key) {
  std::lock_guard<std::mutex> lk(mu_);
  redisReply* r = static_cast<redisReply*>(
      redisCommand(ctx_, "DEL %s", key.c_str()));
  const bool ok = r && r->integer > 0;
  freeReplyObject(r);
  return ok;
}

bool RedisStore::publish(const std::string& channel, const std::string& msg) {
  std::lock_guard<std::mutex> lk(mu_);
  redisReply* r = static_cast<redisReply*>(redisCommand(ctx_,
      "PUBLISH %s %b",
      channel.c_str(), msg.data(), msg.size()));
  const bool ok = r && r->type != REDIS_REPLY_ERROR;
  freeReplyObject(r);
  return ok;
}

std::unique_ptr<RedisSubscriber>
RedisStore::subscribe(const std::string& channel,
                       const std::string& own_session_id,
                       std::function<void(const std::string&)> cb) {
  return std::make_unique<RedisSubscriber>(
      host_, port_, channel, own_session_id, std::move(cb));
}

RedisStore::OTResult
RedisStore::ot_commit(const std::string& project_id,
                       int64_t            base_revision,
                       const std::string& op_bytes) {
  const std::string key = "project:" + project_id + ":oplog";

  std::lock_guard<std::mutex> lk(mu_);

  // EVAL script 1 key base_revision op_bytes
  // ARGV[1] = base_revision as integer string → used as LRANGE start index
  // op at revision R is stored at index R-1; concurrent ops start at index base_revision
  redisReply* r = static_cast<redisReply*>(redisCommand(ctx_,
      "EVAL %s 1 %s %lld %b",
      OT_SCRIPT,
      key.c_str(),
      static_cast<long long>(base_revision),
      op_bytes.data(), op_bytes.size()));

  OTResult result;
  if (!r || r->type != REDIS_REPLY_ARRAY || r->elements < 2) {
    std::cerr << "[redis] ot_commit eval failed\n";
    freeReplyObject(r);
    return result;
  }

  // element[0]: array of concurrent op byte strings
  if (r->element[0]->type == REDIS_REPLY_ARRAY) {
    result.concurrent_ops.reserve(r->element[0]->elements);
    for (size_t i = 0; i < r->element[0]->elements; ++i) {
      auto* elem = r->element[0]->element[i];
      if (elem && elem->str)
        result.concurrent_ops.emplace_back(elem->str,
                                            static_cast<size_t>(elem->len));
    }
  }

  // element[1]: new list length = new revision number
  result.new_revision = r->element[1]->integer;

  freeReplyObject(r);
  return result;
}
