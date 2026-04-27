#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <optional>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <functional>
#include <memory>

#include "../proto/renderer.grpc.pb.h"
#include "redis_client.h"

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────
//  Token utilities
// ─────────────────────────────────────────────────────────────────
namespace token {
  std::string generate(size_t len = 32);
  std::string hash(const std::string& raw);
}

// ─────────────────────────────────────────────────────────────────
//  SessionStore  — Redis-backed, survives server restarts
//
//  Redis keys:
//    session:{token_hash}  →  serialized renderer::Session  (7-day TTL)
// ─────────────────────────────────────────────────────────────────
class SessionStore {
public:
  explicit SessionStore(RedisStore* redis);

  // Returns {session, raw_token}
  std::pair<renderer::Session, std::string> create(const std::string& username);

  // Returns session if token valid, else nullopt
  std::optional<renderer::Session> validate(const std::string& raw_token) const;

  // No-op: Redis TTL handles expiry automatically
  void purge_expired() {}

private:
  RedisStore* redis_;

  static constexpr int SESSION_TTL_S = 7 * 24 * 3600;  // 7 days
  static std::string session_key(const std::string& token_hash);
};

// ─────────────────────────────────────────────────────────────────
//  ProjectStore  — file-backed, thread-safe (unchanged)
//
//  Layout on disk:
//    storage_root/
//      {project_id}.bin      ← serialized protobuf Project
//      {project_id}.meta     ← serialized ProjectMeta protobuf
// ─────────────────────────────────────────────────────────────────
class ProjectStore {
public:
  explicit ProjectStore(const std::string& root_dir);

  std::string save(const renderer::Project& project);

  std::optional<renderer::Project>     load(const std::string& project_id);
  std::optional<renderer::ProjectMeta> load_meta(const std::string& project_id);

  std::vector<renderer::ProjectMeta> list(
      const std::string& owner_id,
      uint32_t page     = 0,
      uint32_t per_page = 20);

  uint32_t count(const std::string& owner_id);

  bool remove(const std::string& project_id, const std::string& owner_id);
  bool rename(const std::string& project_id, const std::string& owner_id,
              const std::string& new_name);

private:
  fs::path           root_;
  mutable std::mutex mu_;

  fs::path project_path(const std::string& id) const;
  fs::path meta_path(const std::string& id) const;

  bool write_proto(const fs::path& p, const google::protobuf::Message& msg);
  bool read_proto(const fs::path& p, google::protobuf::Message* msg);

  std::string new_id() const;
  int64_t     now_ms() const;
};

// ─────────────────────────────────────────────────────────────────
//  CollabRoom  — Redis pub/sub fan-out for one project
//
//  Each subscriber gets a dedicated RedisSubscriber (own connection +
//  thread) that listens on "project:{project_id}".  Messages from
//  own session_id are filtered out inside the subscriber callback.
//  broadcast() publishes to the Redis channel — all server instances
//  receive it automatically.
// ─────────────────────────────────────────────────────────────────
using UpdateCallback = std::function<void(const renderer::CanvasUpdate&)>;

class CollabRoom {
public:
  CollabRoom(RedisStore* redis, std::string project_id);

  // Returns subscriber id
  uint64_t subscribe(const std::string& session_id, UpdateCallback cb);
  void     unsubscribe(uint64_t sub_id);

  // Publish to Redis channel — all subscribers across all servers receive it
  void broadcast(const renderer::CanvasUpdate& update);

  size_t subscriber_count() const;

private:
  RedisStore* redis_;
  std::string project_id_;
  std::string channel_;  // "project:{project_id}"

  mutable std::mutex mu_;
  uint64_t next_id_{ 1 };

  struct Sub {
    std::string                      session_id;
    UpdateCallback                   cb;
    std::unique_ptr<RedisSubscriber> redis_sub;
  };
  std::unordered_map<uint64_t, Sub> subs_;
};

// ─────────────────────────────────────────────────────────────────
//  RoomRegistry  — one CollabRoom per active project
// ─────────────────────────────────────────────────────────────────
class RoomRegistry {
public:
  explicit RoomRegistry(RedisStore* redis);

  std::shared_ptr<CollabRoom> get_or_create(const std::string& project_id);
  void cleanup_empty();

private:
  RedisStore*  redis_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<CollabRoom>> rooms_;
};
