#include <iostream>
#include <string>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>

#include "service.h"

// ─────────────────────────────────────────────────────────────────
//  Configuration (override via env vars)
// ─────────────────────────────────────────────────────────────────
struct Config {
  std::string host         = "0.0.0.0";
  int         port         = 50051;
  std::string storage_root = "./storage";
  int         max_recv_mb  = 64;   // max incoming message size
  int         max_send_mb  = 64;
};

Config load_config() {
  Config c;
  auto env = [](const char* k, const std::string& def) -> std::string {
    const char* v = std::getenv(k);
    return v ? v : def;
  };
  c.host         = env("RENDERER_HOST",    c.host);
  c.port         = std::stoi(env("RENDERER_PORT",    std::to_string(c.port)));
  c.storage_root = env("RENDERER_STORAGE", c.storage_root);
  c.max_recv_mb  = std::stoi(env("RENDERER_MAX_RECV_MB", std::to_string(c.max_recv_mb)));
  c.max_send_mb  = std::stoi(env("RENDERER_MAX_SEND_MB", std::to_string(c.max_send_mb)));
  return c;
}

// ─────────────────────────────────────────────────────────────────
//  Graceful shutdown
// ─────────────────────────────────────────────────────────────────
static std::atomic<bool> g_shutdown{ false };
static grpc::Server*     g_server   = nullptr;

void signal_handler(int sig) {
  std::cout << "\n[server] received signal " << sig
            << ", initiating graceful shutdown...\n";
  g_shutdown = true;
  if (g_server) {
    // Give in-flight RPCs 5 seconds to complete
    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
    g_server->Shutdown(deadline);
  }
}

// ─────────────────────────────────────────────────────────────────
//  Interceptor: request logger
// ─────────────────────────────────────────────────────────────────
class LoggingInterceptor : public grpc::experimental::Interceptor {
public:
  explicit LoggingInterceptor(grpc::experimental::ServerRpcInfo* info)
    : method_(info->method()), start_(std::chrono::steady_clock::now()) {}

  void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override {
    if (methods->QueryInterceptionHookPoint(
          grpc::experimental::InterceptionHookPoints::POST_SEND_MESSAGE)) {
      auto dur = std::chrono::steady_clock::now() - start_;
      auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
      std::cout << "[rpc] " << method_ << " completed in " << ms << "ms\n";
    }
    methods->Proceed();
  }

private:
  std::string method_;
  std::chrono::steady_clock::time_point start_;
};

class LoggingInterceptorFactory
    : public grpc::experimental::ServerInterceptorFactoryInterface {
public:
  grpc::experimental::Interceptor* CreateServerInterceptor(
      grpc::experimental::ServerRpcInfo* info) override {
    return new LoggingInterceptor(info);
  }
};

// ─────────────────────────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────────────────────────
int main() {
  const Config cfg = load_config();

  std::signal(SIGINT,  signal_handler);
  std::signal(SIGTERM, signal_handler);

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();

  grpc::ServerBuilder builder;

  // Listening address
  const std::string addr = cfg.host + ":" + std::to_string(cfg.port);
  builder.AddListeningPort(addr, grpc::InsecureServerCredentials());

  // Message size limits (important for large pixel data)
  builder.SetMaxReceiveMessageSize(cfg.max_recv_mb * 1024 * 1024);
  builder.SetMaxSendMessageSize(cfg.max_send_mb    * 1024 * 1024);

  // Keep-alive settings for long-lived collaborative streams
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS,             30'000);
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS,          10'000);
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
  builder.AddChannelArgument(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA,   0);

  // Register interceptors
  std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>> creators;
  creators.push_back(std::make_unique<LoggingInterceptorFactory>());
  builder.experimental().SetInterceptorCreators(std::move(creators));

  // Register service
  RendererServiceImpl service(cfg.storage_root);
  builder.RegisterService(&service);

  // Build and start
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  g_server = server.get();

  if (!server) {
    std::cerr << "[server] failed to start on " << addr << "\n";
    return 1;
  }

  std::cout << "╔══════════════════════════════════════╗\n"
            << "║     2D Renderer gRPC Backend         ║\n"
            << "╠══════════════════════════════════════╣\n"
            << "║  Address : " << addr << "\n"
            << "║  Storage : " << cfg.storage_root << "\n"
            << "║  MaxRecv : " << cfg.max_recv_mb << " MB\n"
            << "║  MaxSend : " << cfg.max_send_mb << " MB\n"
            << "╚══════════════════════════════════════╝\n";

  server->Wait();

  std::cout << "[server] shut down cleanly.\n";
  return 0;
}
