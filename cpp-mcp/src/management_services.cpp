#include "devbox/management.hpp"
#include "devbox/native.hpp"
#ifdef _WIN32
#include <sddl.h>
#endif
namespace devbox {
namespace {
std::string xml(std::string value) {
    value = replace_all(std::move(value), "&", "&amp;");
    value = replace_all(std::move(value), "<", "&lt;");
    value = replace_all(std::move(value), ">", "&gt;");
    value = replace_all(std::move(value), "\"", "&quot;");
    return replace_all(std::move(value), "'", "&apos;");
}
std::string unit_quote(std::string value, bool command = true) {
    value = replace_all(std::move(value), "\\", "\\\\");
    value = replace_all(std::move(value), "\"", "\\\"");
    value = replace_all(std::move(value), "%", "%%");
    if (command)
        value = replace_all(std::move(value), "$", "$$");
    return "\"" + value + "\"";
}
} // namespace
std::string native_service_definition(std::string_view kind, const fs::path& binary, const fs::path& root) {
    const auto file = path_text(binary), directory = path_text(root);
    if (!binary.is_absolute() || !root.is_absolute() || file.find_first_of("\r\n") != file.npos ||
        directory.find_first_of("\r\n") != directory.npos)
        throw Error("Native service paths must be absolute and contain no newlines");
    const auto label = "devbox-native-" + sha256(directory).substr(0, 16);
    if (kind == "systemd")
        return "[Unit]\nDescription=Devbox native C++23 supervisor\nAfter=network-online.target\n\n"
               "[Service]\nType=simple\nWorkingDirectory=" +
               unit_quote(directory, false) + "\nExecStart=" + unit_quote(file) + " manage run --root " +
               unit_quote(directory) + "\nExecStop=" + unit_quote(file) + " manage stop --root " +
               unit_quote(directory) +
               "\nRestart=on-failure\nRestartSec=5\nTimeoutStopSec=180\nKillMode=process\nSendSIGKILL=no\n"
               "UMask=0077\n\n[Install]\nWantedBy=default.target\n";
    if (kind == "launchd")
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
               "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
               "<plist version=\"1.0\"><dict><key>Label</key><string>" +
               label + "</string><key>ProgramArguments</key><array><string>" + xml(file) +
               "</string><string>manage</string><string>run</string><string>--root</string><string>" +
               xml(directory) + "</string></array><key>WorkingDirectory</key><string>" + xml(directory) +
               "</string><key>RunAtLoad</key><true/><key>KeepAlive</key><dict><key>SuccessfulExit</"
               "key><false/>"
               "</dict><key>ThrottleInterval</key><integer>5</integer><key>ExitTimeOut</key><integer>180</"
               "integer>"
               "<key>StandardOutPath</key><string>" +
               xml(path_text(root / "run" / "native" / "service.stdout.log")) +
               "</string><key>StandardErrorPath</key><string>" +
               xml(path_text(root / "run" / "native" / "service.stderr.log")) + "</string></dict></plist>\n";
    if (kind == "windows") {
        std::string sid;
#ifdef _WIN32
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
            throw Error(windows_error());
        NativeHandle token(raw);
        DWORD bytes = 0;
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
        std::vector<unsigned char> data(bytes);
        if (!GetTokenInformation(token.get(), TokenUser, data.data(), bytes, &bytes))
            throw Error(windows_error());
        LPWSTR text = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &text))
            throw Error(windows_error());
        ScopeExit release([&] { LocalFree(text); });
        sid = narrow(text);
#else
        throw Error("Windows service definitions require the actual Windows account SID");
#endif
        const auto arguments = "manage run --root " + quote_windows_argument(directory);
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
               "<RegistrationInfo><Description>Devbox native C++23 "
               "supervisor</Description></RegistrationInfo>"
               "<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" +
               xml(sid) + "</UserId></LogonTrigger></Triggers><Principals><Principal id=\"Author\"><UserId>" +
               xml(sid) +
               "</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></"
               "Principal></Principals>"
               "<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
               "<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</"
               "StopIfGoingOnBatteries>"
               "<AllowHardTerminate>false</AllowHardTerminate><StartWhenAvailable>true</StartWhenAvailable>"
               "<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><RestartOnFailure><Interval>PT1M</"
               "Interval><Count>3</Count>"
               "</RestartOnFailure></Settings><Actions Context=\"Author\"><Exec><Command>" +
               xml(file) + "</Command><Arguments>" + xml(arguments) + "</Arguments><WorkingDirectory>" +
               xml(directory) + "</WorkingDirectory></Exec></Actions></Task>\n";
    }
    throw Error("Native service kind must be systemd, launchd or windows");
}
} // namespace devbox
