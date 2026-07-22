param(
  [string]$ZipPath = "dist/LocalVoiceChanger-Windows-x64.zip",
  [string]$ChecksumPath = "dist/SHA256SUMS.txt",
  [string]$ExpectedVersion = ""
)

$ErrorActionPreference = "Stop"
$zip = (Resolve-Path $ZipPath).Path
$checksum = (Resolve-Path $ChecksumPath).Path
$temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ("lvc-package-" + [guid]::NewGuid())

function Invoke-CheckedExecutable {
  param(
    [string]$Executable,
    [string]$Argument
  )

  $process = Start-Process -FilePath $Executable -ArgumentList $Argument -PassThru
  if (-not $process.WaitForExit(10000)) {
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    throw "$([IO.Path]::GetFileName($Executable)) $Argument timed out after 10 seconds"
  }
  if ($process.ExitCode -ne 0) {
    throw "$([IO.Path]::GetFileName($Executable)) $Argument failed with exit code $($process.ExitCode)"
  }
}

try {
  $cmake = Get-Content -LiteralPath "CMakeLists.txt" -Raw
  if ($cmake -notmatch '(?s)project\s*\(\s*LocalVoiceChanger\s+VERSION\s+(\d+\.\d+\.\d+)') {
    throw "Could not read the LocalVoiceChanger project version from CMakeLists.txt"
  }
  $projectVersion = $Matches[1]
  if ([string]::IsNullOrWhiteSpace($ExpectedVersion)) {
    $ExpectedVersion = $projectVersion
  } elseif ($ExpectedVersion -ne $projectVersion) {
    throw "Expected version $ExpectedVersion does not match CMake project version $projectVersion"
  }

  $checksumBytes = [IO.File]::ReadAllBytes($checksum)
  if ($checksumBytes.Length -eq 0 -or $checksumBytes[-1] -ne 0x0a -or
      $checksumBytes -contains 0x0d) {
    throw "SHA256SUMS.txt must use a single LF line ending"
  }
  $checksumRecord = [Text.Encoding]::ASCII.GetString($checksumBytes)
  if ($checksumRecord -notmatch '^([0-9a-fA-F]{64})  LocalVoiceChanger-Windows-x64\.zip\n$') {
    throw "SHA256SUMS.txt must contain exactly one portable checksum record"
  }
  $expectedChecksum = $Matches[1].ToLowerInvariant()
  $actualChecksum = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($expectedChecksum -ne $actualChecksum) {
    throw "ZIP checksum mismatch: expected $expectedChecksum, got $actualChecksum"
  }

  New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null
  Expand-Archive -LiteralPath $zip -DestinationPath $temporaryDirectory

  $requiredFiles = @(
    "LocalVoiceChanger.exe",
    "LICENSE",
    "README.md",
    "SECURITY.md",
    "THIRD_PARTY_NOTICES.md",
    "docs/ARCHITECTURE.md",
    "docs/LISTENING_TEST.md",
    "docs/PRIVACY.md",
    "docs/WINDOWS_QA.md",
    "docs/WINDOWS_SETUP.md",
    "docs/VERIFICATION.md"
  )
  foreach ($relativePath in $requiredFiles) {
    if (-not (Test-Path (Join-Path $temporaryDirectory $relativePath))) {
      throw "Required package file is missing: $relativePath"
    }
  }
  $actualFiles = @(Get-ChildItem -Path $temporaryDirectory -Recurse -File |
      ForEach-Object {
        [IO.Path]::GetRelativePath($temporaryDirectory, $_.FullName).Replace('\', '/')
      } | Sort-Object)
  $expectedFiles = @($requiredFiles | Sort-Object)
  $fileDifferences = @(
    Compare-Object -ReferenceObject $expectedFiles -DifferenceObject $actualFiles
  )
  if ($fileDifferences.Count -ne 0) {
    $details = ($fileDifferences | Out-String).Trim()
    throw "Package file set differs from the exact public allowlist: $details"
  }

  $nativeBinaries = @(Get-ChildItem -Path $temporaryDirectory -Recurse -File |
      Where-Object { $_.Extension -in ".exe", ".dll" })
  if ($nativeBinaries.Count -ne 1 -or $nativeBinaries[0].Name -ne "LocalVoiceChanger.exe") {
    throw "The public package must contain only LocalVoiceChanger.exe as a native binary"
  }

  $sensitiveFiles = @(Get-ChildItem -Path $temporaryDirectory -Recurse -File |
      Where-Object {
        $_.Name -match '^(?i:settings|metrics)\.json$' -or
        $_.Extension -match '^\.(?i:wav|mp3|flac|ogg|m4a|log)$'
      })
  if ($sensitiveFiles.Count -ne 0) {
    throw "Package contains a settings export, audio sample, metric export, or log"
  }
  $publicText = Get-ChildItem -Path $temporaryDirectory -Recurse -File |
    Where-Object { $_.Extension -in ".md", ".txt", ".json", ".xml" } |
    ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw } | Out-String
  $privateKeyPattern = 'BEGIN (RSA |EC |OPENSSH )?PRI' + 'VATE KEY'
  $githubPattern = 'github_' + 'pat_'
  $publicPattern = "($privateKeyPattern|gh[pousr]_[A-Za-z0-9_]{20,}|$githubPattern|AKIA[0-9A-Z]{16}|[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})"
  if ($publicText -match $publicPattern) {
    throw "Package text contains a credential-like token or personal email"
  }

  $executable = (Resolve-Path (Join-Path $temporaryDirectory "LocalVoiceChanger.exe")).Path
  $bytes = [IO.File]::ReadAllBytes($executable)
  if ($bytes.Length -lt 512) {
    throw "Executable is unexpectedly small"
  }
  $peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
  $signature = [Text.Encoding]::ASCII.GetString($bytes, $peOffset, 4)
  $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
  $optionalHeader = $peOffset + 24
  $optionalMagic = [BitConverter]::ToUInt16($bytes, $optionalHeader)
  $subsystem = [BitConverter]::ToUInt16($bytes, $optionalHeader + 68)
  $dllCharacteristics = [BitConverter]::ToUInt16($bytes, $optionalHeader + 70)
  $requiredDllCharacteristics = 0x20 -bor 0x40 -bor 0x100 -bor 0x4000
  if ($signature -ne "PE`0`0" -or $machine -ne 0x8664 -or
      $optionalMagic -ne 0x20b -or $subsystem -ne 2 -or
      ($dllCharacteristics -band $requiredDllCharacteristics) -ne
        $requiredDllCharacteristics) {
    throw "Executable is not a hardened x64 PE32+ Windows GUI application"
  }

  $dumpbin = Get-ChildItem -ErrorAction SilentlyContinue `
      "${env:ProgramFiles}\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" |
    Sort-Object FullName -Descending | Select-Object -First 1
  if ($null -eq $dumpbin) {
    throw "Visual Studio dumpbin.exe was not found"
  }
  $dependencies = (& $dumpbin.FullName /nologo /dependents $executable | Out-String)
  if ($LASTEXITCODE -ne 0) {
    throw "Could not inspect executable dependencies"
  }
  if ($dependencies -match "(?i)(VCRUNTIME|MSVCP)[^\s]*\.dll") {
    throw "Executable unexpectedly depends on the Visual C++ redistributable"
  }

  $mt = Get-ChildItem -ErrorAction SilentlyContinue `
      "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\mt.exe" |
    Sort-Object FullName -Descending | Select-Object -First 1
  if ($null -eq $mt) {
    throw "Windows SDK mt.exe was not found"
  }
  $manifestPath = Join-Path $temporaryDirectory "embedded.manifest"
  & $mt.FullName -nologo "-inputresource:$executable;#1" "-out:$manifestPath"
  if ($LASTEXITCODE -ne 0) {
    throw "Could not extract the embedded application manifest"
  }
  $manifest = Get-Content -LiteralPath $manifestPath -Raw
  try {
    [xml]$manifestXml = $manifest
  } catch {
    throw "Embedded application manifest is not valid XML"
  }
  $identity = $manifestXml.SelectSingleNode(
    "/*[local-name()='assembly']/*[local-name()='assemblyIdentity']"
  )
  if ($null -eq $identity -or
      $identity.GetAttribute("name") -ne "SubController.LocalVoiceChanger" -or
      $identity.GetAttribute("version") -ne "$ExpectedVersion.0") {
    throw "Embedded manifest identity or version is incorrect"
  }
  $manifestTokens = @(
    "asInvoker",
    "supportedOS",
    "dpiAware",
    "longPathAware"
  )
  foreach ($token in $manifestTokens) {
    if (-not $manifest.Contains($token)) {
      throw "Embedded manifest is missing: $token"
    }
  }

  Invoke-CheckedExecutable -Executable $executable -Argument "--self-test"
  Invoke-CheckedExecutable -Executable $executable -Argument "--ui-smoke-test"

  $versionInfo = (Get-Item -LiteralPath $executable).VersionInfo
  if ($versionInfo.ProductName -ne "Local Voice Changer" -or
      $versionInfo.ProductVersion -ne $ExpectedVersion -or
      $versionInfo.FileVersion -ne $ExpectedVersion) {
    throw "Windows VERSIONINFO is missing or incorrect"
  }

  $signatureStatus = (Get-AuthenticodeSignature -LiteralPath $executable).Status
  Write-Host "Verified checksum: $actualChecksum"
  Write-Host "Verified hardened x64 GUI PE, VERSIONINFO, manifest, self-test, and UI smoke test"
  Write-Host "Authenticode status: $signatureStatus (unsigned beta builds are documented)"
} finally {
  if (Test-Path $temporaryDirectory) {
    Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force
  }
}
