Option Explicit

Dim fso
Set fso = CreateObject("Scripting.FileSystemObject")

Dim shell
Set shell = CreateObject("WScript.Shell")

Dim root
root = fso.GetParentFolderName(WScript.ScriptFullName)

Dim powerShellExe
powerShellExe = shell.ExpandEnvironmentStrings("%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe")

Dim guardianScript
guardianScript = fso.BuildPath(root, "Ensure-ChatGptDevboxGuardian.ps1")

Dim command
command = """" & powerShellExe & """" & _
    " -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File " & _
    """" & guardianScript & """"

WScript.Quit shell.Run(command, 0, True)
