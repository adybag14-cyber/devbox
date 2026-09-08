#include "devbox/jobs.hpp"
#include <algorithm>
#include <sstream>
namespace devbox {
namespace {
bool one_of(std::string_view value, std::initializer_list<std::string_view> list) {
    return std::find(list.begin(), list.end(), value) != list.end();
}
std::optional<ResourceClass> explicit_class(std::string_view value) {
    const auto name = lower(trim(value));
    return one_of(name, {"watch", "light", "heavy", "io-heavy", "io_heavy", "ioheavy"})
               ? std::optional(resource_class(name))
               : std::nullopt;
}
std::string program_name(std::string_view value) {
    auto name = lower(trim(value));
    const auto separator = name.find_last_of("/\\");
    if (separator != std::string::npos)
        name.erase(0, separator + 1);
    for (const auto suffix : {".exe", ".cmd", ".bat", ".ps1"})
        if (name.ends_with(suffix)) {
            name.resize(name.size() - std::string_view(suffix).size());
            break;
        }
    return name;
}
std::vector<std::string> words(std::string_view text) {
    std::istringstream stream{std::string(text)};
    std::vector<std::string> result;
    std::string value;
    while (stream >> value)
        result.push_back(value);
    return result;
}
std::string unquote(std::string value) {
    while (!value.empty() && (value.front() == '\'' || value.front() == '"'))
        value.erase(value.begin());
    while (!value.empty() && (value.back() == '\'' || value.back() == '"'))
        value.pop_back();
    return value;
}
} // namespace
ResourceClass infer_shell_resource(std::string_view command, std::string_view requested) {
    if (const auto explicit_value = explicit_class(requested))
        return *explicit_value;
    const auto text = lower(std::string(command));
    const auto tokens = words(text);
    for (const auto marker : {"playwright",    "selenium",       "gradle",
                              "gradlew",       "mvn ",           "mvnw",
                              "ninja",         "cmake --build",  "cargo build",
                              "cargo test",    "cargo bench",    "cargo clippy",
                              "rustc ",        "zig build",      "go build",
                              "go test",       "docker build",   "docker buildx build",
                              "pytest",        "npm build",      "npm test",
                              "npm run build", "npm run test",   "pnpm build",
                              "pnpm test",     "pnpm run build", "pnpm run test",
                              "yarn build",    "yarn test",      "bun build",
                              "bun test",      "pip wheel",      "python -m build",
                              "bazel",         "msbuild",        "dotnet build",
                              "dotnet test",   "dotnet publish"})
        if (text.find(marker) != std::string::npos)
            return ResourceClass::heavy;
    if (!tokens.empty() && one_of(tokens.front(), {"make", "gmake", "mingw32-make"}))
        return ResourceClass::heavy;
    const auto raw_tokens = words(command);
    for (std::size_t i = 0; i + 1 < raw_tokens.size(); ++i)
        if (program_name(unquote(raw_tokens[i])) == "rg" &&
            !one_of(unquote(raw_tokens[i + 1]), {"--version", "-V"}))
            return ResourceClass::io_heavy;
    const auto padded = " " + text + " ";
    for (const auto marker : {" find ",
                              " find.exe ",
                              " grep -r",
                              " grep --recursive",
                              " get-childitem ",
                              " -recurse",
                              " du ",
                              " robocopy ",
                              " xcopy ",
                              " rsync ",
                              " cp -r",
                              " cp -a",
                              " rm -r",
                              " remove-item ",
                              " tar ",
                              " 7z ",
                              " 7zz ",
                              " zip -r",
                              " git clone ",
                              " git fetch ",
                              " git gc",
                              " git repack",
                              " npm ci",
                              " npm install",
                              " npm add",
                              " pnpm ci",
                              " pnpm install",
                              " pnpm add",
                              " yarn ci",
                              " yarn install",
                              " yarn add",
                              " bun ci",
                              " bun install",
                              " bun add",
                              " pip install",
                              " pip3 install",
                              " -m pip install",
                              " apt install",
                              " apt add",
                              " apt upgrade",
                              " apt update",
                              " apt-get install",
                              " apt-get add",
                              " apt-get upgrade",
                              " apt-get update",
                              " dnf install",
                              " dnf add",
                              " dnf upgrade",
                              " dnf update",
                              " yum install",
                              " yum add",
                              " yum upgrade",
                              " yum update",
                              " pacman -s",
                              " pacman install",
                              " pacman add",
                              " pacman upgrade",
                              " pacman update",
                              " apk install",
                              " apk add",
                              " apk upgrade",
                              " apk update",
                              " winget install",
                              " winget add",
                              " winget upgrade",
                              " winget update",
                              " choco install",
                              " choco add",
                              " choco upgrade",
                              " choco update",
                              " scoop install",
                              " scoop add",
                              " scoop upgrade",
                              " scoop update"})
        if (padded.find(marker) != std::string::npos)
            return ResourceClass::io_heavy;
    if (text.find("gh run watch") != std::string::npos || text.find("start-sleep") != std::string::npos)
        return ResourceClass::watch;
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i)
        if (tokens[i] == "sleep" && std::all_of(tokens[i + 1].begin(), tokens[i + 1].end(),
                                                [](char c) { return c >= '0' && c <= '9'; }))
            return ResourceClass::watch;
    return ResourceClass::light;
}
ResourceClass infer_program_resource(std::string_view program, const std::vector<std::string>& arguments,
                                     std::string_view requested) {
    if (const auto explicit_value = explicit_class(requested))
        return *explicit_value;
    const auto name = program_name(program);
    std::vector<std::string> raw, args;
    for (const auto& value : arguments) {
        raw.push_back(trim(value));
        args.push_back(lower(trim(value)));
    }
    const auto first = args.empty() ? "" : args[0], second = args.size() < 2 ? "" : args[1],
               third = args.size() < 3 ? "" : args[2];
    const auto raw_first = raw.empty() ? "" : raw[0];
    if ((name == "gh" && first == "run" && second == "watch") || one_of(name, {"sleep", "start-sleep"}))
        return ResourceClass::watch;
    if (name == "wsl")
        return infer_shell_resource(join(raw, " "), requested);
    if (one_of(name, {"find", "du", "robocopy", "xcopy", "rsync", "tar", "7z", "7zz", "zip", "unzip"}) ||
        (name == "rg" && !args.empty() && first != "--version" && raw_first != "-V") ||
        (name == "git" && one_of(first, {"clone", "fetch", "gc", "repack"})) ||
        (one_of(name, {"npm", "pnpm", "yarn", "bun"}) && one_of(first, {"ci", "install", "add"})) ||
        (one_of(name, {"apt", "apt-get", "dnf", "yum", "pacman", "apk", "winget", "choco", "scoop"}) &&
         one_of(first, {"install", "add", "upgrade", "update", "-s"})) ||
        (one_of(name, {"pip", "pip3"}) && first == "install") ||
        (one_of(name, {"python", "python3", "py"}) && first == "-m" && second == "pip" && third == "install"))
        return ResourceClass::io_heavy;
    if (one_of(name, {"pwsh", "powershell"}))
        for (std::size_t i = 0; i + 1 < args.size(); ++i)
            if (one_of(args[i], {"-command", "-c"}))
                return infer_shell_resource(args[i + 1], requested);
    bool heavy = one_of(name, {"rustc", "ninja", "make", "gmake", "mingw32-make", "bazel", "msbuild",
                               "gradle", "gradlew", "mvn", "mvnw", "pytest", "py.test"});
    if (name == "cargo")
        heavy = one_of(first, {"build", "test", "bench", "clippy"});
    if (name == "cmake")
        heavy = std::find(args.begin(), args.end(), "--build") != args.end();
    if (name == "zig")
        heavy = first == "build";
    if (name == "go")
        heavy = one_of(first, {"build", "test", "install"});
    if (name == "docker")
        heavy = first == "build" || (first == "buildx" && second == "build") ||
                (first == "compose" && third == "build");
    if (name == "dotnet")
        heavy = one_of(first, {"build", "test", "publish", "pack"});
    if (one_of(name, {"npm", "pnpm", "yarn", "bun"}))
        heavy = one_of(first, {"build", "test"}) || (first == "run" && one_of(second, {"build", "test"}));
    if (name == "npx")
        heavy = one_of(first, {"playwright", "selenium", "webpack", "vite", "tsc"});
    if (one_of(name, {"pip", "pip3"}))
        heavy = first == "wheel";
    if (one_of(name, {"python", "python3", "py"}))
        heavy = first == "-m" && one_of(second, {"pytest", "build", "pip"}) &&
                (second != "pip" || third == "wheel");
    return heavy ? ResourceClass::heavy : ResourceClass::light;
}
} // namespace devbox
