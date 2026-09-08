# Native C++ installer

`devbox-setup` configures Devbox, stages a verified C++ MCP executable, installs
Node/npm/Git prerequisites when needed, and starts the existing launcher and
Guardian integrations. The installer and TUI share the version in `VERSION`.

Release bundles contain `devbox-setup`, `devbox-tui`, and `devbox-mcp`. Keep the
three files together, or pass `--runtime-binary /absolute/path/to/devbox-mcp`.
The installer also finds the server with the same platform suffix as its own
filename. A new clone is checked out at the installer's source commit, so a
later default-branch change cannot mix incompatible binaries and launcher code.
An existing checkout is never switched to another revision by the installer.

```sh
devbox-setup --guardian
devbox-setup --repo /path/to/existing/devbox --runtime host --auth none
devbox-setup --repo /path/to/devbox --build-runtime-only
devbox-setup --help
```

Use `--no-start` to finish installation and stage the runtime without launching
it. `--skip-install`, `--no-link`, and `--skip-system-packages` control their
respective setup steps. `--dry-run` describes the proposed changes. Existing
configuration keys and comments are preserved except for explicitly selected
settings and the C++ implementation selection.

## Source builds

Use CMake 3.24+, a C++20 compiler, and the vcpkg revision in the repository's
`vcpkg.json`. The root CMake project builds all three executables and their native
tests. See the root README for the POSIX commands.

On Windows, use the Visual Studio C++ build tools and the static-runtime triplet:

```powershell
.\.cpp-build\vcpkg\bootstrap-vcpkg.bat -disableMetrics
cmake -S . -B .cpp-build/native -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DCMAKE_TOOLCHAIN_FILE="$PWD/.cpp-build/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build .cpp-build/native --config Release --parallel 4
ctest --test-dir .cpp-build/native -C Release --output-on-failure
cmake --install .cpp-build/native --config Release --prefix .cpp-build/package
```

Alpine source builds need GCC, Linux headers, CMake, Ninja, make, Perl, and the
usual autotools/archive utilities. Bootstrap vcpkg with `-musl -disableMetrics`
and set `VCPKG_FORCE_SYSTEM_BINARIES=1`. The release installer selects the native
musl bundle automatically.

Android release binaries use NDK `29.0.14206865`, API 21, and static libc++.
Use `cpp-mcp/scripts/build-android.mjs --abi <ABI>` with `ANDROID_NDK_HOME` and
`VCPKG_ROOT` set. Termux installation uses the packaged Android binaries.

## Validation

Native CTest covers configuration, validation before writes, dry runs, preserved
settings, and exact binary staging. `scripts/fresh-install.mjs` executes the real
installer and TUI with private Git checkouts and release-like assets; it also
checks source pinning when the default branch has advanced. The download fixture
verifies all three assets and proves that a corrupt download preserves the
previous installed binaries. Managed lifecycle checks run separately in isolated
checkouts and are required by the C++ certification workflow.
