cmake_minimum_required(VERSION 3.20)
project(botguard VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(botguard_core
  src/risk/risk_engine.cc
  src/risk/order_notional.cc
  src/risk/order_registry.cc
  src/risk/order_gate.cc
  src/execution/order_state_store.cc
  src/execution/order_submission_coordinator.cc
  src/execution/reconciliation_coordinator.cc
  src/execution/startup_recovery.cc
  src/execution/venue_startup_reconciler.cc
)
target_include_directories(botguard_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(botguard_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

add_executable(botguard_demo src/app/main.cc)
target_link_libraries(botguard_demo PRIVATE botguard_core)

add_library(botguard_agent
  src/agent/uds_protocol.cc
  src/agent/uds_agent_server.cc
  src/agent/uds_client.cc
)
target_include_directories(botguard_agent PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(botguard_agent PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
target_link_libraries(botguard_agent PUBLIC botguard_core)

option(BOTGUARD_BUILD_SQLITE_STORE "Build the durable SQLite order-state store" ON)
if(BOTGUARD_BUILD_SQLITE_STORE)
  find_package(SQLite3 3.24 REQUIRED)
  add_library(botguard_sqlite_store
    src/execution/runtime_lease.cc
    src/execution/sqlite_order_state_store.cc
    src/execution/sqlite_runtime_state_store.cc
    src/execution/sqlite_storage_pair.cc
  )
  target_include_directories(botguard_sqlite_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_options(botguard_sqlite_store PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  target_link_libraries(botguard_sqlite_store PUBLIC botguard_core PRIVATE SQLite::SQLite3)
endif()

option(BOTGUARD_BUILD_KALSHI_ADAPTER "Build the Kalshi REST adapter" OFF)
if(BOTGUARD_BUILD_KALSHI_ADAPTER)
  find_package(CURL REQUIRED)
  find_package(OpenSSL REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
  add_library(botguard_kalshi_adapter
    src/execution/kalshi/codec.cc
    src/execution/kalshi/signed_http_client.cc
    src/execution/kalshi_adapter.cc
    src/execution/kalshi_startup_reconciler.cc
  )
  target_include_directories(botguard_kalshi_adapter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_options(botguard_kalshi_adapter PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  target_link_libraries(botguard_kalshi_adapter PUBLIC botguard_core PRIVATE CURL::libcurl OpenSSL::Crypto PkgConfig::JSONCPP)
endif()

if(BOTGUARD_BUILD_SQLITE_STORE AND BOTGUARD_BUILD_KALSHI_ADAPTER)
  add_executable(botguard_kalshi_demo src/app/kalshi_demo_main.cc)
  target_compile_options(botguard_kalshi_demo PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  target_link_libraries(botguard_kalshi_demo PRIVATE botguard_kalshi_adapter botguard_sqlite_store OpenSSL::Crypto)
endif()

if(BOTGUARD_BUILD_SQLITE_STORE)
  add_library(botguard_demo_support
    src/demo/persistent_fault_injecting_venue.cc
    src/demo/simulator_runtime.cc
  )
  target_include_directories(botguard_demo_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_options(botguard_demo_support PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  target_link_libraries(botguard_demo_support PUBLIC botguard_agent botguard_sqlite_store PRIVATE SQLite::SQLite3)

  add_executable(botguard_failure_demo src/app/failure_demo_main.cc)
  target_compile_options(botguard_failure_demo PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  target_link_libraries(botguard_failure_demo PRIVATE botguard_demo_support botguard_sqlite_store SQLite::SQLite3)

  add_executable(botguard_local_agent src/app/local_agent_main.cc)
  target_compile_options(botguard_local_agent PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  target_link_libraries(botguard_local_agent PRIVATE botguard_demo_support botguard_agent)
endif()

option(BOTGUARD_BUILD_TESTS "Build BotGuard unit tests" ON)
if(BOTGUARD_BUILD_TESTS)
  include(CTest)
  find_package(GTest REQUIRED)
  add_executable(botguard_tests
    tests/agent/uds_protocol_tests.cc
    tests/risk/risk_engine_tests.cc
    tests/risk/order_registry_tests.cc
    tests/risk/order_gate_tests.cc
    tests/risk/order_submission_scenarios_tests.cc
    tests/execution/order_submission_coordinator_tests.cc
    tests/execution/reconciliation_coordinator_tests.cc
  )
  target_link_libraries(botguard_tests PRIVATE botguard_agent botguard_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(botguard_tests)

  if(BOTGUARD_BUILD_SQLITE_STORE)
    add_test(NAME botguard_failure_demo COMMAND botguard_failure_demo)
    add_executable(botguard_uds_tests
      tests/agent/uds_killer_integration_tests.cc
      tests/demo/simulator_runtime_tests.cc
    )
    target_link_libraries(botguard_uds_tests PRIVATE botguard_demo_support botguard_agent GTest::gtest_main SQLite::SQLite3)
    gtest_discover_tests(botguard_uds_tests)
    add_executable(botguard_sqlite_tests
      tests/execution/runtime_lease_tests.cc
      tests/execution/sqlite_order_state_store_tests.cc
      tests/execution/sqlite_runtime_state_store_tests.cc
      tests/execution/sqlite_storage_pair_tests.cc
      tests/execution/sqlite_submission_recovery_tests.cc
    )
    target_link_libraries(botguard_sqlite_tests PRIVATE botguard_sqlite_store GTest::gtest_main SQLite::SQLite3)
    gtest_discover_tests(botguard_sqlite_tests)
  endif()

  if(BOTGUARD_BUILD_KALSHI_ADAPTER)
    add_executable(botguard_kalshi_tests tests/execution/kalshi_adapter_tests.cc)
    target_link_libraries(botguard_kalshi_tests PRIVATE botguard_kalshi_adapter GTest::gtest_main OpenSSL::Crypto)
    gtest_discover_tests(botguard_kalshi_tests)
  endif()
endif()

option(BOTGUARD_BUILD_BENCHMARKS "Build BotGuard microbenchmarks" OFF)
if(BOTGUARD_BUILD_BENCHMARKS)
  add_executable(botguard_risk_bench bench/risk_engine_bench.cc)
  target_link_libraries(botguard_risk_bench PRIVATE botguard_core)
endif()
