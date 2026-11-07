# Installs Glasswork Bitmap for the current user (no administrator rights needed).
#
#   powershell -ExecutionPolicy Bypass -File packaging\windows\install.ps1 [-Build <folder>] [-NoContextMenu]
#
# Copies build\glasswork-bitmap.exe and build\host to %LOCALAPPDATA%\Programs\Glasswork Bitmap, adds a
# Start menu shortcut, makes "glasswork-bitmap" run from Win+R, adds "Open with Glasswork Bitmap" to
# folders' right-click menu, and registers an uninstaller in Settings > Apps.

param(
    [string]$Build = (Join-Path $PSScriptRoot "..\..\build"),
    [switch]$NoContextMenu
)

$ErrorActionPreference = "Stop"
$Build = (Resolve-Path $Build).Path
$exe = Join-Path $Build "glasswork-bitmap.exe"
if (-not (Test-Path $exe)) {
    throw "No glasswork-bitmap.exe in $Build. Build first: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release; cmake --build build"
}
$hostSource = Join-Path $Build "host"
if (-not (Test-Path (Join-Path $hostSource "main.js"))) {
    $hostSource = Join-Path $PSScriptRoot "..\..\host"
}

$dest = Join-Path $env:LOCALAPPDATA "Programs\Glasswork Bitmap"
$running = Get-Process glasswork-bitmap -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dest*" }
if ($running) {
    throw "Glasswork Bitmap is running from $dest. Quit it first, then run the installer again."
}

New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item $exe (Join-Path $dest "glasswork-bitmap.exe") -Force
$hostDest = Join-Path $dest "host"
if (Test-Path $hostDest) {
    Remove-Item $hostDest -Recurse -Force
}
Copy-Item $hostSource $hostDest -Recurse -Force
Copy-Item (Join-Path $PSScriptRoot "uninstall.ps1") (Join-Path $dest "uninstall.ps1") -Force
Copy-Item (Join-Path $PSScriptRoot "glasswork-bitmap.ico") (Join-Path $dest "glasswork-bitmap.ico") -Force
$target = Join-Path $dest "glasswork-bitmap.exe"

# Start menu.
$programs = [Environment]::GetFolderPath("Programs")
$shell = New-Object -ComObject WScript.Shell
$link = $shell.CreateShortcut((Join-Path $programs "Glasswork Bitmap.lnk"))
$link.TargetPath = $target
$link.WorkingDirectory = $env:USERPROFILE
$link.IconLocation = "$target,0"
$link.Description = "Glasswork Bitmap code editor"
$link.Save()

# Win+R and "start glasswork-bitmap".
$appPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\glasswork-bitmap.exe"
New-Item -Path $appPath -Force | Out-Null
Set-ItemProperty -Path $appPath -Name "(default)" -Value $target
Set-ItemProperty -Path $appPath -Name "Path" -Value $dest

# "Open with Glasswork Bitmap" on folders, a folder's background, and files.
if (-not $NoContextMenu) {
    foreach ($key in @("Directory\shell\GlassworkBitmap", "Directory\Background\shell\GlassworkBitmap", "*\shell\GlassworkBitmap")) {
        $path = "HKCU:\Software\Classes\$key"
        New-Item -Path $path -Force | Out-Null
        Set-ItemProperty -Path $path -Name "(default)" -Value "Open with Glasswork Bitmap"
        Set-ItemProperty -Path $path -Name "Icon" -Value "`"$target`",0"
        $argument = if ($key -like "Directory\Background*") { "%V" } else { "%1" }
        New-Item -Path "$path\command" -Force | Out-Null
        Set-ItemProperty -Path "$path\command" -Name "(default)" -Value "`"$target`" `"$argument`""
    }
}

# Settings > Apps.
$uninstall = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\GlassworkBitmap"
New-Item -Path $uninstall -Force | Out-Null
Set-ItemProperty -Path $uninstall -Name "DisplayName" -Value "Glasswork Bitmap"
Set-ItemProperty -Path $uninstall -Name "DisplayVersion" -Value "1.0.0"
Set-ItemProperty -Path $uninstall -Name "Publisher" -Value "Glasswork"
Set-ItemProperty -Path $uninstall -Name "DisplayIcon" -Value "$target,0"
Set-ItemProperty -Path $uninstall -Name "InstallLocation" -Value $dest
Set-ItemProperty -Path $uninstall -Name "UninstallString" -Value "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$dest\uninstall.ps1`""
Set-ItemProperty -Path $uninstall -Name "NoModify" -Value 1 -Type DWord
Set-ItemProperty -Path $uninstall -Name "NoRepair" -Value 1 -Type DWord
$size = [int]((Get-ChildItem $dest -Recurse | Measure-Object Length -Sum).Sum / 1KB)
Set-ItemProperty -Path $uninstall -Name "EstimatedSize" -Value $size -Type DWord

Write-Host "Glasswork Bitmap is installed in $dest"
Write-Host "Start it from the Start menu, or run: glasswork-bitmap [folder]"
