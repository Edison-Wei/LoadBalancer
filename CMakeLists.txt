cmake_minimum_required(VERSION 3.28)
project(LoadBalancer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(FetchContent)
find_package(Threads REQUIRED)
include(cmake/Sanitizers.cmake)

option(LB_FORCE_MUTEX_SHARED_PTR "Exercise the mutex snapshot fallback on any platform" OFF)
add_library(lb_snapshot INTERFACE)
target_include_directories(lb_snapshot INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(lb_snapshot INTERFACE cxx_std_20)
target_link_libraries(lb_snapshot INTERFACE Threads::Threads lb_sanitizers)
if(LB_FORCE_MUTEX_SHARED_PTR)
  target_compile_definitions(lb_snapshot INTERFACE LB_FORCE_MUTEX_SHARED_PTR=1)
endif()

# One CMake-ready Boost archive supplies Asio, Beast, and their dependencies.
# Version pins match the roadmap. Archive hashes can be added after download validation.
set(BOOST_INCLUDE_LIBRARIES "asio;beast;system")
set(BOOST_ENABLE_CMAKE ON)
FetchContent_Declare(
  Boost
  URL https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(Boost)

add_library(lb_core src/foundation.cpp src/registry.cpp src/config.cpp src/proxy_http.cpp
  src/upstream_proxy.cpp src/client_session.cpp src/listener.cpp)
target_include_directories(lb_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(lb_core PUBLIC cxx_std_20)
target_link_libraries(lb_core PUBLIC Boost::asio Boost::beast lb_snapshot)

# --smoke retains the Phase 0 foundation check; ordinary startup runs the proxy.
add_executable(load_balancer src/main.cpp)
target_link_libraries(load_balancer PRIVATE lb_core)

add_library(lb_test_backend apps/test_backend_handlers.cpp apps/test_backend_server.cpp)
target_link_libraries(lb_test_backend PUBLIC Boost::asio Boost::beast lb_snapshot)
add_executable(test_backend apps/test_backend.cpp)
target_link_libraries(test_backend PRIVATE lb_test_backend)

if(BUILD_TESTING)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.18.0
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(googletest)

  add_executable(lb_tests
    tests/unit/foundation_test.cpp
    tests/unit/round_robin_test.cpp
    tests/unit/registry_test.cpp
    tests/unit/test_backend_test.cpp
    tests/unit/proxy_http_test.cpp
  )
  target_link_libraries(lb_tests PRIVATE lb_core lb_test_backend GTest::gtest_main)
  include(GoogleTest)
  # Discovery at test time also works with instrumented executables.
  gtest_discover_tests(lb_tests DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 10)
  add_test(NAME foundation_cli COMMAND load_balancer --smoke)
  set_tests_properties(foundation_cli PROPERTIES TIMEOUT 10)

  add_executable(snapshot_portability_test tests/unit/snapshot_portability_test.cpp)
  target_link_libraries(snapshot_portability_test PRIVATE lb_snapshot)
  add_test(NAME snapshot_portability COMMAND snapshot_portability_test)
  set_tests_properties(snapshot_portability PROPERTIES TIMEOUT 30)

  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME test_backend_process
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_backend_process.py"
      --backend "$<TARGET_FILE:test_backend>"
      --logs "${CMAKE_CURRENT_BINARY_DIR}/Testing/backend-process")
  set_tests_properties(test_backend_process PROPERTIES TIMEOUT 45 LABELS integration)
  add_test(NAME proxy_process
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/proxy_process.py"
      --backend "$<TARGET_FILE:test_backend>" --proxy "$<TARGET_FILE:load_balancer>"
      --logs "${CMAKE_CURRENT_BINARY_DIR}/Testing/proxy-process")
  set_tests_properties(proxy_process PROPERTIES TIMEOUT 60 LABELS integration)
  add_test(NAME test_backend_help COMMAND test_backend --help)
  set_tests_properties(test_backend_help PROPERTIES TIMEOUT 10)
  add_test(NAME load_balancer_help COMMAND load_balancer --help)
  set_tests_properties(load_balancer_help PROPERTIES TIMEOUT 10)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  foreach(lb_target IN ITEMS lb_core load_balancer lb_tests snapshot_portability_test lb_test_backend test_backend)
    if(TARGET ${lb_target})
      target_compile_options(${lb_target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
  endforeach()
endif()
