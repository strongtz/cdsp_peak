#!/usr/bin/env pwsh

param(
    [string]$SdkRoot = "C:\Qualcomm\Hexagon_SDK\6.5.0.1",
    [string]$BuildDir = "build-windows",
    [string]$BuildType = "Release",
    [string]$Cert = $env:HEXAGON_HTP_CERT,
    [string]$CertPassword = $env:HEXAGON_HTP_CERT_PASSWORD,
    [string]$WindowsSdkBin = $env:WINDOWS_SDK_BIN
)

$ErrorActionPreference = "Stop"

$SdkRoot = (Resolve-Path $SdkRoot).Path
$SdkJson = Get-Content (Join-Path $SdkRoot "hexagon_sdk.json") -Raw | ConvertFrom-Json
$ToolsRel = $SdkJson.root.tools.info |
    Where-Object { $_.name -eq "Hexagon Tools" } |
    Select-Object -First 1 -ExpandProperty path
$ToolsRoot = Join-Path $SdkRoot $ToolsRel
$CMake = Join-Path $SdkRoot "tools\cmake-3.28.3-windows-arm64\bin\cmake.exe"

$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path $VsWhere)) {
    throw "vswhere.exe not found. Install Visual Studio with ARM64 C++ tools."
}

$VsRoot = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath
if (!$VsRoot) {
    throw "Visual Studio ARM64 C++ tools were not found."
}

$VcVars = Join-Path $VsRoot "VC\Auxiliary\Build\vcvarsall.bat"
$ClangCl = Join-Path $VsRoot "VC\Tools\Llvm\ARM64\bin\clang-cl.exe"
if (!(Test-Path $ClangCl)) {
    $ClangCl = Join-Path $VsRoot "VC\Tools\Llvm\bin\clang-cl.exe"
}

$ConfigureArgs = @(
    "`"$CMake`""
    "-S ."
    "-B `"$BuildDir`""
    "-G Ninja"
    "-DCMAKE_BUILD_TYPE=$BuildType"
    "-DCMAKE_C_COMPILER=`"$($ClangCl -replace '\\','/')`""
    "-DCDSP_PEAK_SDK_ROOT=`"$($SdkRoot -replace '\\','/')`""
    "-DHEXAGON_TOOLS_ROOT=`"$($ToolsRoot -replace '\\','/')`""
)
if ($Cert) {
    $ConfigureArgs += "-DCDSP_PEAK_HTP_CERT=`"$Cert`""
}
if ($CertPassword) {
    $ConfigureArgs += "-DCDSP_PEAK_HTP_CERT_PASSWORD=`"$CertPassword`""
}

if (!$WindowsSdkBin) {
    $candidate = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0"
    if (Test-Path (Join-Path $candidate "x86\Inf2Cat.exe")) {
        $WindowsSdkBin = $candidate
    }
}
if ($WindowsSdkBin) {
    $env:WINDOWS_SDK_BIN = $WindowsSdkBin
}

$BuildArgs = @(
    "`"$CMake`""
    "--build `"$BuildDir`""
    "-j 4"
)

$Cmd = @(
    "call `"$VcVars`" arm64"
    ($ConfigureArgs -join " ")
    ($BuildArgs -join " ")
) -join " && "

cmd.exe /d /s /c $Cmd
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
