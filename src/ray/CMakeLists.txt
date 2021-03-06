cmake_minimum_required(VERSION 3.2)
set(CMAKE_CXX_FLAGS "-g -Wall -Werror -std=c++11")

include_directories(${CMAKE_CURRENT_LIST_DIR}/../common/thirdparty/ae)

add_subdirectory(util)
add_subdirectory(gcs)
add_subdirectory(object_manager)
add_subdirectory(raylet)

include(${CMAKE_CURRENT_LIST_DIR}/../common/cmake/Common.cmake)

set(AE_SRCS
  ${CMAKE_CURRENT_LIST_DIR}/../common/thirdparty/ae/ae.c
)

set(HIREDIS_SRCS
  ${CMAKE_CURRENT_LIST_DIR}/../common/thirdparty/hiredis/async.c
  ${CMAKE_CURRENT_LIST_DIR}/../common/thirdparty/hiredis/dict.c
  ${CMAKE_CURRENT_LIST_DIR}/../common/thirdparty/hiredis/hiredis.c
  ${CMAKE_CURRENT_LIST_DIR}/../common/thirdparty/hiredis/net.c
  ${CMAKE_CURRENT_LIST_DIR}/../common/thirdparty/hiredis/read.c
  ${CMAKE_CURRENT_LIST_DIR}/../common/thirdparty/hiredis/sds.c
)

set(UTIL_SRCS
  ${CMAKE_CURRENT_LIST_DIR}/../common/thirdparty/sha256.c
  ${CMAKE_CURRENT_LIST_DIR}/../common/common_protocol.cc
)

set(RAY_SRCS
  id.cc
  status.cc
  gcs/client.cc
  gcs/tables.cc
  gcs/redis_context.cc
  gcs/asio.cc
  common/client_connection.cc
  object_manager/object_manager_client_connection.cc
  object_manager/object_store_client.cc
  object_manager/object_directory.cc
  object_manager/object_manager.cc
  raylet/mock_gcs_client.cc
  raylet/task.cc
  raylet/task_execution_spec.cc
  raylet/task_spec.cc
  raylet/worker.cc
  raylet/worker_pool.cc
  raylet/scheduling_resources.cc
  raylet/actor.cc
  raylet/scheduling_queue.cc
  raylet/scheduling_policy.cc
  raylet/task_dependency_manager.cc
  raylet/reconstruction_policy.cc
  raylet/node_manager.cc
  raylet/lineage_cache.cc
  raylet/raylet.cc
)

install(FILES
  api.h
  id.h
  status.h
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ray")

# pkg-config support
configure_file(ray.pc.in
  "${CMAKE_CURRENT_BINARY_DIR}/ray.pc"
  @ONLY)
install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/ray.pc"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig/")

ADD_RAY_LIB(ray
  SOURCES ${RAY_SRCS} ${AE_SRCS} ${HIREDIS_SRCS} ${UTIL_SRCS}
    DEPENDENCIES gen_gcs_fbs gen_object_manager_fbs gen_node_manager_fbs
    SHARED_LINK_LIBS ""
    STATIC_LINK_LIBS ${PLASMA_STATIC_LIB} ${ARROW_STATIC_LIB})
