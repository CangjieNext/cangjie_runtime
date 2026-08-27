[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

$caseDir = $PSScriptRoot
$workspaceRoot = (Resolve-Path (Join-Path $caseDir "..\..\..\..\..")).Path
$sdkRoot = if ($env:MSGC_SDK_ROOT) {
    $env:MSGC_SDK_ROOT
} else {
    Join-Path $workspaceRoot "cangjie_compiler_CMSGC\output\msgc"
}
$stdxDir = Join-Path $sdkRoot "windows_x86_64_cjnative\dynamic\stdx"
$runtimeDir = Join-Path $sdkRoot "runtime\lib\windows_x86_64_cjnative"
$cjc = Join-Path $sdkRoot "bin\cjc.exe"

$payload = if ($env:CASE25_PAYLOAD) { $env:CASE25_PAYLOAD } else { "10K" }
$concurrency = if ($env:CASE25_CONCURRENCY) { $env:CASE25_CONCURRENCY } else { "8" }
$durationMs = if ($env:CASE25_DURATION_MS) { $env:CASE25_DURATION_MS } else { "60000" }
$heapSize = if ($env:HEAP_SIZE) { $env:HEAP_SIZE } else { "2gb" }
$port = if ($env:CASE25_SERVER_PORT) { $env:CASE25_SERVER_PORT } else { "18225" }
$payloadDir = if ($env:CASE25_PAYLOAD_DIR) {
    $env:CASE25_PAYLOAD_DIR
} else {
    Join-Path $caseDir "payloads"
}

$requiredFiles = @(
    $cjc,
    (Join-Path $payloadDir "$payload.json"),
    (Join-Path $stdxDir "libstdx.net.http.dll.a"),
    (Join-Path $stdxDir "libstdx.encoding.json.dll.a"),
    (Join-Path $stdxDir "libstdx.encoding.json.stream.dll.a")
)
foreach ($requiredFile in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required file not found: $requiredFile"
    }
}

$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$payloadTag = $payload.ToLowerInvariant()
$outDir = Join-Path $caseDir "result\${timestamp}_windows_dynamic_${heapSize}_c${concurrency}_${payloadTag}_msgc"
New-Item -ItemType Directory -Path $outDir | Out-Null
$benchmark = Join-Path $outDir "benchmark-dynamic.exe"

$compileArgs = @(
    "-O2",
    "--gc-mode=marksweep",
    "--dy-std",
    "--import-path", $stdxDir,
    "-L", $stdxDir,
    "--link-option", (Join-Path $stdxDir "libstdx.net.http.dll.a"),
    "--link-option", (Join-Path $stdxDir "libstdx.encoding.json.dll.a"),
    "--link-option", (Join-Path $stdxDir "libstdx.encoding.json.stream.dll.a"),
    (Join-Path $caseDir "benchmark.cj"),
    (Join-Path $caseDir "msgc\stream_transform.cj"),
    (Join-Path $caseDir "persistent_baseline.cj"),
    "-o", $benchmark
)

Write-Host "[INFO] Compiling dynamic case25 with MSGC SDK: $sdkRoot"
& $cjc @compileArgs
if ($LASTEXITCODE -ne 0) {
    throw "cjc failed with exit code $LASTEXITCODE"
}

$env:CANGJIE_HOME = $sdkRoot
$env:PATH = "$runtimeDir;$stdxDir;$(Join-Path $sdkRoot 'third_party\llvm\bin');$env:PATH"
$env:cjHeapSize = $heapSize
$env:CASE25_PAYLOAD = $payload
$env:CASE25_CONCURRENCY = $concurrency
$env:CASE25_DURATION_MS = $durationMs
$env:CASE25_SERVER_PORT = $port
$env:CASE25_PAYLOAD_DIR = $payloadDir
$env:CASE25_PERSISTENT_BYTES = if ($env:CASE25_PERSISTENT_BYTES) {
    $env:CASE25_PERSISTENT_BYTES
} else {
    "0"
}
$env:MRT_REPORT = Join-Path $outDir "gc.log"

Write-Host "[INFO] Running: $benchmark"
Write-Host "[INFO] Output: $outDir"
& $benchmark 2>&1 | Tee-Object -FilePath (Join-Path $outDir "stdout.txt")
$runExitCode = $LASTEXITCODE
if ($runExitCode -ne 0) {
    throw "case25 failed with exit code $runExitCode"
}
