#include "service.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <iostream>

using grpc::Status;
using grpc::StatusCode;
using grpc::ServerContext;
using renderer::CanvasUpdate;
using renderer::UpdateType;

// ─────────────────────────────────────────────────────────────────
//  OT helpers
// ─────────────────────────────────────────────────────────────────

// Returns true for ops that mutate persistent canvas state and need OT.
// Ephemeral ops (STROKE_BEGIN, STROKE_POINT, CURSOR_MOVE) are broadcast
// as-is — they don't affect the final document.
static bool needs_ot(UpdateType t) {
  return t == renderer::STROKE_END   ||
         t == renderer::LAYER_ADD    ||
         t == renderer::LAYER_DELETE ||
         t == renderer::LAYER_MOVE   ||
         t == renderer::CANVAS_CLEAR;
}

// Transform op_a against a concurrent op_b that was committed first.
// Returns op_a', which may be a NOOP if op_a is now invalid.
static CanvasUpdate ot_transform(CanvasUpdate op_a, const CanvasUpdate& op_b) {
  const UpdateType ta = op_a.type();
  const UpdateType tb = op_b.type();

  // Stroke targeting a deleted or cleared layer → cancel
  if (ta == renderer::STROKE_END) {
    const std::string& layer = op_a.stroke().layer_id();
    if (tb == renderer::LAYER_DELETE && op_b.layer_id() == layer)
      op_a.set_type(renderer::NOOP);
    else if (tb == renderer::CANVAS_CLEAR && op_b.layer_id() == layer)
      op_a.set_type(renderer::NOOP);
  }

  // Duplicate LAYER_DELETE on the same layer → idempotent, drop second
  if (ta == renderer::LAYER_DELETE && tb == renderer::LAYER_DELETE &&
      op_a.layer_id() == op_b.layer_id())
    op_a.set_type(renderer::NOOP);

  // Concurrent LAYER_MOVE ops: if b moved a layer at or above a's target
  // position, a's index shifts by 1 to stay consistent
  if (ta == renderer::LAYER_MOVE && tb == renderer::LAYER_MOVE) {
    const int32_t order_a = op_a.layer().order();
    const int32_t order_b = op_b.layer().order();
    if (order_b <= order_a)
      op_a.mutable_layer()->set_order(order_a + 1);
  }

  // All other combinations (STROKE×STROKE, STROKE×STROKE_END, etc.)
  // are commutative for a pixel canvas — apply both without modification.
  return op_a;
}

// ─────────────────────────────────────────────────────────────────
//  Constructor
// ─────────────────────────────────────────────────────────────────
RendererServiceImpl::RendererServiceImpl(const std::string& storage_root,
                                          const std::string& redis_host,
                                          int                redis_port)
  : redis_(redis_host, redis_port),
    sessions_(&redis_),
    projects_(std::make_unique<ProjectStore>(storage_root)),
    rooms_(&redis_)
{
  std::thread([this]() {
    while (true) {
      std::this_thread::sleep_for(std::chrono::minutes(10));
      sessions_.purge_expired();
      rooms_.cleanup_empty();
    }
  }).detach();
}

// ─────────────────────────────────────────────────────────────────
//  Auth helper
// ─────────────────────────────────────────────────────────────────
Status RendererServiceImpl::auth(const std::string& token,
                                  renderer::Session* out_session) {
  if (token.empty())
    return Status(StatusCode::UNAUTHENTICATED, "Missing token");
  auto sess = sessions_.validate(token);
  if (!sess)
    return Status(StatusCode::UNAUTHENTICATED, "Invalid or expired token");
  if (out_session) *out_session = *sess;
  return Status::OK;
}

// ─────────────────────────────────────────────────────────────────
//  CreateSession
// ─────────────────────────────────────────────────────────────────
Status RendererServiceImpl::CreateSession(
    ServerContext*,
    const renderer::CreateSessionRequest* req,
    renderer::CreateSessionResponse* resp)
{
  if (req->username().empty())
    return Status(StatusCode::INVALID_ARGUMENT, "Username required");
  if (req->username().size() > 64)
    return Status(StatusCode::INVALID_ARGUMENT, "Username too long");

  auto [sess, token] = sessions_.create(req->username());
  *resp->mutable_session() = sess;
  resp->set_token(token);
  return Status::OK;
}

// ─────────────────────────────────────────────────────────────────
//  ValidateSession
// ─────────────────────────────────────────────────────────────────
Status RendererServiceImpl::ValidateSession(
    ServerContext*,
    const renderer::ValidateSessionRequest* req,
    renderer::ValidateSessionResponse* resp)
{
  auto sess = sessions_.validate(req->token());
  if (sess) {
    resp->set_valid(true);
    *resp->mutable_session() = *sess;
  } else {
    resp->set_valid(false);
  }
  return Status::OK;
}

// ─────────────────────────────────────────────────────────────────
//  SaveProject
// ─────────────────────────────────────────────────────────────────
Status RendererServiceImpl::SaveProject(
    ServerContext*,
    const renderer::SaveProjectRequest* req,
    renderer::SaveProjectResponse* resp)
{
  renderer::Session sess;
  if (auto s = auth(req->token(), &sess); !s.ok()) return s;

  if (req->project().name().empty())
    return Status(StatusCode::INVALID_ARGUMENT, "Project name required");

  renderer::Project proj = req->project();
  proj.set_owner_id(sess.user_id());

  if (proj.layers_size() == 0)
    return Status(StatusCode::INVALID_ARGUMENT, "Project must have at least one layer");

  for (const auto& layer : proj.layers()) {
    const size_t expected = static_cast<size_t>(layer.width()) *
                            static_cast<size_t>(layer.height()) * 4;
    if (!layer.pixels().empty() && layer.pixels().size() != expected)
      return Status(StatusCode::INVALID_ARGUMENT,
        "Layer '" + layer.name() + "' pixel data size mismatch");
  }

  const std::string id = projects_->save(proj);
  resp->set_success(true);
  resp->set_project_id(id);
  resp->set_message("Saved successfully");
  return Status::OK;
}

// ─────────────────────────────────────────────────────────────────
//  LoadProject
// ─────────────────────────────────────────────────────────────────
Status RendererServiceImpl::LoadProject(
    ServerContext*,
    const renderer::LoadProjectRequest* req,
    renderer::LoadProjectResponse* resp)
{
  renderer::Session sess;
  if (auto s = auth(req->token(), &sess); !s.ok()) return s;

  auto proj = projects_->load(req->project_id());
  if (!proj) {
    resp->set_success(false);
    resp->set_message("Project not found");
    return Status::OK;
  }
  if (proj->owner_id() != sess.user_id())
    return Status(StatusCode::PERMISSION_DENIED, "Access denied");

  resp->set_success(true);
  *resp->mutable_project() = *proj;
  return Status::OK;
}

// ─────────────────────────────────────────────────────────────────
//  ListProjects
// ─────────────────────────────────────────────────────────────────
Status RendererServiceImpl::ListProjects(
    ServerContext*,
    const renderer::ListProjectsRequest* req,
    renderer::ListProjectsResponse* resp)
{
  renderer::Session sess;
  if (auto s = auth(req->token(), &sess); !s.ok()) return s;

  const uint32_t per_page = req->per_page() == 0 ? 20 : req->per_page();
  auto metas = projects_->list(sess.user_id(), req->page(), per_page);
  for (auto& m : metas) *resp->add_projects() = std::move(m);
  resp->set_total(projects_->count(sess.user_id()));
  resp->set_page(req->page());
  return Status::OK;
}

// ─────────────────────────────────────────────────────────────────
//  DeleteProject
// ─────────────────────────────────────────────────────────────────
Status RendererServiceImpl::DeleteProject(
    ServerContext*,
    const renderer::DeleteProjectRequest* req,
    renderer::DeleteProjectResponse* resp)
{
  renderer::Session sess;
  if (auto s = auth(req->token(), &sess); !s.ok()) return s;

  const bool ok = projects_->remove(req->project_id(), sess.user_id());
  resp->set_success(ok);
  resp->set_message(ok ? "Deleted" : "Not found or access denied");
  return Status::OK;
}

// ─────────────────────────────────────────────────────────────────
//  RenameProject
// ─────────────────────────────────────────────────────────────────
Status RendererServiceImpl::RenameProject(
    ServerContext*,
    const renderer::RenameProjectRequest* req,
    renderer::RenameProjectResponse* resp)
{
  renderer::Session sess;
  if (auto s = auth(req->token(), &sess); !s.ok()) return s;

  if (req->new_name().empty())
    return Status(StatusCode::INVALID_ARGUMENT, "Name required");

  const bool ok = projects_->rename(req->project_id(), sess.user_id(),
                                    req->new_name());
  resp->set_success(ok);
  resp->set_message(ok ? "Renamed" : "Not found or access denied");
  return Status::OK;
}

// ─────────────────────────────────────────────────────────────────
//  ExportProject
// ─────────────────────────────────────────────────────────────────
Status RendererServiceImpl::ExportProject(
    ServerContext*,
    const renderer::ExportRequest* req,
    renderer::ExportResponse* resp)
{
  renderer::Session sess;
  if (auto s = auth(req->token(), &sess); !s.ok()) return s;

  auto proj = projects_->load(req->project_id());
  if (!proj) {
    resp->set_success(false);
    resp->set_message("Project not found");
    return Status::OK;
  }
  if (proj->owner_id() != sess.user_id())
    return Status(StatusCode::PERMISSION_DENIED, "Access denied");

  const uint32_t W = proj->canvas_width();
  const uint32_t H = proj->canvas_height();
  const size_t pixel_count = static_cast<size_t>(W) * H;
  std::vector<uint8_t> composite(pixel_count * 4, 0);

  for (const auto& layer : proj->layers()) {
    if (!layer.visible()) continue;
    const auto& px = layer.pixels();
    if (px.size() != pixel_count * 4) continue;

    for (size_t i = 0; i < pixel_count; ++i) {
      const uint8_t sr = static_cast<uint8_t>(px[i*4]);
      const uint8_t sg = static_cast<uint8_t>(px[i*4+1]);
      const uint8_t sb = static_cast<uint8_t>(px[i*4+2]);
      const uint8_t sa = static_cast<uint8_t>(px[i*4+3]);
      uint8_t& dr = composite[i*4];
      uint8_t& dg = composite[i*4+1];
      uint8_t& db = composite[i*4+2];
      uint8_t& da = composite[i*4+3];
      const float alpha_s = sa / 255.0f;
      const float alpha_d = da / 255.0f;
      const float alpha_o = alpha_s + alpha_d * (1.0f - alpha_s);
      if (alpha_o < 1e-6f) continue;
      dr = static_cast<uint8_t>((sr * alpha_s + dr * alpha_d * (1.0f - alpha_s)) / alpha_o);
      dg = static_cast<uint8_t>((sg * alpha_s + dg * alpha_d * (1.0f - alpha_s)) / alpha_o);
      db = static_cast<uint8_t>((sb * alpha_s + db * alpha_d * (1.0f - alpha_s)) / alpha_o);
      da = static_cast<uint8_t>(alpha_o * 255.0f);
    }
  }

  resp->set_success(true);
  resp->set_data(std::string(composite.begin(), composite.end()));
  resp->set_mime_type("image/raw-rgba");
  resp->set_message(std::to_string(W) + "x" + std::to_string(H) + " RGBA");
  return Status::OK;
}

// ─────────────────────────────────────────────────────────────────
//  Collaborate  — bidirectional streaming with OT
//
//  Flow:
//    1. Client sends first message identifying session + project
//    2. Server subscribes to Redis channel for that project
//    3. Incoming ops: ephemeral ones broadcast immediately;
//       mutating ops go through OT (transform + Redis op log commit)
//       then broadcast.  Sender receives an ack with server revision.
//    4. Writer thread flushes pending outbound messages to client.
// ─────────────────────────────────────────────────────────────────
Status RendererServiceImpl::Collaborate(
    ServerContext* ctx,
    grpc::ServerReaderWriter<CanvasUpdate, CanvasUpdate>* stream)
{
  CanvasUpdate first;
  if (!stream->Read(&first))
    return Status(StatusCode::INVALID_ARGUMENT, "Expected initial message");

  auto sess = sessions_.validate(first.session_id());
  if (!sess)
    return Status(StatusCode::UNAUTHENTICATED, "Invalid session");

  const std::string& project_id = first.project_id();
  if (project_id.empty())
    return Status(StatusCode::INVALID_ARGUMENT, "project_id required");

  auto room = rooms_.get_or_create(project_id);

  std::mutex write_mu;
  std::condition_variable write_cv;
  std::vector<CanvasUpdate> pending;
  bool done = false;

  // Helper: push a message into the writer queue
  auto enqueue = [&](const CanvasUpdate& u) {
    std::lock_guard<std::mutex> lk(write_mu);
    pending.push_back(u);
    write_cv.notify_one();
  };

  // Subscribe: Redis messages arrive here, already filtered for own session_id
  const uint64_t sub_id = room->subscribe(sess->id(),
    [&](const CanvasUpdate& upd) { enqueue(upd); });

  ++active_streams_;
  std::cout << "[collab] user=" << sess->username()
            << " joined project=" << project_id
            << " active_streams=" << active_streams_ << "\n";

  // Writer thread
  std::thread writer([&]() {
    while (!done && !ctx->IsCancelled()) {
      std::unique_lock<std::mutex> lk(write_mu);
      write_cv.wait_for(lk, std::chrono::milliseconds(100),
        [&]{ return !pending.empty() || done; });
      if (pending.empty()) continue;
      auto batch = std::move(pending);
      lk.unlock();
      for (auto& u : batch)
        if (!stream->Write(u)) { done = true; break; }
    }
  });

  // Broadcast first handshake message
  room->broadcast(first);

  // Read loop: client → OT → Redis → broadcast
  CanvasUpdate upd;
  while (!ctx->IsCancelled() && stream->Read(&upd)) {
    upd.set_user_id(sess->user_id());
    upd.set_username(sess->username());

    if (needs_ot(upd.type())) {
      // Atomically get concurrent ops + commit this op to the Redis op log
      const std::string op_bytes = upd.SerializeAsString();
      auto ot = redis_.ot_commit(project_id, upd.base_revision(), op_bytes);

      // Transform against each op committed since client's base revision
      CanvasUpdate transformed = upd;
      for (const auto& concurrent_bytes : ot.concurrent_ops) {
        CanvasUpdate concurrent;
        if (concurrent.ParseFromString(concurrent_bytes))
          transformed = ot_transform(transformed, concurrent);
      }
      transformed.set_revision(ot.new_revision);

      // Ack back to sender: client uses this to advance localRevision
      // and drain its pendingOps queue
      enqueue(transformed);

      // Broadcast transformed op to all other clients (NOOP not broadcast)
      if (transformed.type() != renderer::NOOP)
        room->broadcast(transformed);

    } else {
      // Ephemeral ops: forward immediately without OT
      room->broadcast(upd);
    }
  }

  done = true;
  write_cv.notify_all();
  if (writer.joinable()) writer.join();

  room->unsubscribe(sub_id);
  --active_streams_;

  std::cout << "[collab] user=" << sess->username()
            << " left project=" << project_id
            << " active_streams=" << active_streams_ << "\n";
  return Status::OK;
}

// ─────────────────────────────────────────────────────────────────
//  JoinRoom  — server-side stream (read-only observer)
// ─────────────────────────────────────────────────────────────────
Status RendererServiceImpl::JoinRoom(
    ServerContext* ctx,
    const renderer::JoinRoomRequest* req,
    grpc::ServerWriter<CanvasUpdate>* writer)
{
  renderer::Session sess;
  if (auto s = auth(req->token(), &sess); !s.ok()) return s;

  auto room = rooms_.get_or_create(req->project_id());

  std::mutex mu;
  std::condition_variable cv;
  std::vector<CanvasUpdate> pending;
  bool done = false;

  const uint64_t sub_id = room->subscribe(sess.id(),
    [&](const CanvasUpdate& upd) {
      { std::lock_guard<std::mutex> lk(mu); pending.push_back(upd); }
      cv.notify_one();
    });

  std::cout << "[join-room] observer=" << sess.username()
            << " project=" << req->project_id() << "\n";

  while (!done && !ctx->IsCancelled()) {
    std::unique_lock<std::mutex> lk(mu);
    cv.wait_for(lk, std::chrono::milliseconds(200),
      [&]{ return !pending.empty() || done; });
    auto batch = std::move(pending);
    lk.unlock();
    for (auto& u : batch)
      if (!writer->Write(u)) { done = true; break; }
  }

  room->unsubscribe(sub_id);
  return Status::OK;
}
