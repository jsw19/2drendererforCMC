#pragma once

#include <memory>
#include <atomic>

#include <grpcpp/grpcpp.h>
#include "../proto/renderer.grpc.pb.h"
#include "storage.h"
#include "redis_client.h"

// ─────────────────────────────────────────────────────────────────
//  RendererServiceImpl
// ─────────────────────────────────────────────────────────────────
class RendererServiceImpl final : public renderer::RendererService::Service {
public:
  RendererServiceImpl(const std::string& storage_root,
                      const std::string& redis_host,
                      int                redis_port);

  // ── Session ──────────────────────────────────────────────────
  grpc::Status CreateSession(
      grpc::ServerContext*, const renderer::CreateSessionRequest*,
      renderer::CreateSessionResponse*) override;

  grpc::Status ValidateSession(
      grpc::ServerContext*, const renderer::ValidateSessionRequest*,
      renderer::ValidateSessionResponse*) override;

  // ── Projects ─────────────────────────────────────────────────
  grpc::Status SaveProject(
      grpc::ServerContext*, const renderer::SaveProjectRequest*,
      renderer::SaveProjectResponse*) override;

  grpc::Status LoadProject(
      grpc::ServerContext*, const renderer::LoadProjectRequest*,
      renderer::LoadProjectResponse*) override;

  grpc::Status ListProjects(
      grpc::ServerContext*, const renderer::ListProjectsRequest*,
      renderer::ListProjectsResponse*) override;

  grpc::Status DeleteProject(
      grpc::ServerContext*, const renderer::DeleteProjectRequest*,
      renderer::DeleteProjectResponse*) override;

  grpc::Status RenameProject(
      grpc::ServerContext*, const renderer::RenameProjectRequest*,
      renderer::RenameProjectResponse*) override;

  // ── Export ───────────────────────────────────────────────────
  grpc::Status ExportProject(
      grpc::ServerContext*, const renderer::ExportRequest*,
      renderer::ExportResponse*) override;

  // ── Real-time collaboration ───────────────────────────────────
  grpc::Status Collaborate(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<renderer::CanvasUpdate,
                               renderer::CanvasUpdate>*) override;

  grpc::Status JoinRoom(
      grpc::ServerContext*, const renderer::JoinRoomRequest*,
      grpc::ServerWriter<renderer::CanvasUpdate>*) override;

private:
  RedisStore                    redis_;
  SessionStore                  sessions_;
  std::unique_ptr<ProjectStore> projects_;
  RoomRegistry                  rooms_;

  alignas(64) std::atomic<uint64_t> active_streams_{ 0 };

  grpc::Status auth(const std::string& token,
                    renderer::Session* out_session);
};
