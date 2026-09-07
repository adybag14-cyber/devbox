find_package(Boost CONFIG REQUIRED COMPONENTS asio beast)
find_package(ada CONFIG REQUIRED)
find_package(re2 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
set(DEVBOX_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${DEVBOX_GENERATED_DIR}")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/contract/reference-tools.json" reference_hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," reference_bytes "${reference_hex}")
file(WRITE "${DEVBOX_GENERATED_DIR}/reference_contract.hpp.tmp" "inline constexpr unsigned char embedded_contract[] = {${reference_bytes}};\n")
file(COPY_FILE "${DEVBOX_GENERATED_DIR}/reference_contract.hpp.tmp" "${DEVBOX_GENERATED_DIR}/reference_contract.hpp" ONLY_IF_DIFFERENT)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/contract/reference-tools.json")
add_custom_target(devbox-build-identity
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE_ROOT=${PROJECT_SOURCE_DIR}" "-DOUTPUT_DIR=${DEVBOX_GENERATED_DIR}"
    "-DCOMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}" "-DVERSION=${PROJECT_VERSION}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_identity.cmake"
  BYPRODUCTS "${DEVBOX_GENERATED_DIR}/build_identity.hpp"
  VERBATIM)
add_library(devbox_core
  src/common.cpp
  src/state_file.cpp
  src/config.cpp
  src/result.cpp
  src/process.cpp
  src/storage.cpp
  src/runtime.cpp
  src/scheduler.cpp
  src/job_logs.cpp
  src/jobs.cpp
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
)
add_dependencies(devbox_core devbox-build-identity)
target_include_directories(devbox_core PRIVATE "${DEVBOX_GENERATED_DIR}")
target_include_directories(devbox_core PUBLIC include)
target_link_libraries(devbox_core PUBLIC nlohmann_json::nlohmann_json Boost::asio Boost::beast OpenSSL::Crypto CURL::libcurl Threads::Threads ada::ada re2::re2)
target_compile_definitions(devbox_core PUBLIC BOOST_ASIO_NO_DEPRECATED)
add_executable(devbox-mcp src/main.cpp)
target_link_libraries(devbox-mcp PRIVATE devbox_core)
if(MSVC)
  target_compile_options(devbox-mcp PRIVATE /utf-8)
endif()
if(WIN32)
  target_compile_definitions(devbox_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_link_libraries(devbox_core PUBLIC ws2_32 bcrypt crypt32 advapi32 psapi userenv shell32 ole32 windowscodecs gdi32 dwmapi shcore)
endif()
if(UNIX AND NOT APPLE)
  target_compile_definitions(devbox_core PRIVATE _GNU_SOURCE)
endif()
if(DEVBOX_BUILD_TESTS)
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
