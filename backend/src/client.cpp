/**
 * Test client — exercises all RPC endpoints.
 * Usage:  ./renderer_client [host:port]
 */

#include <iostream>
#include <string>
#include <thread>
#include <chrono>

#include <grpcpp/grpcpp.h>
#include "../proto/renderer.grpc.pb.h"

class RendererClient {
public:
  explicit RendererClient(std::shared_ptr<grpc::Channel> channel)
    : stub_(renderer::RendererService::NewStub(channel)) {}

  // ── CreateSession ────────────────────────────────────────────
  bool create_session(const std::string& username) {
    renderer::CreateSessionRequest req;
    req.set_username(username);

    renderer::CreateSessionResponse resp;
    grpc::ClientContext ctx;
    const auto status = stub_->CreateSession(&ctx, req, &resp);
    if (!status.ok()) {
      std::cerr << "CreateSession error: " << status.error_message() << "\n";
      return false;
    }
    token_   = resp.token();
    user_id_ = resp.session().user_id();
    std::cout << "[client] session created  user=" << username
              << "  id=" << resp.session().id() << "\n";
    return true;
  }

  // ── ValidateSession ──────────────────────────────────────────
  bool validate_session() {
    renderer::ValidateSessionRequest req;
    req.set_token(token_);
    renderer::ValidateSessionResponse resp;
    grpc::ClientContext ctx;
    stub_->ValidateSession(&ctx, req, &resp);
    std::cout << "[client] session valid=" << resp.valid() << "\n";
    return resp.valid();
  }

  // ── SaveProject ──────────────────────────────────────────────
  std::string save_project(const std::string& name,
                           uint32_t W = 800, uint32_t H = 600) {
    renderer::SaveProjectRequest req;
    req.set_token(token_);

    renderer::Project& proj = *req.mutable_project();
    proj.set_name(name);
    proj.set_canvas_width(W);
    proj.set_canvas_height(H);

    // Create two dummy layers with solid-colour pixel data
    auto make_layer = [&](const std::string& lname,
                          uint8_t r, uint8_t g, uint8_t b) {
      renderer::Layer* layer = proj.add_layers();
      layer->set_name(lname);
      layer->set_visible(true);
      layer->set_locked(false);
      layer->set_width(W);
      layer->set_height(H);

      std::string pixels(static_cast<size_t>(W) * H * 4, '\0');
      for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) {
        pixels[i*4]   = static_cast<char>(r);
        pixels[i*4+1] = static_cast<char>(g);
        pixels[i*4+2] = static_cast<char>(b);
        pixels[i*4+3] = static_cast<char>(255);
      }
      layer->set_pixels(pixels);
    };

    make_layer("Background", 255, 255, 255);
    make_layer("Layer 1",    100, 149, 237);

    renderer::SaveProjectResponse resp;
    grpc::ClientContext ctx;
    const auto status = stub_->SaveProject(&ctx, req, &resp);
    if (!status.ok()) {
      std::cerr << "SaveProject error: " << status.error_message() << "\n";
      return "";
    }
    std::cout << "[client] project saved  id=" << resp.project_id()
              << "  msg=" << resp.message() << "\n";
    return resp.project_id();
  }

  // ── LoadProject ──────────────────────────────────────────────
  void load_project(const std::string& id) {
    renderer::LoadProjectRequest req;
    req.set_token(token_);
    req.set_project_id(id);

    renderer::LoadProjectResponse resp;
    grpc::ClientContext ctx;
    const auto status = stub_->LoadProject(&ctx, req, &resp);
    if (!status.ok()) {
      std::cerr << "LoadProject error: " << status.error_message() << "\n";
      return;
    }
    const auto& p = resp.project();
    std::cout << "[client] loaded project  name=" << p.name()
              << "  size=" << p.canvas_width() << "x" << p.canvas_height()
              << "  layers=" << p.layers_size() << "\n";
  }

  // ── ListProjects ─────────────────────────────────────────────
  void list_projects() {
    renderer::ListProjectsRequest req;
    req.set_token(token_);
    req.set_page(0);
    req.set_per_page(10);

    renderer::ListProjectsResponse resp;
    grpc::ClientContext ctx;
    stub_->ListProjects(&ctx, req, &resp);
    std::cout << "[client] total projects: " << resp.total() << "\n";
    for (const auto& m : resp.projects())
      std::cout << "  - " << m.id() << "  " << m.name()
                << "  layers=" << m.layer_count() << "\n";
  }

  // ── RenameProject ────────────────────────────────────────────
  void rename_project(const std::string& id, const std::string& new_name) {
    renderer::RenameProjectRequest req;
    req.set_token(token_);
    req.set_project_id(id);
    req.set_new_name(new_name);

    renderer::RenameProjectResponse resp;
    grpc::ClientContext ctx;
    stub_->RenameProject(&ctx, req, &resp);
    std::cout << "[client] rename  success=" << resp.success()
              << "  msg=" << resp.message() << "\n";
  }

  // ── ExportProject ────────────────────────────────────────────
  void export_project(const std::string& id) {
    renderer::ExportRequest req;
    req.set_token(token_);
    req.set_project_id(id);
    req.set_format("png");

    renderer::ExportResponse resp;
    grpc::ClientContext ctx;
    stub_->ExportProject(&ctx, req, &resp);
    std::cout << "[client] export  success=" << resp.success()
              << "  bytes=" << resp.data().size()
              << "  info=" << resp.message() << "\n";
  }

  // ── Collaborate (bidi stream demo) ───────────────────────────
  void collaborate_demo(const std::string& project_id) {
    grpc::ClientContext ctx;
    auto stream = stub_->Collaborate(&ctx);

    // Send a STROKE_BEGIN
    renderer::CanvasUpdate upd;
    upd.set_session_id(token_);   // reuse token as session id in demo
    upd.set_project_id(project_id);
    upd.set_type(renderer::STROKE_BEGIN);
    upd.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());

    renderer::StrokeData* stroke = upd.mutable_stroke();
    stroke->set_tool("pen");
    stroke->mutable_stroke_color()->set_r(255);
    stroke->mutable_stroke_color()->set_a(1.0f);
    stroke->set_size(4);
    stroke->set_opacity(1.0f);
    auto* pt = stroke->add_points();
    pt->set_x(100); pt->set_y(150);

    stream->Write(upd);

    // In a real client this would be a loop; close write side after demo
    stream->WritesDone();

    renderer::CanvasUpdate recv;
    while (stream->Read(&recv))
      std::cout << "[client] bidi recv from user=" << recv.username() << "\n";

    auto status = stream->Finish();
    std::cout << "[client] collaborate stream finished  ok="
              << status.ok() << "\n";
  }

  // ── DeleteProject ────────────────────────────────────────────
  void delete_project(const std::string& id) {
    renderer::DeleteProjectRequest req;
    req.set_token(token_);
    req.set_project_id(id);

    renderer::DeleteProjectResponse resp;
    grpc::ClientContext ctx;
    stub_->DeleteProject(&ctx, req, &resp);
    std::cout << "[client] delete  success=" << resp.success()
              << "  msg=" << resp.message() << "\n";
  }

private:
  std::unique_ptr<renderer::RendererService::Stub> stub_;
  std::string token_;
  std::string user_id_;
};

// ─────────────────────────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
  const std::string addr = argc > 1 ? argv[1] : "localhost:50051";
  std::cout << "[client] connecting to " << addr << "\n\n";

  grpc::ChannelArguments args;
  args.SetMaxReceiveMessageSize(64 * 1024 * 1024);
  args.SetMaxSendMessageSize(64 * 1024 * 1024);

  RendererClient client(grpc::CreateCustomChannel(
      addr, grpc::InsecureChannelCredentials(), args));

  // 1. Create session
  if (!client.create_session("alice")) return 1;

  // 2. Validate session
  client.validate_session();

  // 3. Save a project
  const std::string pid = client.save_project("My Canvas", 800, 600);
  if (pid.empty()) return 1;

  // 4. Load it back
  client.load_project(pid);

  // 5. List all projects
  client.list_projects();

  // 6. Rename
  client.rename_project(pid, "My Canvas (renamed)");

  // 7. Export
  client.export_project(pid);

  // 8. Bidi stream demo
  client.collaborate_demo(pid);

  // 9. Delete
  client.delete_project(pid);

  // 10. Confirm deletion
  client.list_projects();

  return 0;
}
