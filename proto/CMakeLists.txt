# gRPC 接口定义。
#
# 历史说明：早期的 pir_comm.proto（表上传接口）已随 src/common、src/client、
# src/server 一并移除（决策 D9）。当前只有 vmpq.proto 一个协议文件，
# 服务于 src/apps 的端到端 demo 与真实 gRPC 通道。

# protoc / grpc_cpp_plugin：默认取依赖前缀下的可执行文件；找不到就退回 PATH，
# 都找不到则给出**明确**报错（而不是让自定义命令在构建期神秘失败）。
set(_PROTOBUF_PROTOC "${TSB_DEPS_PREFIX}/bin/protoc" CACHE FILEPATH "protoc 可执行文件")
set(_GRPC_CPP_PLUGIN "${TSB_DEPS_PREFIX}/bin/grpc_cpp_plugin" CACHE FILEPATH "grpc_cpp_plugin 可执行文件")
if(NOT EXISTS "${_PROTOBUF_PROTOC}")
    find_program(_TSB_PROTOC_IN_PATH NAMES protoc)
    if(_TSB_PROTOC_IN_PATH)
        set(_PROTOBUF_PROTOC "${_TSB_PROTOC_IN_PATH}")
        message(STATUS "protoc 未在前缀下找到，改用 PATH：${_PROTOBUF_PROTOC}")
    else()
        message(FATAL_ERROR
            "找不到 protoc。请用 -DTSB_DEPS_PREFIX=<依赖前缀>（其中含 bin/protoc），"
            "或 -D_PROTOBUF_PROTOC=<绝对路径>，或把它放进 PATH。")
    endif()
endif()
if(NOT EXISTS "${_GRPC_CPP_PLUGIN}")
    find_program(_TSB_GRPC_PLUGIN_IN_PATH NAMES grpc_cpp_plugin)
    if(_TSB_GRPC_PLUGIN_IN_PATH)
        set(_GRPC_CPP_PLUGIN "${_TSB_GRPC_PLUGIN_IN_PATH}")
        message(STATUS "grpc_cpp_plugin 未在前缀下找到，改用 PATH：${_GRPC_CPP_PLUGIN}")
    else()
        message(FATAL_ERROR
            "找不到 grpc_cpp_plugin。请用 -DTSB_DEPS_PREFIX=<依赖前缀>（其中含 bin/grpc_cpp_plugin），"
            "或 -D_GRPC_CPP_PLUGIN=<绝对路径>，或把它放进 PATH。")
    endif()
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/gen/vmpq")
set(VMPQ_PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/vmpq.proto")
set(VMPQ_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gen/vmpq")

add_custom_command(
    OUTPUT
        "${VMPQ_GEN_DIR}/vmpq.pb.cc"
        "${VMPQ_GEN_DIR}/vmpq.pb.h"
        "${VMPQ_GEN_DIR}/vmpq.grpc.pb.cc"
        "${VMPQ_GEN_DIR}/vmpq.grpc.pb.h"
    COMMAND ${_PROTOBUF_PROTOC}
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
        --cpp_out=${VMPQ_GEN_DIR}
        --grpc_out=${VMPQ_GEN_DIR}
        --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN}
        ${VMPQ_PROTO_FILE}
    DEPENDS ${VMPQ_PROTO_FILE}
    COMMENT "Generating C++/gRPC sources for vmpq"
)

add_library(vmpq_proto
    "${VMPQ_GEN_DIR}/vmpq.grpc.pb.cc"
    "${VMPQ_GEN_DIR}/vmpq.pb.cc"
)
target_include_directories(vmpq_proto PUBLIC ${VMPQ_GEN_DIR})
target_link_libraries(vmpq_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)

# ---- MPRAQ 协议（MPA-08 的传输层；与 vmpq 完全独立的包/服务）----
#
# ⚠️ 生成目录必须与 vmpq **分开**：protoc 的输出文件名只取决于 .proto 的文件名，
#    两个包共用同一个 `--cpp_out` 目录会让 vmpq.pb.cc ←→ mpraq.pb.cc 互相覆盖式
#    的构建规则冲突（"multiple rules generate the same file"）。
file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/gen/mpraq")
set(MPRAQ_PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/mpraq.proto")
set(MPRAQ_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gen/mpraq")

add_custom_command(
    OUTPUT
        "${MPRAQ_GEN_DIR}/mpraq.pb.cc"
        "${MPRAQ_GEN_DIR}/mpraq.pb.h"
        "${MPRAQ_GEN_DIR}/mpraq.grpc.pb.cc"
        "${MPRAQ_GEN_DIR}/mpraq.grpc.pb.h"
    COMMAND ${_PROTOBUF_PROTOC}
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
        --cpp_out=${MPRAQ_GEN_DIR}
        --grpc_out=${MPRAQ_GEN_DIR}
        --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN}
        ${MPRAQ_PROTO_FILE}
    DEPENDS ${MPRAQ_PROTO_FILE}
    COMMENT "Generating C++/gRPC sources for mpraq"
)

add_library(mpraq_proto
    "${MPRAQ_GEN_DIR}/mpraq.grpc.pb.cc"
    "${MPRAQ_GEN_DIR}/mpraq.pb.cc"
)
target_include_directories(mpraq_proto PUBLIC ${MPRAQ_GEN_DIR})
target_link_libraries(mpraq_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)
