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

#include "../proto/renderer.grpc.pb.h"

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────
//  Token utilities
// ─────────────────────────────────────────────────────────────────
namespace token {
  std::string generate(size_t len = 32);
  std::string hash(const std::string& raw);
}

// ─────────────────────────────────────────────────────────────────
//  SessionStore  — in-memory, thread-safe
// ─────────────────────────────────────────────────────────────────
class SessionStore {
public:
  // Returns {session, token}
  std::pair<renderer::Session, std::string> create(const std::string& username);

  // Returns session if token is valid and not expired, else nullopt
  std::optional<renderer::Session> validate(const std::string& token) const;

  void purge_expired();

private:
  struct Entry {
    renderer::Session session;
    std::string       token_hash;
    int64_t           expires_at;
  };

  mutable std::mutex              mu_;
  std::unordered_map<std::string, Entry> by_token_hash_;  // hash → entry
  std::unordered_map<std::string, std::string> user_token_; // user_id → hash
};

// ─────────────────────────────────────────────────────────────────
//  ProjectStore  — file-backed, thread-safe
//
//  Layout on disk:
//    storage_root/
//      {project_id}.bin      ← serialized protobuf
//      {project_id}.meta     ← serialized ProjectMeta protobuf
// ─────────────────────────────────────────────────────────────────
class ProjectStore {
public:
  explicit ProjectStore(const std::string& root_dir);

  // Returns project_id (creates new id if project.id() is empty)
  std::string save(const renderer::Project& project);

  std::optional<renderer::Project>     load(const std::string& project_id);
  std::optional<renderer::ProjectMeta> load_meta(const std::string& project_id);

  // Returns all metas owned by owner_id, sorted by updated_at desc
  std::vector<renderer::ProjectMeta> list(
      const std::string& owner_id,
      uint32_t page     = 0,
      uint32_t per_page = 20);

  uint32_t count(const std::string& owner_id);

  bool remove(const std::string& project_id, const std::string& owner_id);
  bool rename(const std::string& project_id, const std::string& owner_id,
              const std::string& new_name);

private:
  fs::path            root_;
  mutable std::mutex  mu_;

  fs::path project_path(const std::string& id) const;
  fs::path meta_path(const std::string& id) const;

  bool write_proto(const fs::path& p, const google::protobuf::Message& msg);
  bool read_proto(const fs::path& p, google::protobuf::Message* msg);

  std::string new_id() const;
  int64_t now_ms() const;
};

// ─────────────────────────────────────────────────────────────────
//  CollabRoom  — fan-out in-memory message bus for one project
// ─────────────────────────────────────────────────────────────────
using UpdateCallback = std::function<void(const renderer::CanvasUpdate&)>;

class CollabRoom {
public:
  // Returns subscriber id
  uint64_t subscribe(const std::string& session_id, UpdateCallback cb);
  void unsubscribe(uint64_t sub_id);
  void broadcast(const renderer::CanvasUpdate& update, uint64_t except_sub = 0);
  size_t subscriber_count() const;

private:
  mutable std::mutex mu_;
  uint64_t next_id_{ 1 };
  struct Sub { std::string session_id; UpdateCallback cb; };
  std::unordered_map<uint64_t, Sub> subs_;
};

// ─────────────────────────────────────────────────────────────────
//  RoomRegistry  — one CollabRoom per project
// ─────────────────────────────────────────────────────────────────
class RoomRegistry {
public:
  std::shared_ptr<CollabRoom> get_or_create(const std::string& project_id);
  void cleanup_empty();

private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<CollabRoom>> rooms_;
};
