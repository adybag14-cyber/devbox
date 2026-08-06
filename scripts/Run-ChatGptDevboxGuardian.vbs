Option Explicit

Dim fso
Set fso = CreateObject("Scripting.FileSystemObject")

Dim shell
Set shell = CreateObject("WScript.Shell")

Function ResolvePowerShellExe()
    Dim configured, pwsh7, fallbackExe, legacyExe, programFiles, systemRoot
    configured = shell.ExpandEnvironmentStrings("%POWERSHELL_EXE%")
    If configured = "%POWERSHELL_EXE%" Then configured = ""
    If configured <> "" And fso.FileExists(configured) Then
        ResolvePowerShellExe = configured
        Exit Function
    End If

    programFiles = shell.ExpandEnvironmentStrings("%ProgramFiles%")
    pwsh7 = fso.BuildPath(programFiles, "PowerShell\7\pwsh.exe")
    If fso.FileExists(pwsh7) Then
        ResolvePowerShellExe = pwsh7
        Exit Function
    End If

    fallbackExe = shell.ExpandEnvironmentStrings("%POWERSHELL_FALLBACK_EXE%")
    If fallbackExe = "%POWERSHELL_FALLBACK_EXE%" Then fallbackExe = ""
    If fallbackExe <> "" And fso.FileExists(fallbackExe) Then
        ResolvePowerShellExe = fallbackExe
        Exit Function
    End If

    systemRoot = shell.ExpandEnvironmentStrings("%SystemRoot%")
    legacyExe = fso.BuildPath(systemRoot, "System32\WindowsPowerShell\v1.0\powershell.exe")
    ResolvePowerShellExe = legacyExe
End Function

Dim root
root = fso.GetParentFolderName(WScript.ScriptFullName)

Dim powerShellExe
powerShellExe = ResolvePowerShellExe()

Dim targetScript
targetScript = fso.BuildPath(root, "Watch-ChatGptDevboxGuardian.ps1")

Dim command
command = Chr(34) & powerShellExe & Chr(34) & _
    " -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File " & _
    Chr(34) & targetScript & Chr(34)

WScript.Quit shell.Run(command, 0, False)
