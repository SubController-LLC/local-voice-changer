param(
  [string]$BuildDirectory = "build/windows-msvc",
  [string]$OutputDirectory = "dist"
)

$ErrorActionPreference = "Stop"
$portableDirectory = Join-Path $OutputDirectory "LocalVoiceChanger"
$zipPath = Join-Path $OutputDirectory "LocalVoiceChanger-Windows-x64.zip"
$checksumPath = Join-Path $OutputDirectory "SHA256SUMS.txt"

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
if (Test-Path $portableDirectory) {
  Remove-Item -LiteralPath $portableDirectory -Recurse -Force
}
if (Test-Path $zipPath) {
  Remove-Item -LiteralPath $zipPath -Force
}

cmake --install $BuildDirectory --config Release --prefix $portableDirectory
if ($LASTEXITCODE -ne 0) {
  throw "cmake --install failed with exit code $LASTEXITCODE"
}

Compress-Archive -Path (Join-Path $portableDirectory "*") -DestinationPath $zipPath
$hash = (Get-FileHash $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  LocalVoiceChanger-Windows-x64.zip" |
  Set-Content -LiteralPath $checksumPath -Encoding ascii

Write-Host "Created $zipPath"
Write-Host "SHA-256: $hash"
