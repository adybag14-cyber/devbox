cmake_minimum_required(VERSION 3.16)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../cpp-bootstrap/VERSION" DEVBOX_SETUP_VERSION LIMIT_COUNT 1)
project(devbox_tui VERSION "${DEVBOX_SETUP_VERSION}" LANGUAGES CXX)

add_executable(devbox-tui src/main.cpp)
target_compile_definitions(devbox-tui PRIVATE DEVBOX_TUI_VERSION="${DEVBOX_SETUP_VERSION}")
target_compile_features(devbox-tui PRIVATE cxx_std_17)

if(MSVC)
  target_compile_options(devbox-tui PRIVATE /W4 /permissive- /EHsc)
  target_compile_definitions(devbox-tui PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  target_compile_options(devbox-tui PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS devbox-tui RUNTIME DESTINATION bin)
