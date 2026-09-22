find_package(Boost CONFIG REQUIRED COMPONENTS asio beast)
find_package(ada CONFIG REQUIRED)
find_package(re2 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(lexbor CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_path(DEVBOX_LEXBOR_INCLUDE_DIR lexbor/html/interfaces/document.h
  HINTS "${lexbor_DIR}/../../include" REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(libdeflate CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
set(DEVBOX_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${DEVBOX_GENERATED_DIR}")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/contract/tool-registry.json" registry_json)
string(JSON registry_count LENGTH "${registry_json}" tools)
string(JSON registry_version GET "${registry_json}" contract_version)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/contract/tool-registry.json" registry_hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," registry_bytes "${registry_hex}")
file(WRITE "${DEVBOX_GENERATED_DIR}/tool_registry.hpp.tmp" "inline constexpr unsigned char embedded_tool_registry[] = {${registry_bytes}};\n")
file(COPY_FILE "${DEVBOX_GENERATED_DIR}/tool_registry.hpp.tmp" "${DEVBOX_GENERATED_DIR}/tool_registry.hpp" ONLY_IF_DIFFERENT)
file(MAKE_DIRECTORY "${DEVBOX_GENERATED_DIR}/include/devbox")
file(WRITE "${DEVBOX_GENERATED_DIR}/registry_counts.hpp.tmp" "#pragma once\n#include <cstddef>\nnamespace devbox { inline constexpr std::size_t cpp_tool_count = ${registry_count}; inline constexpr unsigned cpp_contract_version = ${registry_version}; }\n")
file(COPY_FILE "${DEVBOX_GENERATED_DIR}/registry_counts.hpp.tmp" "${DEVBOX_GENERATED_DIR}/include/devbox/registry_counts.hpp" ONLY_IF_DIFFERENT)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/contract/tool-registry.json")
add_custom_target(devbox-build-identity
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE_ROOT=${PROJECT_SOURCE_DIR}" "-DOUTPUT_DIR=${DEVBOX_GENERATED_DIR}"
    "-DCOMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}" "-DVERSION=${PROJECT_VERSION}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_identity.cmake"
  BYPRODUCTS "${DEVBOX_GENERATED_DIR}/build_identity.hpp"
  VERBATIM)
add_library(devbox_core
  src/common.cpp
  src/json_output.cpp
  src/resource_budget.cpp
  src/state_file.cpp
  src/state_store.cpp
  src/state_coordinator.cpp
  src/config.cpp
  src/result.cpp
  src/process.cpp
  src/posix_process.cpp
  src/storage.cpp
  src/filesystem_worker.cpp
  src/artifacts.cpp
  src/security.cpp
  src/provider.cpp
  src/provider_protocol.cpp
  src/grants.cpp
  src/runs.cpp
  src/run_service.cpp
  src/tasks.cpp
  src/engine_extensions.cpp
  src/isolation.cpp
  src/isolation_linux.cpp
  src/runtime.cpp
  src/scheduler.cpp
  src/job_logs.cpp
  src/jobs.cpp
  src/indexed_jobs.cpp
  src/detached.cpp
  src/resource_class.cpp
  src/job_manager.cpp
  src/job_runner.cpp
  src/jobs_maintenance.cpp
  src/oauth.cpp
  src/async.cpp
  src/gateway.cpp
  src/server.cpp
  src/background.cpp
  src/lifecycle.cpp
  src/github_auth.cpp
  src/docker_files.cpp
  src/search.cpp
  src/web_transport.cpp
  src/web_document.cpp
  src/web_offers.cpp
  src/web_discovery.cpp
  src/web_policy.cpp
  src/web_providers.cpp
  src/research.cpp
  src/host_inspect.cpp
  src/usage.cpp
  src/allocator.cpp
  src/performance.cpp
  src/provenance.cpp
  src/contract.cpp
  src/monitoring.cpp
  src/engine.cpp
  src/engine_execution.cpp
  src/engine_files.cpp
  src/engine_waits.cpp
  src/capture.cpp
  src/capture_windows.cpp
  src/capture_posix.cpp
  src/computer_use.cpp
  src/computer_broker.cpp
  src/engine_capture.cpp
)
add_dependencies(devbox_core devbox-build-identity)
target_include_directories(devbox_core PRIVATE "${DEVBOX_GENERATED_DIR}")
target_include_directories(devbox_core PRIVATE "${DEVBOX_LEXBOR_INCLUDE_DIR}")
target_include_directories(devbox_core PUBLIC include "${DEVBOX_GENERATED_DIR}/include")
target_link_libraries(devbox_core PUBLIC nlohmann_json::nlohmann_json Boost::asio Boost::beast OpenSSL::Crypto CURL::libcurl ZLIB::ZLIB Threads::Threads ada::ada re2::re2)
target_link_libraries(devbox_core PUBLIC $<IF:$<TARGET_EXISTS:libdeflate::libdeflate_shared>,libdeflate::libdeflate_shared,libdeflate::libdeflate_static>)
target_link_libraries(devbox_core PUBLIC $<IF:$<TARGET_EXISTS:lexbor::lexbor_static>,lexbor::lexbor_static,lexbor::lexbor>)
target_link_libraries(devbox_core PUBLIC pugixml::pugixml)
target_link_libraries(devbox_core PUBLIC unofficial::sqlite3::sqlite3)
install(FILES "${unofficial-sqlite3_DIR}/../sqlite3/copyright" DESTINATION share/devbox RENAME sqlite3.LICENSE.txt)
target_compile_definitions(devbox_core PUBLIC BOOST_ASIO_NO_DEPRECATED)
if(DEVBOX_FORCE_FORK_EXEC)
  target_compile_definitions(devbox_core PRIVATE DEVBOX_FORCE_FORK_EXEC=1)
endif()
add_executable(devbox-mcp src/main.cpp)
target_link_libraries(devbox-mcp PRIVATE devbox_core)
if(DEVBOX_BUILD_BENCHMARKS)
  add_executable(devbox-component-bench benchmarks/components.cpp)
  target_link_libraries(devbox-component-bench PRIVATE devbox_core)
endif()
if(MSVC)
  target_compile_options(devbox-mcp PRIVATE /utf-8)
endif()
if(WIN32)
  target_link_libraries(devbox-mcp PRIVATE synchronization)
  target_compile_definitions(devbox_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_link_libraries(devbox_core PUBLIC ws2_32 bcrypt crypt32 advapi32 psapi userenv shell32 ole32 windowscodecs gdi32 dwmapi shcore OneCoreUAP)
endif()
if(APPLE)
  target_link_libraries(devbox_core PUBLIC "-framework ApplicationServices")
endif()
if(UNIX AND NOT APPLE)
  target_compile_definitions(devbox_core PRIVATE _GNU_SOURCE)
endif()
if(DEVBOX_BUILD_TESTS)
  add_executable(devbox-indexed-maintenance-tests tests/indexed_maintenance_tests.cpp)
  target_link_libraries(devbox-indexed-maintenance-tests PRIVATE devbox_core)
  add_test(NAME indexed-maintenance COMMAND devbox-indexed-maintenance-tests)
  set_tests_properties(indexed-maintenance PROPERTIES TIMEOUT 60)
  add_executable(devbox-filesystem-worker-tests tests/filesystem_worker_tests.cpp)
  target_link_libraries(devbox-filesystem-worker-tests PRIVATE devbox_core)
  add_test(NAME filesystem-worker COMMAND devbox-filesystem-worker-tests)
  set_tests_properties(filesystem-worker PROPERTIES TIMEOUT 20)
  add_executable(devbox-runs-tests tests/runs_tests.cpp)
  target_link_libraries(devbox-runs-tests PRIVATE devbox_core)
  add_test(NAME runs COMMAND devbox-runs-tests)
  set_tests_properties(runs PROPERTIES TIMEOUT 60)
  add_executable(devbox-provider-tests tests/provider_tests.cpp)
  target_link_libraries(devbox-provider-tests PRIVATE devbox_core)
  add_test(NAME providers COMMAND devbox-provider-tests)
  set_tests_properties(providers PROPERTIES TIMEOUT 30)
  add_executable(devbox-grants-tests tests/grants_tests.cpp)
  target_link_libraries(devbox-grants-tests PRIVATE devbox_core)
  add_test(NAME grants COMMAND devbox-grants-tests)
  set_tests_properties(grants PROPERTIES TIMEOUT 30)
  add_executable(devbox-isolation-probe tests/isolation_probe.cpp)
  if(DEVBOX_SANITIZERS AND NOT MSVC)
    # The broker stays instrumented. The tiny OS-policy probe must fit the worker's
    # address-space limit instead of reserving an ASan shadow mapping of the host.
    target_compile_options(devbox-isolation-probe PRIVATE -fno-sanitize=all)
    target_link_options(devbox-isolation-probe PRIVATE -fno-sanitize=all)
  endif()
  if(WIN32)
    target_link_libraries(devbox-isolation-probe PRIVATE advapi32 ws2_32)
    target_compile_definitions(devbox-isolation-probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  endif()
  add_executable(devbox-isolation-tests tests/isolation_tests.cpp)
  target_link_libraries(devbox-isolation-tests PRIVATE devbox_core)
  add_dependencies(devbox-isolation-tests devbox-isolation-probe)
  add_test(NAME isolation COMMAND devbox-isolation-tests)
  set_tests_properties(isolation PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
  add_executable(devbox-artifacts-tests tests/artifacts_tests.cpp)
  target_link_libraries(devbox-artifacts-tests PRIVATE devbox_core)
  add_test(NAME artifacts COMMAND devbox-artifacts-tests)
  set_tests_properties(artifacts PROPERTIES TIMEOUT 120)
  add_executable(devbox-indexed-jobs-tests tests/indexed_jobs_tests.cpp)
  target_link_libraries(devbox-indexed-jobs-tests PRIVATE devbox_core)
  add_test(NAME indexed-jobs COMMAND devbox-indexed-jobs-tests)
  set_tests_properties(indexed-jobs PROPERTIES TIMEOUT 60)
  add_executable(devbox-state-coordinator-tests tests/state_coordinator_tests.cpp)
  target_link_libraries(devbox-state-coordinator-tests PRIVATE devbox_core)
  if(MSVC)
    target_compile_options(devbox-state-coordinator-tests PRIVATE /utf-8)
  endif()
  add_test(NAME state-coordinator COMMAND devbox-state-coordinator-tests)
  set_tests_properties(state-coordinator PROPERTIES TIMEOUT 60)
  add_executable(devbox-state-store-tests tests/state_store_tests.cpp)
  target_link_libraries(devbox-state-store-tests PRIVATE devbox_core)
  if(MSVC)
    target_compile_options(devbox-state-store-tests PRIVATE /utf-8)
  endif()
  add_test(NAME state-store COMMAND devbox-state-store-tests)
  set_tests_properties(state-store PROPERTIES TIMEOUT 180)
  if(WIN32)
    # Hosted FULL/WAL seeding was observed progressing through 80k rows at 167s.
    # Keep all 100k records and query-step limits; allow disk-dependent fixture setup to finish.
    set_tests_properties(state-store PROPERTIES TIMEOUT 360)
  endif()
  add_executable(devbox-resource-budget-tests tests/resource_budget_tests.cpp)
  target_link_libraries(devbox-resource-budget-tests PRIVATE devbox_core)
  if(MSVC)
    target_compile_options(devbox-resource-budget-tests PRIVATE /utf-8)
  endif()
  add_test(NAME resource-budget COMMAND devbox-resource-budget-tests)
  set_tests_properties(resource-budget PROPERTIES TIMEOUT 20)
  add_executable(devbox-security-tests tests/security_tests.cpp)
  target_link_libraries(devbox-security-tests PRIVATE devbox_core)
  add_test(NAME security COMMAND devbox-security-tests)
  set_tests_properties(security PROPERTIES TIMEOUT 30)
  add_executable(devbox-web-offer-tests tests/web_offer_tests.cpp)
  target_link_libraries(devbox-web-offer-tests PRIVATE devbox_core)
  if(MSVC)
    target_compile_options(devbox-web-offer-tests PRIVATE /utf-8)
  endif()
  add_test(NAME web-offers COMMAND devbox-web-offer-tests)
  set_tests_properties(web-offers PROPERTIES TIMEOUT 15)
  add_executable(devbox-web-research-tests tests/web_research_tests.cpp)
  target_link_libraries(devbox-web-research-tests PRIVATE devbox_core)
  if(MSVC)
    target_compile_options(devbox-web-research-tests PRIVATE /utf-8)
  endif()
  add_test(NAME web-research COMMAND devbox-web-research-tests)
  set_tests_properties(web-research PROPERTIES TIMEOUT 75)
  add_executable(devbox-computer-tests tests/computer_use_tests.cpp)
  target_link_libraries(devbox-computer-tests PRIVATE devbox_core)
  add_test(NAME computer-contract COMMAND devbox-computer-tests)
  set_tests_properties(computer-contract PROPERTIES TIMEOUT 30)
  if(WIN32)
    add_test(NAME computer-native COMMAND devbox-computer-tests native)
    add_test(NAME computer-broker-native COMMAND devbox-computer-tests broker-native)
    set_tests_properties(computer-native computer-broker-native PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
  endif()
  add_executable(devbox-async-tests tests/async_tests.cpp)
  target_link_libraries(devbox-async-tests PRIVATE devbox_core)
  add_test(NAME async COMMAND devbox-async-tests)
  set_tests_properties(async PROPERTIES TIMEOUT 30)
  add_executable(devbox-capture-tests tests/capture_tests.cpp)
  target_link_libraries(devbox-capture-tests PRIVATE devbox_core)
  add_test(NAME capture COMMAND devbox-capture-tests)
  set_tests_properties(capture PROPERTIES TIMEOUT 45)
  add_executable(devbox-engine-tests tests/engine_tests.cpp)
  target_link_libraries(devbox-engine-tests PRIVATE devbox_core)
  target_compile_definitions(devbox-engine-tests PRIVATE DEVBOX_CONTRACT_FILE="${CMAKE_CURRENT_SOURCE_DIR}/contract/reference-tools.json")
  if(MSVC)
    target_compile_options(devbox-engine-tests PRIVATE /utf-8)
  endif()
  add_test(NAME engine COMMAND devbox-engine-tests)
  set_tests_properties(engine PROPERTIES TIMEOUT 60)
  add_executable(devbox-telemetry-tests tests/telemetry_tests.cpp)
  target_link_libraries(devbox-telemetry-tests PRIVATE devbox_core)
  if(MSVC)
    target_compile_options(devbox-telemetry-tests PRIVATE /utf-8)
  endif()
  add_test(NAME telemetry COMMAND devbox-telemetry-tests)
  set_tests_properties(telemetry PROPERTIES TIMEOUT 30)
  add_executable(devbox-core-tests tests/core_tests.cpp)
  target_link_libraries(devbox-core-tests PRIVATE devbox_core)
  if(MSVC)
    target_compile_options(devbox-core-tests PRIVATE /utf-8)
  endif()
  add_test(NAME core COMMAND devbox-core-tests)
  add_executable(devbox-process-tests tests/process_tests.cpp)
  target_link_libraries(devbox-process-tests PRIVATE devbox_core)
  if(MSVC)
    target_compile_options(devbox-process-tests PRIVATE /utf-8)
  endif()
  add_test(NAME process COMMAND devbox-process-tests)
  set_tests_properties(process PROPERTIES TIMEOUT 60)
  add_executable(devbox-storage-tests tests/storage_tests.cpp)
  target_link_libraries(devbox-storage-tests PRIVATE devbox_core)
  if(MSVC)
    target_compile_options(devbox-storage-tests PRIVATE /utf-8)
  endif()
  add_test(NAME storage COMMAND devbox-storage-tests)
  set_tests_properties(storage PROPERTIES TIMEOUT 60)
  add_executable(devbox-runtime-tests tests/runtime_tests.cpp)
  target_link_libraries(devbox-runtime-tests PRIVATE devbox_core)
  if(MSVC)
    target_compile_options(devbox-runtime-tests PRIVATE /utf-8)
  endif()
  add_test(NAME runtime COMMAND devbox-runtime-tests)
  set_tests_properties(runtime PROPERTIES TIMEOUT 60)
  add_executable(devbox-scheduler-tests tests/scheduler_tests.cpp)
  target_link_libraries(devbox-scheduler-tests PRIVATE devbox_core)
  if(MSVC)
    target_compile_options(devbox-scheduler-tests PRIVATE /utf-8)
  endif()
  add_test(NAME scheduler COMMAND devbox-scheduler-tests)
  set_tests_properties(scheduler PROPERTIES TIMEOUT 60)
  add_executable(devbox-jobs-tests tests/jobs_tests.cpp)
  target_link_libraries(devbox-jobs-tests PRIVATE devbox_core)
  if(MSVC)
    target_compile_options(devbox-jobs-tests PRIVATE /utf-8)
  endif()
  add_test(NAME jobs COMMAND devbox-jobs-tests)
  set_tests_properties(jobs PROPERTIES TIMEOUT 60)
  add_executable(devbox-oauth-tests tests/oauth_tests.cpp)
  target_link_libraries(devbox-oauth-tests PRIVATE devbox_core)
  target_compile_definitions(devbox-oauth-tests PRIVATE DEVBOX_CONTRACT_FILE="${CMAKE_CURRENT_SOURCE_DIR}/contract/reference-tools.json")
  if(MSVC)
    target_compile_options(devbox-oauth-tests PRIVATE /utf-8)
  endif()
  add_test(NAME oauth COMMAND devbox-oauth-tests)
  set_tests_properties(oauth PROPERTIES TIMEOUT 45)
  add_executable(devbox-transport-tests tests/transport_tests.cpp)
  target_link_libraries(devbox-transport-tests PRIVATE devbox_core)
  if(MSVC)
    target_compile_options(devbox-transport-tests PRIVATE /utf-8)
  endif()
  add_test(NAME transport COMMAND devbox-transport-tests)
  set_tests_properties(transport PROPERTIES TIMEOUT 60)
  if(WIN32)
    add_executable(devbox-shutdown-tests tests/shutdown_tests.cpp src/main.cpp)
    target_compile_definitions(devbox-shutdown-tests PRIVATE DEVBOX_NO_MAIN=1)
    target_link_libraries(devbox-shutdown-tests PRIVATE devbox_core synchronization)
    if(MSVC)
      target_compile_options(devbox-shutdown-tests PRIVATE /utf-8)
    endif()
    add_test(NAME shutdown-terminate COMMAND devbox-shutdown-tests terminate)
    add_test(NAME shutdown-interrupt COMMAND devbox-shutdown-tests interrupt)
    add_test(NAME shutdown-stuck-filesystem COMMAND devbox-shutdown-tests stuck)
    set_tests_properties(shutdown-terminate shutdown-interrupt shutdown-stuck-filesystem PROPERTIES TIMEOUT 20)
  endif()
  add_executable(devbox-lifecycle-tests tests/lifecycle_tests.cpp)
  target_link_libraries(devbox-lifecycle-tests PRIVATE devbox_core)
  if(MSVC)
    target_compile_options(devbox-lifecycle-tests PRIVATE /utf-8)
  endif()
  add_test(NAME lifecycle COMMAND devbox-lifecycle-tests)
  set_tests_properties(lifecycle PROPERTIES TIMEOUT 45)
  add_executable(devbox-search-tests tests/search_tests.cpp)
  target_link_libraries(devbox-search-tests PRIVATE devbox_core)
  if(MSVC)
    target_compile_options(devbox-search-tests PRIVATE /utf-8)
  endif()
  add_test(NAME search COMMAND devbox-search-tests)
  set_tests_properties(search PROPERTIES TIMEOUT 45)
endif()
if(MSVC)
  target_compile_options(devbox_core PRIVATE /W4 /permissive- /utf-8 /EHsc /bigobj)
else()
  target_compile_options(devbox_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
