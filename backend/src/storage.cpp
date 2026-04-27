#include "storage.h"

#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>

// ─────────────────────────────────────────────────────────────────
//  Helpers
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

// FNV-1a → hex (used only as a fast key; not for security)
std::string hash(const std::string& raw) {
  uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : raw) { h ^= c; h *= 1099511628211ULL; }
  std::ostringstream oss;
  oss << std::hex << std::setw(16) << std::setfill('0') << h;
  return oss.str();
}

} // namespace token

static int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static std::string new_uuid() {
  static thread_local std::mt19937_64 rng{ std::random_device{}() };
  std::uniform_int_distribution<uint64_t> dist;
  uint64_t a = dist(rng), b = dist(rng);
  std::ostringstream ss;
  ss << std::hex << std::setw(16) << std::setfill('0') << a
                 << std::setw(16) << std::setfill('0') << b;
  std::string s = ss.str();
  return s.substr(0,8)+"-"+s.substr(8,4)+"-"+s.substr(12,4)
        +"-"+s.substr(16,4)+"-"+s.substr(20,12);
}

// ─────────────────────────────────────────────────────────────────
//  SessionStore  (Redis-backed)
// ─────────────────────────────────────────────────────────────────
std::string SessionStore::session_key(const std::string& token_hash) {
  return "session:" + token_hash;
}

SessionStore::SessionStore(RedisStore* redis) : redis_(redis) {}

std::pair<renderer::Session, std::string>
SessionStore::create(const std::string& username) {
  renderer::Session sess;
  sess.set_id(new_uuid());
  sess.set_user_id(new_uuid());
  sess.set_username(username);
  sess.set_created_at(now_ms());
  sess.set_expires_at(sess.created_at() + static_cast<int64_t>(SESSION_TTL_S) * 1000);

  const std::string raw_token = token::generate(48);
  const std::string h         = token::hash(raw_token);

  std::string bytes;
  sess.SerializeToString(&bytes);
  redis_->set(session_key(h), bytes, SESSION_TTL_S);

  std::cout << "[session] created user=" << username
            << " id=" << sess.id() << "\n";
  return { sess, raw_token };
}

std::optional<renderer::Session>
SessionStore::validate(const std::string& raw_token) const {
  const std::string h    = token::hash(raw_token);
  auto              data = redis_->get(session_key(h));
  if (!data) return std::nullopt;

  renderer::Session sess;
  if (!sess.ParseFromString(*data)) return std::nullopt;
  if (now_ms() > sess.expires_at()) {
    redis_->del(session_key(h));  // clean up expired key early
    return std::nullopt;
  }
  return sess;
}

// ─────────────────────────────────────────────────────────────────
//  ProjectStore  (file-backed, unchanged from original)
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

std::string ProjectStore::new_id() const { return new_uuid(); }
int64_t     ProjectStore::now_ms() const { return ::now_ms(); }

std::string ProjectStore::save(const renderer::Project& project) {
  std::lock_guard<std::mutex> lk(mu_);

  renderer::Project proj = project;
  if (proj.id().empty()) proj.set_id(new_id());

  const int64_t t = now_ms();
  if (proj.created_at() == 0) proj.set_created_at(t);
  proj.set_updated_at(t);

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

  std::sort(results.begin(), results.end(),
    [](const renderer::ProjectMeta& a, const renderer::ProjectMeta& b) {
      return a.updated_at() > b.updated_at();
    });

  const size_t start = static_cast<size_t>(page) * per_page;
  if (start >= results.size()) return {};
  const size_t end = std::min(start + per_page, results.size());
  return { results.begin() + static_cast<ptrdiff_t>(start),
           results.begin() + static_cast<ptrdiff_t>(end) };
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
//  CollabRoom  (Redis pub/sub)
// ─────────────────────────────────────────────────────────────────
CollabRoom::CollabRoom(RedisStore* redis, std::string project_id)
    : redis_(redis),
      project_id_(std::move(project_id)),
      channel_("project:" + project_id_)
{}

uint64_t CollabRoom::subscribe(const std::string& session_id,
                                UpdateCallback cb) {
  std::lock_guard<std::mutex> lk(mu_);
  const uint64_t id = next_id_++;

  // Wrap the callback: deserialize the Redis message bytes into a
  // CanvasUpdate, filter out own session, then call the user callback.
  auto wrapped = [session_id, cb = std::move(cb)](const std::string& msg_bytes) {
    renderer::CanvasUpdate op;
    if (!op.ParseFromString(msg_bytes)) return;
    if (op.session_id() == session_id) return;  // skip own messages
    cb(op);
  };

  subs_[id] = Sub{
    session_id,
    cb,  // kept for potential direct use; redis_sub drives actual delivery
    redis_->subscribe(channel_, session_id, std::move(wrapped))
  };

  return id;
}

void CollabRoom::unsubscribe(uint64_t sub_id) {
  std::lock_guard<std::mutex> lk(mu_);
  subs_.erase(sub_id);  // RedisSubscriber dtor stops the thread
}

void CollabRoom::broadcast(const renderer::CanvasUpdate& update) {
  std::string bytes;
  update.SerializeToString(&bytes);
  redis_->publish(channel_, bytes);
}

size_t CollabRoom::subscriber_count() const {
  std::lock_guard<std::mutex> lk(mu_);
  return subs_.size();
}

// ─────────────────────────────────────────────────────────────────
//  RoomRegistry
// ─────────────────────────────────────────────────────────────────
RoomRegistry::RoomRegistry(RedisStore* redis) : redis_(redis) {}

std::shared_ptr<CollabRoom>
RoomRegistry::get_or_create(const std::string& project_id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto& ptr = rooms_[project_id];
  if (!ptr) ptr = std::make_shared<CollabRoom>(redis_, project_id);
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
