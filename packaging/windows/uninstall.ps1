# Removes Glasswork Bitmap installed by install.ps1. Settings and extensions in
# %APPDATA%\Glasswork Bitmap are kept; delete that folder too for a clean slate.

$ErrorActionPreference = "SilentlyContinue"
$dest = Join-Path $env:LOCALAPPDATA "Programs\Glasswork Bitmap"

Get-Process glasswork-bitmap | Where-Object { $_.Path -like "$dest*" } | Stop-Process

Remove-Item (Join-Path ([Environment]::GetFolderPath("Programs")) "Glasswork Bitmap.lnk") -Force
Remove-Item "HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\glasswork-bitmap.exe" -Recurse -Force
foreach ($key in @("Directory\shell\GlassworkBitmap", "Directory\Background\shell\GlassworkBitmap", "*\shell\GlassworkBitmap")) {
    Remove-Item -LiteralPath "HKCU:\Software\Classes\$key" -Recurse -Force
}
Remove-Item "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\GlassworkBitmap" -Recurse -Force

# The script itself lives in the folder it deletes: finish from a copy in the temp folder.
if ($PSScriptRoot -eq $dest) {
    $copy = Join-Path $env:TEMP "glasswork-bitmap-uninstall.ps1"
    Copy-Item $PSCommandPath $copy -Force
    Start-Process powershell.exe -ArgumentList "-NoProfile -ExecutionPolicy Bypass -Command `"Start-Sleep 1; Remove-Item -LiteralPath '$dest' -Recurse -Force`"" -WindowStyle Hidden
} else {
    Remove-Item -LiteralPath $dest -Recurse -Force
}
Write-Host "Glasswork Bitmap was removed. Your settings are still in $env:APPDATA\Glasswork Bitmap."
