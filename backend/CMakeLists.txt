cmake_minimum_required(VERSION 3.20)
project(renderer_backend CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─────────────────────────────────────────────────────────────────
#  Find gRPC + Protobuf
#  Install via: vcpkg install grpc  OR  brew install grpc
# ─────────────────────────────────────────────────────────────────
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC     CONFIG REQUIRED)

# Resolve protoc + grpc_cpp_plugin binaries
set(PROTOC       $<TARGET_FILE:protobuf::protoc>)
set(GRPC_PLUGIN  $<TARGET_FILE:gRPC::grpc_cpp_plugin>)

# ─────────────────────────────────────────────────────────────────
#  Generate protobuf / gRPC sources from .proto
# ─────────────────────────────────────────────────────────────────
set(PROTO_DIR    "${CMAKE_SOURCE_DIR}/proto")
set(PROTO_FILE   "${PROTO_DIR}/renderer.proto")
set(PROTO_OUT    "${CMAKE_BINARY_DIR}/proto")

file(MAKE_DIRECTORY "${PROTO_OUT}")

add_custom_command(
  OUTPUT
    "${PROTO_OUT}/renderer.pb.cc"
    "${PROTO_OUT}/renderer.pb.h"
    "${PROTO_OUT}/renderer.grpc.pb.cc"
    "${PROTO_OUT}/renderer.grpc.pb.h"
  COMMAND ${PROTOC}
    --proto_path="${PROTO_DIR}"
    --cpp_out="${PROTO_OUT}"
    --grpc_out="${PROTO_OUT}"
    --plugin=protoc-gen-grpc="${GRPC_PLUGIN}"
    "${PROTO_FILE}"
  DEPENDS "${PROTO_FILE}"
  COMMENT "Generating protobuf/gRPC C++ sources"
)

# Convenience library for generated code
add_library(proto_gen STATIC
  "${PROTO_OUT}/renderer.pb.cc"
  "${PROTO_OUT}/renderer.grpc.pb.cc"
)
target_include_directories(proto_gen PUBLIC "${PROTO_OUT}")
target_link_libraries(proto_gen PUBLIC
  protobuf::libprotobuf
  gRPC::grpc++
)

# ─────────────────────────────────────────────────────────────────
#  Server executable
# ─────────────────────────────────────────────────────────────────
add_executable(renderer_server
  src/server.cpp
  src/service.cpp
  src/storage.cpp
)
target_include_directories(renderer_server PRIVATE "${PROTO_OUT}")
target_link_libraries(renderer_server PRIVATE
  proto_gen
  gRPC::grpc++
  gRPC::grpc++_reflection
  protobuf::libprotobuf
)

# ─────────────────────────────────────────────────────────────────
#  Client executable
# ─────────────────────────────────────────────────────────────────
add_executable(renderer_client
  src/client.cpp
)
target_include_directories(renderer_client PRIVATE "${PROTO_OUT}")
target_link_libraries(renderer_client PRIVATE
  proto_gen
  gRPC::grpc++
  protobuf::libprotobuf
)

# ─────────────────────────────────────────────────────────────────
#  Compiler flags
# ─────────────────────────────────────────────────────────────────
if(MSVC)
  target_compile_options(renderer_server PRIVATE /W4 /WX-)
  target_compile_options(renderer_client PRIVATE /W4 /WX-)
else()
  set(WARN_FLAGS -Wall -Wextra -Wpedantic -Wno-unused-parameter)
  target_compile_options(renderer_server PRIVATE ${WARN_FLAGS})
  target_compile_options(renderer_client PRIVATE ${WARN_FLAGS})
endif()

# ─────────────────────────────────────────────────────────────────
#  Install
# ─────────────────────────────────────────────────────────────────
install(TARGETS renderer_server renderer_client
  RUNTIME DESTINATION bin
)
