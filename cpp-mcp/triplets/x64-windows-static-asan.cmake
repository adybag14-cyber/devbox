set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
# Instrument dependencies too: mixing MSVC annotated/unannotated STL objects
# is an ODR violation and fails linking, not a reason to disable annotations.
set(VCPKG_C_FLAGS "/fsanitize=address /Zi")
set(VCPKG_CXX_FLAGS "/fsanitize=address /Zi")
