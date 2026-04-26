#include "storage.h"

#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <functional>

// ─────────────────────────────────────────────────────────────────
//  Token utilities
// ─────────────────────────────────────────────────────────────────
namespace token {

std::string generate(size_t len) {
  static const char charset[] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789";
  static thread_local std::mt19937_64 rng{ std::random_device{}() };
  std::uniform_int_distribution<size_t> dist(0, sizeof(charset) - 2);
  std::string out(len, ' ');
  for (auto& c : out) c = charset[dist(rng)];
  return out;
}

// Simple FNV-1a string hash → hex string (good enough for in-memory use)
std::string hash(const std::string& raw) {
  uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : raw) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  std::ostringstream oss;
  oss << std::hex << std::setw(16) << std::setfill('0') << h;
  return oss.str();
}

} // namespace token

// ─────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────
static int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static std::string new_uuid() {
  // Simple UUID-like id from random bytes
  static thread_local std::mt19937_64 rng{ std::random_device{}() };
  std::uniform_int_distribution<uint64_t> dist;
  uint64_t a = dist(rng), b = dist(rng);
  std::ostringstream ss;
  ss << std::hex << std::setw(16) << std::setfill('0') << a
     << std::setw(16) << std::setfill('0') << b;
  std::string s = ss.str();
  // Format: 8-4-4-4-12
  return s.substr(0,8) + "-" + s.substr(8,4) + "-" + s.substr(12,4)
       + "-" + s.substr(16,4) + "-" + s.substr(20,12);
}

// ─────────────────────────────────────────────────────────────────
//  SessionStore
// ─────────────────────────────────────────────────────────────────
std::pair<renderer::Session, std::string>
SessionStore::create(const std::string& username) {
  std::lock_guard<std::mutex> lk(mu_);

  renderer::Session sess;
  sess.set_id(new_uuid());
  sess.set_user_id(new_uuid());
  sess.set_username(username);
  sess.set_created_at(now_ms());
  // 7-day expiry
  const int64_t TTL_MS = 7LL * 24 * 3600 * 1000;
  sess.set_expires_at(sess.created_at() + TTL_MS);

  const std::string raw_token = token::generate(48);
  const std::string h         = token::hash(raw_token);

  Entry e;
  e.session    = sess;
  e.token_hash = h;
  e.expires_at = sess.expires_at();

  // Remove any previous token for this user
  auto it = user_token_.find(sess.user_id());
  if (it != user_token_.end()) {
    by_token_hash_.erase(it->second);
  }
  by_token_hash_[h]              = std::move(e);
  user_token_[sess.user_id()]    = h;

  return { sess, raw_token };
}

std::optional<renderer::Session>
SessionStore::validate(const std::string& raw_token) const {
  const std::string h = token::hash(raw_token);
  std::lock_guard<std::mutex> lk(mu_);
  auto it = by_token_hash_.find(h);
  if (it == by_token_hash_.end()) return std::nullopt;
  if (now_ms() > it->second.expires_at) return std::nullopt;
  return it->second.session;
}

void SessionStore::purge_expired() {
  std::lock_guard<std::mutex> lk(mu_);
  const int64_t t = now_ms();
  for (auto it = by_token_hash_.begin(); it != by_token_hash_.end(); ) {
    if (t > it->second.expires_at) {
      user_token_.erase(it->second.session.user_id());
      it = by_token_hash_.erase(it);
    } else {
      ++it;
    }
  }
}

// ─────────────────────────────────────────────────────────────────
//  ProjectStore
// ─────────────────────────────────────────────────────────────────
ProjectStore::ProjectStore(const std::string& root_dir) : root_(root_dir) {
  fs::create_directories(root_);
}

fs::path ProjectStore::project_path(const std::string& id) const {
  return root_ / (id + ".bin");
}

fs::path ProjectStore::meta_path(const std::string& id) const {
  return root_ / (id + ".meta");
}

bool ProjectStore::write_proto(const fs::path& p,
                               const google::protobuf::Message& msg) {
  std::string bytes;
  if (!msg.SerializeToString(&bytes)) return false;
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return f.good();
}

bool ProjectStore::read_proto(const fs::path& p,
                              google::protobuf::Message* msg) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::string bytes((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
  return msg->ParseFromString(bytes);
}

std::string ProjectStore::new_id() const {
  return new_uuid();
}

int64_t ProjectStore::now_ms() const {
  return ::now_ms();
}

std::string ProjectStore::save(const renderer::Project& project) {
  std::lock_guard<std::mutex> lk(mu_);

  renderer::Project proj = project;
  if (proj.id().empty()) proj.set_id(new_id());

  const int64_t t = now_ms();
  if (proj.created_at() == 0) proj.set_created_at(t);
  proj.set_updated_at(t);

  // Build meta
  renderer::ProjectMeta meta;
  meta.set_id(proj.id());
  meta.set_name(proj.name());
  meta.set_canvas_width(proj.canvas_width());
  meta.set_canvas_height(proj.canvas_height());
  meta.set_created_at(proj.created_at());
  meta.set_updated_at(proj.updated_at());
  meta.set_owner_id(proj.owner_id());
  meta.set_thumbnail(proj.thumbnail());
  meta.set_layer_count(static_cast<uint32_t>(proj.layers_size()));

  write_proto(project_path(proj.id()), proj);
  write_proto(meta_path(proj.id()), meta);

  return proj.id();
}

std::optional<renderer::Project>
ProjectStore::load(const std::string& project_id) {
  std::lock_guard<std::mutex> lk(mu_);
  renderer::Project proj;
  if (!read_proto(project_path(project_id), &proj)) return std::nullopt;
  return proj;
}

std::optional<renderer::ProjectMeta>
ProjectStore::load_meta(const std::string& project_id) {
  std::lock_guard<std::mutex> lk(mu_);
  renderer::ProjectMeta meta;
  if (!read_proto(meta_path(project_id), &meta)) return std::nullopt;
  return meta;
}

std::vector<renderer::ProjectMeta>
ProjectStore::list(const std::string& owner_id,
                   uint32_t page, uint32_t per_page) {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<renderer::ProjectMeta> results;

  for (const auto& entry : fs::directory_iterator(root_)) {
    if (entry.path().extension() != ".meta") continue;
    renderer::ProjectMeta meta;
    if (!read_proto(entry.path(), &meta)) continue;
    if (meta.owner_id() != owner_id) continue;
    results.push_back(std::move(meta));
  }

  // Sort by updated_at descending
  std::sort(results.begin(), results.end(),
    [](const renderer::ProjectMeta& a, const renderer::ProjectMeta& b) {
      return a.updated_at() > b.updated_at();
    });

  // Paginate
  const size_t start = static_cast<size_t>(page) * per_page;
  if (start >= results.size()) return {};
  const size_t end = std::min(start + per_page, results.size());
  return { results.begin() + start, results.begin() + end };
}

uint32_t ProjectStore::count(const std::string& owner_id) {
  std::lock_guard<std::mutex> lk(mu_);
  uint32_t n = 0;
  for (const auto& entry : fs::directory_iterator(root_)) {
    if (entry.path().extension() != ".meta") continue;
    renderer::ProjectMeta meta;
    if (!read_proto(entry.path(), &meta)) continue;
    if (meta.owner_id() == owner_id) ++n;
  }
  return n;
}

bool ProjectStore::remove(const std::string& project_id,
                          const std::string& owner_id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto meta_p = meta_path(project_id);
  renderer::ProjectMeta meta;
  if (!read_proto(meta_p, &meta)) return false;
  if (meta.owner_id() != owner_id) return false;
  fs::remove(project_path(project_id));
  fs::remove(meta_p);
  return true;
}

bool ProjectStore::rename(const std::string& project_id,
                          const std::string& owner_id,
                          const std::string& new_name) {
  std::lock_guard<std::mutex> lk(mu_);
  auto meta_p = meta_path(project_id);
  renderer::ProjectMeta meta;
  if (!read_proto(meta_p, &meta)) return false;
  if (meta.owner_id() != owner_id) return false;
  meta.set_name(new_name);
  meta.set_updated_at(now_ms());

  // Also update name in full project file
  renderer::Project proj;
  auto proj_p = project_path(project_id);
  if (read_proto(proj_p, &proj)) {
    proj.set_name(new_name);
    proj.set_updated_at(meta.updated_at());
    write_proto(proj_p, proj);
  }
  return write_proto(meta_p, meta);
}

// ─────────────────────────────────────────────────────────────────
//  CollabRoom
// ─────────────────────────────────────────────────────────────────
uint64_t CollabRoom::subscribe(const std::string& session_id,
                               UpdateCallback cb) {
  std::lock_guard<std::mutex> lk(mu_);
  const uint64_t id = next_id_++;
  subs_[id] = { session_id, std::move(cb) };
  return id;
}

void CollabRoom::unsubscribe(uint64_t sub_id) {
  std::lock_guard<std::mutex> lk(mu_);
  subs_.erase(sub_id);
}

void CollabRoom::broadcast(const renderer::CanvasUpdate& update,
                           uint64_t except_sub) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& [id, sub] : subs_) {
    if (id == except_sub) continue;
    sub.cb(update);
  }
}

size_t CollabRoom::subscriber_count() const {
  std::lock_guard<std::mutex> lk(mu_);
  return subs_.size();
}

// ─────────────────────────────────────────────────────────────────
//  RoomRegistry
// ─────────────────────────────────────────────────────────────────
std::shared_ptr<CollabRoom>
RoomRegistry::get_or_create(const std::string& project_id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto& ptr = rooms_[project_id];
  if (!ptr) ptr = std::make_shared<CollabRoom>();
  return ptr;
}

void RoomRegistry::cleanup_empty() {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto it = rooms_.begin(); it != rooms_.end(); ) {
    if (it->second->subscriber_count() == 0)
      it = rooms_.erase(it);
    else
      ++it;
  }
}
