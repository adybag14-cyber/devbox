#!/usr/bin/env bash
set -euo pipefail
deps_root="${1:?Pass the task-owned dependency directory}"
source_root="${2:?Pass the C++ source worktree}"
mode="${3:-release}"
shift 3
[[ "$deps_root" = /* && "$source_root" = /* ]] || exit 2
export PATH="$deps_root/cmake-4.2.1-linux-x86_64/bin:/usr/bin:/bin"
export CC=/usr/bin/gcc-12
export CXX="$deps_root/gcc12/bin/g++"
export VCPKG_MAX_CONCURRENCY=4
build_dir="$deps_root/build"
options=(-DCMAKE_BUILD_TYPE=Release -DDEVBOX_SANITIZERS=OFF -DDEVBOX_FORCE_FORK_EXEC=OFF)
if [[ "$mode" = asan ]]; then
  build_dir="$deps_root/build-asan"
  options=(-DCMAKE_BUILD_TYPE=RelWithDebInfo '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O1 -g -DNDEBUG' -DDEVBOX_SANITIZERS=ON -DDEVBOX_FORCE_FORK_EXEC=OFF)
elif [[ "$mode" = fork ]]; then
  build_dir="$deps_root/build-fork"
  options=(-DCMAKE_BUILD_TYPE=Release -DDEVBOX_SANITIZERS=OFF -DDEVBOX_FORCE_FORK_EXEC=ON)
elif [[ "$mode" != release ]]; then
  echo "Unknown build mode: $mode" >&2
  exit 2
fi
cmake -S "$source_root" -B "$build_dir" -G Ninja \
  "${options[@]}" -DDEVBOX_BUILD_TUI=OFF -DDEVBOX_BUILD_TESTS=ON \
  -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
  "-DCMAKE_TOOLCHAIN_FILE=$deps_root/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  "-DVCPKG_INSTALLED_DIR=$deps_root/installed" \
  -DVCPKG_TARGET_TRIPLET=x64-linux -DVCPKG_MANIFEST_INSTALL=OFF
targets=()
if (( $# )); then targets=(--target "$@"); fi
cmake --build "$build_dir" --parallel 4 "${targets[@]}"
