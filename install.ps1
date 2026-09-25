# Installs the latest whoport release on Windows:
#   irm https://raw.githubusercontent.com/vqorn/whoport/main/install.ps1 | iex
$ErrorActionPreference = 'Stop'

$url = 'https://github.com/vqorn/whoport/releases/latest/download/whoport-windows-x86_64.zip'
$dest = Join-Path $env:LOCALAPPDATA 'Programs\whoport'
$zip = Join-Path $env:TEMP 'whoport-windows-x86_64.zip'

Write-Host "Downloading $url"
Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Expand-Archive -Path $zip -DestinationPath $dest -Force
Remove-Item $zip

# Add to the user's PATH once, so `whoport` works in every new terminal.
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if (-not ($userPath -split ';' | Where-Object { $_ -eq $dest })) {
    [Environment]::SetEnvironmentVariable('Path', ($userPath.TrimEnd(';') + ';' + $dest), 'User')
    Write-Host "Added $dest to your PATH. Open a new terminal to use whoport."
}

& (Join-Path $dest 'whoport.exe') --version
