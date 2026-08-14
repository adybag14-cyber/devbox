function Split-WindowsCommandLine {
    param([Parameter(Mandatory = $true)][string]$CommandLine)

    if (-not ('DevboxMcpCommandLineNativeMethods' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class DevboxMcpCommandLineNativeMethods {
    [DllImport("shell32.dll", SetLastError = true)]
    private static extern IntPtr CommandLineToArgvW(
        [MarshalAs(UnmanagedType.LPWStr)] string commandLine,
        out int argumentCount
    );

    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr memory);

    public static string[] Split(string commandLine) {
        int count;
        IntPtr argv = CommandLineToArgvW(commandLine, out count);
        if (argv == IntPtr.Zero) {
            return Array.Empty<string>();
        }
        try {
            var arguments = new List<string>(count);
            for (int i = 0; i < count; i++) {
                IntPtr value = Marshal.ReadIntPtr(argv, i * IntPtr.Size);
                arguments.Add(Marshal.PtrToStringUni(value) ?? string.Empty);
            }
            return arguments.ToArray();
        } finally {
            LocalFree(argv);
        }
    }
}
'@
    }

    return @([DevboxMcpCommandLineNativeMethods]::Split($CommandLine))
}

function Resolve-McpComparablePath {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }
    try {
        $resolved = (Get-Item -LiteralPath $Path -Force -ErrorAction Stop).FullName
    } catch {
        try {
            $resolved = [IO.Path]::GetFullPath($Path)
        } catch {
            return $null
        }
    }
    return ([string]$resolved).TrimEnd('\').Replace('/', '\').ToLowerInvariant()
}

function Test-IsPathRootedSafe {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    try { return [IO.Path]::IsPathRooted($Path) }
    catch { return $false }
}

function Test-IsOwnedServerCommandLine {
    param(
        [string]$CommandLine,
        [string]$ProjectRoot
    )

    if ([string]::IsNullOrWhiteSpace($CommandLine)) {
        return $false
    }

    if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
        return $false
    }

    $normalizedRoot = Resolve-McpComparablePath -Path $ProjectRoot
    if ([string]::IsNullOrWhiteSpace($normalizedRoot)) {
        return $false
    }
    $expectedServer = "$normalizedRoot\src\server.js"
    $expectedRuntimeEnv = "$normalizedRoot\.env.runtime"
    $expectedRust = "$normalizedRoot\rust-mcp\target\release\devbox-mcp.exe"
    $expectedVersionedRustDir = "$normalizedRoot\run\bin"
    $arguments = @(Split-WindowsCommandLine -CommandLine ([string]$CommandLine))
    if ($arguments.Count -eq 0) {
        return $false
    }

    $executableArgument = [string]$arguments[0]
    $executable = if (Test-IsPathRootedSafe -Path $executableArgument) {
        Resolve-McpComparablePath -Path $executableArgument
    } else {
        $null
    }
    if ($executable -eq $expectedRust) {
        return $true
    }
    if (-not [string]::IsNullOrWhiteSpace($executable)) {
        $executableDirectory = [IO.Path]::GetDirectoryName($executable)
        $executableName = [IO.Path]::GetFileName($executable)
        if (
            $executableDirectory -eq $expectedVersionedRustDir -and
            $executableName -match '^devbox-mcp-[a-z0-9]+-[a-f0-9]{16}\.exe$'
        ) {
            return $true
        }
    }

    $serverMatches = $false
    $runtimeEnvMatches = $false
    for ($i = 1; $i -lt $arguments.Count; $i++) {
        $argument = [string]$arguments[$i]
        if ($argument.StartsWith('--env-file=', [StringComparison]::OrdinalIgnoreCase)) {
            $runtimePath = $argument.Substring('--env-file='.Length)
            $runtimeEnvMatches = (Test-IsPathRootedSafe -Path $runtimePath) -and ((Resolve-McpComparablePath -Path $runtimePath) -eq $expectedRuntimeEnv)
            continue
        }
        if ($argument.Equals('--env-file', [StringComparison]::OrdinalIgnoreCase) -and ($i + 1) -lt $arguments.Count) {
            $i++
            $runtimePath = [string]$arguments[$i]
            $runtimeEnvMatches = (Test-IsPathRootedSafe -Path $runtimePath) -and ((Resolve-McpComparablePath -Path $runtimePath) -eq $expectedRuntimeEnv)
            continue
        }
        if ((Test-IsPathRootedSafe -Path $argument) -and ((Resolve-McpComparablePath -Path $argument) -eq $expectedServer)) {
            $serverMatches = $true
        }
    }
    return ($serverMatches -and $runtimeEnvMatches)
}
