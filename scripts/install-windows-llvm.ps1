# Copyright (c) 2026 The vycor-cpp Authors
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$version = '22.1.8'
$name = "clang+llvm-$version-x86_64-pc-windows-msvc"
$sha256 = 'd96c2cc1736f4eb7fa43cb9bbdf56d93551a9ae0a9aadb9c99c3c3b2b712a234'
New-Item -ItemType Directory -Force $Destination | Out-Null
$Destination = (Resolve-Path $Destination).Path
$archive = Join-Path $Destination "$name.tar.xz"
$sdk = Join-Path $Destination $name
if (!(Test-Path $archive)) {
  Invoke-WebRequest "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/$name.tar.xz" -OutFile "$archive.download"
  Move-Item "$archive.download" $archive
}
if ((Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sha256) {
  throw "LLVM archive checksum mismatch: $archive"
}
if (!(Test-Path "$sdk\lib\cmake\clang\ClangConfig.cmake")) {
  # Git's Unix tar treats C: as a remote archive. Use Windows' bundled tar.
  & "$env:SystemRoot\System32\tar.exe" -xf $archive -C $Destination
  if ($LASTEXITCODE) { throw 'LLVM extraction failed' }
}
foreach ($file in @('include\clang\AST\AST.h', 'lib\clangBasic.lib',
                    'lib\cmake\llvm\LLVMConfig.cmake', 'lib\clang\22\include\stddef.h')) {
  if (!(Test-Path (Join-Path $sdk $file))) { throw "Incomplete LLVM SDK: $file" }
}
Write-Output $sdk
