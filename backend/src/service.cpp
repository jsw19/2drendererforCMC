#include "service.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <iostream>

using grpc::Status;
using grpc::StatusCode;
using grpc::ServerContext;

// ─────────────────────────────────────────────────────────────────
//  Constructor
// ─────────────────────────────────────────────────────────────────
RendererServiceImpl::RendererServiceImpl(const std::string& storage_root)
  : projects_(std::make_unique<ProjectStore>(storage_root))
{
  // Background thread to purge expired sessions every 10 minutes
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
    ServerContext* /*ctx*/,
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

  std::cout << "[session] created for user: " << req->username()
            << " id=" << sess.id() << "\n";
  return Status::OK;
}

// ─────────────────────────────────────────────────────────────────
//  ValidateSession
// ─────────────────────────────────────────────────────────────────
Status RendererServiceImpl::ValidateSession(
    ServerContext* /*ctx*/,
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
    ServerContext* /*ctx*/,
    const renderer::SaveProjectRequest* req,
    renderer::SaveProjectResponse* resp)
{
  renderer::Session sess;
  if (auto s = auth(req->token(), &sess); !s.ok()) return s;

  if (req->project().name().empty())
    return Status(StatusCode::INVALID_ARGUMENT, "Project name required");

  renderer::Project proj = req->project();
  proj.set_owner_id(sess.user_id());

  // Basic sanity: at least one layer required
  if (proj.layers_size() == 0)
    return Status(StatusCode::INVALID_ARGUMENT, "Project must have at least one layer");

  // Check pixel data size consistency
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

  std::cout << "[project] saved id=" << id
            << " layers=" << proj.layers_size()
            << " owner=" << sess.username() << "\n";
  return Status::OK;
}

// ─────────────────────────────────────────────────────────────────
//  LoadProject
// ─────────────────────────────────────────────────────────────────
Status RendererServiceImpl::LoadProject(
    ServerContext* /*ctx*/,
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

  std::cout << "[project] loaded id=" << req->project_id()
            << " by=" << sess.username() << "\n";
  return Status::OK;
}

// ─────────────────────────────────────────────────────────────────
//  ListProjects
// ─────────────────────────────────────────────────────────────────
Status RendererServiceImpl::ListProjects(
    ServerContext* /*ctx*/,
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
    ServerContext* /*ctx*/,
    const renderer::DeleteProjectRequest* req,
    renderer::DeleteProjectResponse* resp)
{
  renderer::Session sess;
  if (auto s = auth(req->token(), &sess); !s.ok()) return s;

  const bool ok = projects_->remove(req->project_id(), sess.user_id());
  resp->set_success(ok);
  resp->set_message(ok ? "Deleted" : "Not found or access denied");

  if (ok) std::cout << "[project] deleted id=" << req->project_id()
                    << " by=" << sess.username() << "\n";
  return Status::OK;
}

// ─────────────────────────────────────────────────────────────────
//  RenameProject
// ─────────────────────────────────────────────────────────────────
Status RendererServiceImpl::RenameProject(
    ServerContext* /*ctx*/,
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
//  Note: actual PNG encoding would need a library like stb_image_write
//  or libpng. Here we return the raw RGBA bytes of the composited canvas
//  and mark the mime type so the client can encode if needed.
// ─────────────────────────────────────────────────────────────────
Status RendererServiceImpl::ExportProject(
    ServerContext* /*ctx*/,
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

  // Composite layers into a single RGBA buffer
  std::vector<uint8_t> composite(pixel_count * 4, 0);

  for (const auto& layer : proj->layers()) {
    if (!layer.visible()) continue;
    const auto& px = layer.pixels();
    if (px.size() != pixel_count * 4) continue;

    // Alpha-composite over existing buffer
    for (size_t i = 0; i < pixel_count; ++i) {
      const uint8_t sr = static_cast<uint8_t>(px[i*4]);
      const uint8_t sg = static_cast<uint8_t>(px[i*4+1]);
      const uint8_t sb = static_cast<uint8_t>(px[i*4+2]);
      const uint8_t sa = static_cast<uint8_t>(px[i*4+3]);

      uint8_t& dr = composite[i*4];
      uint8_t& dg = composite[i*4+1];
      uint8_t& db = composite[i*4+2];
      uint8_t& da = composite[i*4+3];

      // Porter-Duff source-over
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
  resp->set_mime_type("image/raw-rgba");  // client encodes to PNG/JPEG
  resp->set_message(std::to_string(W) + "x" + std::to_string(H) + " RGBA");

  std::cout << "[export] project=" << req->project_id()
            << " size=" << W << "x" << H << "\n";
  return Status::OK;
}

// ─────────────────────────────────────────────────────────────────
//  Collaborate  — bidirectional streaming
//
//  Flow:
//    1. Client sends first message with session_id + project_id
//    2. Server subscribes the stream to the project's CollabRoom
//    3. All subsequent messages from client are broadcast to others
//    4. Messages from others arrive via the room callback → Write()
// ─────────────────────────────────────────────────────────────────
Status RendererServiceImpl::Collaborate(
    ServerContext* ctx,
    grpc::ServerReaderWriter<renderer::CanvasUpdate,
                             renderer::CanvasUpdate>* stream)
{
  // Receive first message to identify session + project
  renderer::CanvasUpdate first;
  if (!stream->Read(&first))
    return Status(StatusCode::INVALID_ARGUMENT, "Expected initial message");

  auto sess = sessions_.validate(first.session_id());
  if (!sess)
    return Status(StatusCode::UNAUTHENTICATED, "Invalid session");

  const std::string& project_id = first.project_id();
  if (project_id.empty())
    return Status(StatusCode::INVALID_ARGUMENT, "project_id required");

  auto room = rooms_.get_or_create(project_id);

  // Mutex + CV to safely write from the room callback thread
  std::mutex write_mu;
  std::condition_variable write_cv;
  std::vector<renderer::CanvasUpdate> pending;
  bool done = false;

  const uint64_t sub_id = room->subscribe(sess->id(),
    [&](const renderer::CanvasUpdate& upd) {
      {
        std::lock_guard<std::mutex> lk(write_mu);
        pending.push_back(upd);
      }
      write_cv.notify_one();
    });

  ++active_streams_;
  std::cout << "[collab] user=" << sess->username()
            << " joined project=" << project_id
            << " active_streams=" << active_streams_ << "\n";

  // Writer thread: flush pending updates to client
  std::thread writer([&]() {
    while (!done && !ctx->IsCancelled()) {
      std::unique_lock<std::mutex> lk(write_mu);
      write_cv.wait_for(lk, std::chrono::milliseconds(100),
        [&]{ return !pending.empty() || done; });
      if (pending.empty()) continue;
      auto batch = std::move(pending);
      lk.unlock();
      for (auto& u : batch) {
        if (!stream->Write(u)) { done = true; break; }
      }
    }
  });

  // Broadcast first message
  room->broadcast(first, sub_id);

  // Read loop: client → room
  renderer::CanvasUpdate upd;
  while (!ctx->IsCancelled() && stream->Read(&upd)) {
    upd.set_user_id(sess->user_id());
    upd.set_username(sess->username());
    room->broadcast(upd, sub_id);
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
    grpc::ServerWriter<renderer::CanvasUpdate>* writer)
{
  renderer::Session sess;
  if (auto s = auth(req->token(), &sess); !s.ok()) return s;

  auto room = rooms_.get_or_create(req->project_id());

  std::mutex mu;
  std::condition_variable cv;
  std::vector<renderer::CanvasUpdate> pending;
  bool done = false;

  const uint64_t sub_id = room->subscribe(sess.id(),
    [&](const renderer::CanvasUpdate& upd) {
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
    for (auto& u : batch) {
      if (!writer->Write(u)) { done = true; break; }
    }
  }

  room->unsubscribe(sub_id);
  return Status::OK;
}
