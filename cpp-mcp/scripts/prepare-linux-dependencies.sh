#!/usr/bin/env bash
set -euo pipefail
# Everything is installed below this task-owned directory; no distro packages are changed.
deps_root="${1:?Pass an absolute task-owned dependency directory}"
source_root="${2:?Pass the C++ source worktree}"
[[ "$deps_root" = /* && "$source_root" = /* ]] || exit 2
mkdir -p "$deps_root"
cmake_version=4.2.1
cmake_archive="cmake-${cmake_version}-linux-x86_64.tar.gz"
cmake_release="https://github.com/Kitware/CMake/releases/download/v${cmake_version}"
if [[ ! -x "$deps_root/cmake-${cmake_version}-linux-x86_64/bin/cmake" ]]; then
  curl --fail --location --retry 3 "$cmake_release/$cmake_archive" --output "$deps_root/$cmake_archive"
  curl --fail --location --retry 3 "$cmake_release/cmake-${cmake_version}-SHA-256.txt" --output "$deps_root/cmake-SHA-256.txt"
  (
    cd "$deps_root"
    rg "  ${cmake_archive}$" cmake-SHA-256.txt | sha256sum --check --strict
    tar -xzf "$cmake_archive"
  )
fi
vcpkg_revision=04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4
if [[ ! -d "$deps_root/vcpkg/.git" ]]; then
  git init "$deps_root/vcpkg"
  git -C "$deps_root/vcpkg" remote add origin https://github.com/microsoft/vcpkg.git
  git -C "$deps_root/vcpkg" fetch --depth 1 origin "$vcpkg_revision"
  git -C "$deps_root/vcpkg" checkout --detach FETCH_HEAD
fi
[[ "$(git -C "$deps_root/vcpkg" rev-parse HEAD)" = "$vcpkg_revision" ]] || { printf 'Unexpected vcpkg revision\n' >&2; exit 3; }
if [[ ! -x "$deps_root/vcpkg/vcpkg" ]]; then "$deps_root/vcpkg/bootstrap-vcpkg.sh" -disableMetrics; fi
export VCPKG_MAX_CONCURRENCY=4
"$deps_root/vcpkg/vcpkg" install --triplet x64-linux --x-manifest-root="$source_root" --x-install-root="$deps_root/installed"
"$deps_root/cmake-${cmake_version}-linux-x86_64/bin/cmake" -S "$source_root" -B "$deps_root/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DDEVBOX_BUILD_TUI=OFF -DDEVBOX_BUILD_TESTS=ON \
  -DCMAKE_TOOLCHAIN_FILE="$deps_root/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_INSTALLED_DIR="$deps_root/installed" -DVCPKG_TARGET_TRIPLET=x64-linux
