param(
    [Parameter(Position = 0)]
    [ValidateSet("focus", "mark", "send-sam", "send-throw", "sam", "throw", "sample")]
    [string]$Action = "sample",

    [int]$DelayMs = 800,
    [int]$HoldRightMs = 450,
    [int]$AfterActionMs = 3500
)

$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class NativeInput {
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT {
        public uint type;
        public InputUnion u;
    }

    [StructLayout(LayoutKind.Explicit)]
    public struct InputUnion {
        [FieldOffset(0)] public KEYBDINPUT ki;
        [FieldOffset(0)] public MOUSEINPUT mi;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT {
        public ushort wVk;
        public ushort wScan;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT {
        public int dx;
        public int dy;
        public uint mouseData;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);
}
"@

$INPUT_MOUSE = 0
$INPUT_KEYBOARD = 1
$KEYEVENTF_KEYUP = 0x0002
$KEYEVENTF_SCANCODE = 0x0008
$MOUSEEVENTF_LEFTDOWN = 0x0002
$MOUSEEVENTF_LEFTUP = 0x0004
$MOUSEEVENTF_RIGHTDOWN = 0x0008
$MOUSEEVENTF_RIGHTUP = 0x0010

$SCAN_Y = 0x15

$RepoRoot = Split-Path -Parent $PSScriptRoot
$GameRoot = Split-Path -Parent (Split-Path -Parent $RepoRoot)
$SessionTrigger = Join-Path $GameRoot "DollmanMute.session"

function Get-GameProcess {
    $proc = Get-Process -Name DS2 -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } |
        Select-Object -First 1
    if (-not $proc) {
        throw "DS2 window not found"
    }
    return $proc
}

function Focus-Game {
    $proc = Get-GameProcess
    [void][NativeInput]::ShowWindow($proc.MainWindowHandle, 9)
    Start-Sleep -Milliseconds 120
    [void][NativeInput]::SetForegroundWindow($proc.MainWindowHandle)
    Start-Sleep -Milliseconds $DelayMs
    $foreground = [NativeInput]::GetForegroundWindow()
    Write-Output ("focused pid={0} title=""{1}"" foreground={2}" -f $proc.Id, $proc.MainWindowTitle, ($foreground -eq $proc.MainWindowHandle))
}

function Send-Key {
    param([int]$Scan, [int]$HoldMs = 90)
    $down = New-Object NativeInput+INPUT
    $down.type = $INPUT_KEYBOARD
    $down.u.ki.wVk = [ushort]0
    $down.u.ki.wScan = [ushort]$Scan
    $down.u.ki.dwFlags = $KEYEVENTF_SCANCODE

    $up = New-Object NativeInput+INPUT
    $up.type = $INPUT_KEYBOARD
    $up.u.ki.wVk = [ushort]0
    $up.u.ki.wScan = [ushort]$Scan
    $up.u.ki.dwFlags = $KEYEVENTF_SCANCODE -bor $KEYEVENTF_KEYUP

    [void][NativeInput]::SendInput(1, @($down), [Runtime.InteropServices.Marshal]::SizeOf([type][NativeInput+INPUT]))
    Start-Sleep -Milliseconds $HoldMs
    [void][NativeInput]::SendInput(1, @($up), [Runtime.InteropServices.Marshal]::SizeOf([type][NativeInput+INPUT]))
}

function Send-MouseButton {
    param([uint32]$DownFlag, [uint32]$UpFlag, [int]$HoldMs = 80)
    $down = New-Object NativeInput+INPUT
    $down.type = $INPUT_MOUSE
    $down.u.mi.dwFlags = $DownFlag

    $up = New-Object NativeInput+INPUT
    $up.type = $INPUT_MOUSE
    $up.u.mi.dwFlags = $UpFlag

    [void][NativeInput]::SendInput(1, @($down), [Runtime.InteropServices.Marshal]::SizeOf([type][NativeInput+INPUT]))
    Start-Sleep -Milliseconds $HoldMs
    [void][NativeInput]::SendInput(1, @($up), [Runtime.InteropServices.Marshal]::SizeOf([type][NativeInput+INPUT]))
}

function Send-Mark {
    New-Item -Path $SessionTrigger -ItemType File -Force | Out-Null
    Start-Sleep -Milliseconds 220
    Write-Output "created DollmanMute.session trigger"
}

function Send-Sam {
    Focus-Game
    Send-Key -Scan $SCAN_Y -HoldMs 120
    Write-Output "sent Y Sam-talk negative sample"
}

function Send-DollmanThrow {
    Focus-Game

    $rightDown = New-Object NativeInput+INPUT
    $rightDown.type = $INPUT_MOUSE
    $rightDown.u.mi.dwFlags = $MOUSEEVENTF_RIGHTDOWN
    [void][NativeInput]::SendInput(1, @($rightDown), [Runtime.InteropServices.Marshal]::SizeOf([type][NativeInput+INPUT]))

    Start-Sleep -Milliseconds $HoldRightMs
    Send-MouseButton -DownFlag $MOUSEEVENTF_LEFTDOWN -UpFlag $MOUSEEVENTF_LEFTUP -HoldMs 80
    Start-Sleep -Milliseconds 120

    $rightUp = New-Object NativeInput+INPUT
    $rightUp.type = $INPUT_MOUSE
    $rightUp.u.mi.dwFlags = $MOUSEEVENTF_RIGHTUP
    [void][NativeInput]::SendInput(1, @($rightUp), [Runtime.InteropServices.Marshal]::SizeOf([type][NativeInput+INPUT]))

    Write-Output "sent hold-right then left-click Dollman throw positive sample"
}

switch ($Action) {
    "focus" {
        Focus-Game
    }
    "mark" {
        Send-Mark
    }
    "send-sam" {
        Send-Sam
    }
    "send-throw" {
        Send-DollmanThrow
    }
    "sam" {
        Send-Mark
        Start-Sleep -Milliseconds 350
        Send-Sam
        Start-Sleep -Milliseconds $AfterActionMs
    }
    "throw" {
        Send-Mark
        Start-Sleep -Milliseconds 350
        Send-DollmanThrow
        Start-Sleep -Milliseconds $AfterActionMs
    }
    "sample" {
        Send-Mark
        Start-Sleep -Milliseconds 350
        Send-Sam
        Start-Sleep -Milliseconds $AfterActionMs
        Send-Mark
        Start-Sleep -Milliseconds 350
        Send-DollmanThrow
        Start-Sleep -Milliseconds $AfterActionMs
    }
}
