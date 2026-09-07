#!/usr/bin/env bash
set -euo pipefail
deps_root="${1:?Pass the absolute task-owned dependency directory}"
[[ "$deps_root" = /* ]] || exit 2
compiler_root="$deps_root/gcc12"
mkdir -p "$compiler_root/archives" "$compiler_root/bin"
version=12.3.0-1ubuntu1~22.04.3
if [[ ! -x "$compiler_root/usr/bin/x86_64-linux-gnu-g++-12" ]]; then
  (
    cd "$compiler_root/archives"
    apt-get download "g++-12=$version" "libstdc++-12-dev=$version"
    sha256sum ./*.deb > downloaded-SHA256.txt
    for archive in ./*.deb; do dpkg-deb --extract "$archive" "$compiler_root"; done
  )
fi
cat > "$compiler_root/bin/g++" <<EOF
#!/usr/bin/env bash
exec "$compiler_root/usr/bin/x86_64-linux-gnu-g++-12" \
  -B"$compiler_root/usr/lib/gcc/x86_64-linux-gnu/12/" \
  -B/usr/lib/gcc/x86_64-linux-gnu/12/ \
  -isystem "$compiler_root/usr/include/c++/12" \
  -isystem "$compiler_root/usr/include/x86_64-linux-gnu/c++/12" \
  -isystem "$compiler_root/usr/include/c++/12/backward" "\$@"
EOF
chmod +x "$compiler_root/bin/g++"
printf '#include <string>\n#include <string_view>\nconstexpr bool test(){std::string s="C++20";return std::string_view(s)=="C++20";}\nstatic_assert(test());\nint main(){}\n' > "$compiler_root/probe.cpp"
"$compiler_root/bin/g++" -std=c++20 "$compiler_root/probe.cpp" -o "$compiler_root/probe"
"$compiler_root/probe"
"$compiler_root/bin/g++" --version
