#pragma once

#include <memory>
#include <atomic>

#include <grpcpp/grpcpp.h>
#include "../proto/renderer.grpc.pb.h"
#include "storage.h"

// ─────────────────────────────────────────────────────────────────
//  RendererServiceImpl
//  Implements all RPCs defined in renderer.proto
// ─────────────────────────────────────────────────────────────────
class RendererServiceImpl final : public renderer::RendererService::Service {
public:
  explicit RendererServiceImpl(const std::string& storage_root);

  // ── Session ──────────────────────────────────────────────────
  grpc::Status CreateSession(
      grpc::ServerContext* ctx,
      const renderer::CreateSessionRequest* req,
      renderer::CreateSessionResponse* resp) override;

  grpc::Status ValidateSession(
      grpc::ServerContext* ctx,
      const renderer::ValidateSessionRequest* req,
      renderer::ValidateSessionResponse* resp) override;

  // ── Projects ─────────────────────────────────────────────────
  grpc::Status SaveProject(
      grpc::ServerContext* ctx,
      const renderer::SaveProjectRequest* req,
      renderer::SaveProjectResponse* resp) override;

  grpc::Status LoadProject(
      grpc::ServerContext* ctx,
      const renderer::LoadProjectRequest* req,
      renderer::LoadProjectResponse* resp) override;

  grpc::Status ListProjects(
      grpc::ServerContext* ctx,
      const renderer::ListProjectsRequest* req,
      renderer::ListProjectsResponse* resp) override;

  grpc::Status DeleteProject(
      grpc::ServerContext* ctx,
      const renderer::DeleteProjectRequest* req,
      renderer::DeleteProjectResponse* resp) override;

  grpc::Status RenameProject(
      grpc::ServerContext* ctx,
      const renderer::RenameProjectRequest* req,
      renderer::RenameProjectResponse* resp) override;

  // ── Export ───────────────────────────────────────────────────
  grpc::Status ExportProject(
      grpc::ServerContext* ctx,
      const renderer::ExportRequest* req,
      renderer::ExportResponse* resp) override;

  // ── Real-time collaboration ───────────────────────────────────
  grpc::Status Collaborate(
      grpc::ServerContext* ctx,
      grpc::ServerReaderWriter<renderer::CanvasUpdate,
                               renderer::CanvasUpdate>* stream) override;

  grpc::Status JoinRoom(
      grpc::ServerContext* ctx,
      const renderer::JoinRoomRequest* req,
      grpc::ServerWriter<renderer::CanvasUpdate>* writer) override;

private:
  SessionStore                sessions_;
  std::unique_ptr<ProjectStore> projects_;
  RoomRegistry                rooms_;

  // Validate token, populate session; returns error status on failure
  grpc::Status auth(const std::string& token,
                    renderer::Session* out_session);

  std::atomic<uint64_t> active_streams_{ 0 };
};
