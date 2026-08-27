[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

$caseDir = $PSScriptRoot
$scriptDir = Join-Path $caseDir "script"
$workspaceRoot = (Resolve-Path (Join-Path $caseDir "..\..\..\..\..")).Path
$platformDir = "windows_x86_64_cjnative"

function Get-Setting {
    param([string]$Name, [string]$Default)

    $value = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrWhiteSpace($value)) {
        return $Default
    }
    return $value
}

function Resolve-Tool {
    param([string]$SettingName, [string]$Fallback)

    $configured = [Environment]::GetEnvironmentVariable($SettingName)
    if (-not [string]::IsNullOrWhiteSpace($configured)) {
        return $configured
    }
    $command = Get-Command ([IO.Path]::GetFileNameWithoutExtension($Fallback)) -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    return $Fallback
}

function ConvertTo-ByteSize {
    param([string]$Value)

    $normalized = $Value.Trim().ToLowerInvariant()
    if ($normalized -notmatch '^(?<magnitude>[0-9]+)(?<unit>mb|gb)?$') {
        throw "Invalid CASE25_PERSISTENT_BYTES='$Value'; expected bytes, mb, or gb"
    }
    $multiplier = switch ($Matches.unit) {
        "mb" { 1MB }
        "gb" { 1GB }
        default { 1 }
    }
    try {
        return [Convert]::ToInt64($Matches.magnitude) * $multiplier
    } catch {
        throw "CASE25_PERSISTENT_BYTES is too large: '$Value'"
    }
}

function ConvertTo-JavaHeap {
    param([string]$Value)

    $normalized = $Value.ToLowerInvariant()
    if ($normalized -match '^(?<n>[0-9]+)(?<unit>gb|mb|kb)$') {
        $suffix = @{ gb = "g"; mb = "m"; kb = "k" }[$Matches.unit]
        return "$($Matches.n)$suffix"
    }
    return $Value
}

function Set-ChildEnvironment {
    param([hashtable]$Values)

    $saved = @{}
    foreach ($name in $Values.Keys) {
        $saved[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
        [Environment]::SetEnvironmentVariable($name, [string]$Values[$name], "Process")
    }
    return $saved
}

function Restore-ChildEnvironment {
    param([hashtable]$Values)

    foreach ($name in $Values.Keys) {
        [Environment]::SetEnvironmentVariable($name, $Values[$name], "Process")
    }
}

function Invoke-Captured {
    param(
        [string]$FilePath,
        [AllowEmptyCollection()][string[]]$ArgumentList,
        [string]$WorkingDirectory,
        [string]$StdoutPath,
        [string]$StderrPath,
        [hashtable]$Environment = @{}
    )

    $nativeArguments = (($ArgumentList | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' ')
    $saved = Set-ChildEnvironment $Environment
    try {
        $startParameters = @{
            FilePath = $FilePath
            WorkingDirectory = $WorkingDirectory
            RedirectStandardOutput = $StdoutPath
            RedirectStandardError = $StderrPath
            WindowStyle = "Hidden"
            PassThru = $true
        }
        if ($nativeArguments) {
            $startParameters.ArgumentList = $nativeArguments
        }
        $process = Start-Process @startParameters
        $null = $process.Handle
        $process.WaitForExit()
        $process.Refresh()
        return [int]$process.ExitCode
    } finally {
        Restore-ChildEnvironment $saved
    }
}

function ConvertTo-NativeArgument {
    param([string]$Value)

    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    $builder = New-Object Text.StringBuilder
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') {
            $slashes++
        } elseif ($character -eq '"') {
            [void]$builder.Append(('\' * (2 * $slashes + 1)))
            [void]$builder.Append('"')
            $slashes = 0
        } else {
            [void]$builder.Append(('\' * $slashes))
            [void]$builder.Append($character)
            $slashes = 0
        }
    }
    [void]$builder.Append(('\' * (2 * $slashes)))
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Start-MonitoredProcess {
    param(
        [string]$FilePath,
        [AllowEmptyCollection()][string[]]$ArgumentList,
        [string]$WorkingDirectory,
        [string]$StdoutPath,
        [string]$StderrPath,
        [string]$RssPath,
        [hashtable]$Environment = @{},
        [long]$MaxRuntimeMs = 0
    )

    $nativeArguments = (($ArgumentList | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' ')
    $saved = Set-ChildEnvironment $Environment
    try {
        $startParameters = @{
            FilePath = $FilePath
            WorkingDirectory = $WorkingDirectory
            RedirectStandardOutput = $StdoutPath
            RedirectStandardError = $StderrPath
            WindowStyle = "Hidden"
            PassThru = $true
        }
        if ($nativeArguments) {
            $startParameters.ArgumentList = $nativeArguments
        }
        $process = Start-Process @startParameters
        # Opening the handle before exit keeps ExitCode available in Windows PowerShell 5.1.
        $null = $process.Handle
    } finally {
        Restore-ChildEnvironment $saved
    }

    $samples = New-Object Collections.Generic.List[string]
    $lastCpu = [TimeSpan]::Zero
    $lastTime = Get-Date
    $startedAt = $lastTime
    $timedOut = $false
    $processorCount = [Environment]::ProcessorCount
    while (-not $process.HasExited) {
        if ($MaxRuntimeMs -gt 0 -and ((Get-Date) - $startedAt).TotalMilliseconds -gt $MaxRuntimeMs) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $timedOut = $true
            break
        }
        try {
            $current = Get-Process -Id $process.Id -ErrorAction Stop
            $now = Get-Date
            $elapsedSeconds = [Math]::Max(($now - $lastTime).TotalSeconds, 0.001)
            $cpuSeconds = [Math]::Max(($current.TotalProcessorTime - $lastCpu).TotalSeconds, 0)
            $cpuPercent = 100.0 * $cpuSeconds / $elapsedSeconds / $processorCount
            $timestamp = $now.ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ss.ffffff")
            $rssKb = [Math]::Round($current.WorkingSet64 / 1KB)
            $samples.Add(('{0},{1},{2:F2}' -f $timestamp, $rssKb, $cpuPercent))
            $lastCpu = $current.TotalProcessorTime
            $lastTime = $now
        } catch {
            break
        }
        Start-Sleep -Milliseconds 200
        $process.Refresh()
    }
    $process.WaitForExit()
    $process.Refresh()
    $samples | Set-Content -LiteralPath $RssPath -Encoding ASCII
    if ($timedOut) {
        Add-Content -LiteralPath $StderrPath -Value "[ERROR] process exceeded timeout ${MaxRuntimeMs}ms" -Encoding ASCII
        return 124
    }
    return [int]$process.ExitCode
}

function Write-ExitCode {
    param([string]$Path, [int]$ExitCode)
    [IO.File]::WriteAllText($Path, "$ExitCode`r`n", [Text.Encoding]::ASCII)
}

function Write-Metadata {
    param(
        [string]$Path,
        [string]$Label,
        [string]$SdkRoot,
        [string]$StdxRoot,
        [int]$Port
    )

    @(
        "case=case25_http_complex_json_stream"
        "label=$Label"
        "sdk_root=$SdkRoot"
        "stdx_dir=$StdxRoot"
        "platform_dir=$platformDir"
        "payload=$payload"
        "concurrency=$concurrency"
        "duration_ms=$durationMs"
        "persistent_bytes=$persistentBytes"
        "persistent_workers=$persistentWorkers"
        "client_mode=$clientMode"
        "send_interval_ms=$sendIntervalMs"
        "max_inflight=$maxInflight"
        "server_port=$Port"
        "heap_size=$heapSize"
        "procs=$procs"
        "timestamp=$((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))"
    ) | Set-Content -LiteralPath $Path -Encoding ASCII
}

function Get-CangjieEnvironment {
    param([string]$SdkRoot, [string]$StdxDir)

    $pathEntries = @(
        (Join-Path $SdkRoot "bin")
        (Join-Path $SdkRoot "tools\bin")
        (Join-Path $SdkRoot "runtime\lib\$platformDir")
        (Join-Path $SdkRoot "lib\$platformDir")
        (Join-Path $SdkRoot "tools\lib")
        $StdxDir
        (Join-Path $SdkRoot "third_party\llvm\bin")
        $env:PATH
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
    return @{
        CANGJIE_HOME = $SdkRoot
        PATH = $pathEntries -join ';'
    }
}

function Test-CangjieSdk {
    param([string]$Label, [string]$SdkRoot, [string]$StdxRoot, [bool]$NeedsImportLibraries)

    $stdxDir = Join-Path $StdxRoot "dynamic\stdx"
    $required = @(
        (Join-Path $SdkRoot "bin\cjc.exe")
        (Join-Path $SdkRoot "runtime\lib\$platformDir\libcangjie-runtime.dll")
        (Join-Path $stdxDir "libstdx.net.http.dll")
        (Join-Path $stdxDir "libstdx.encoding.json.dll")
        (Join-Path $stdxDir "libstdx.encoding.json.stream.dll")
    )
    if ($NeedsImportLibraries) {
        $required += @(
            (Join-Path $stdxDir "libstdx.net.http.dll.a")
            (Join-Path $stdxDir "libstdx.encoding.json.dll.a")
            (Join-Path $stdxDir "libstdx.encoding.json.stream.dll.a")
        )
    }
    $missing = @($required | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
    if ($missing.Count -gt 0) {
        Write-Warning "Skipping ${Label}: missing $($missing -join ', ')"
        return $false
    }
    return $true
}

function Invoke-CangjieSide {
    param(
        [string]$Label,
        [string]$OutputDirectory,
        [string]$SdkRoot,
        [string]$StdxRoot,
        [int]$Port,
        [bool]$IsMsgc
    )

    $stdxDir = Join-Path $StdxRoot "dynamic\stdx"
    $transform = Join-Path $caseDir "$($Label.ToLowerInvariant())\stream_transform.cj"
    Write-Metadata (Join-Path $OutputDirectory "run_metadata.env") $Label $SdkRoot $StdxRoot $Port
    @(
        "cjc_gc_mode=$(if ($IsMsgc) { 'marksweep' } else { '' })"
        "cj_heap_growth=$(if ($IsMsgc) { $msgcHeapGrowth } else { '' })"
        "cj_scavenge_growth=$(if ($IsMsgc) { $msgcScavengeGrowth } else { '' })"
        "cj_ms_min_gc_threshold=$(if ($IsMsgc) { $msgcMinGcThreshold } else { '' })"
    ) | Add-Content -LiteralPath (Join-Path $OutputDirectory "run_metadata.env") -Encoding ASCII
    Copy-Item -LiteralPath (Join-Path $caseDir "benchmark.cj") -Destination (Join-Path $OutputDirectory "benchmark.cj.snapshot")
    Copy-Item -LiteralPath (Join-Path $payloadDir "$payload.json") -Destination (Join-Path $OutputDirectory "payload.snapshot.json")
    Copy-Item -LiteralPath $transform -Destination (Join-Path $OutputDirectory "stream_transform.cj.snapshot")

    $benchmark = Join-Path $OutputDirectory "benchmark.exe"
    $linkExtension = if ($IsMsgc) { ".dll.a" } else { ".dll" }
    $compileArgs = @("-O2")
    if ($IsMsgc) {
        $compileArgs += "--gc-mode=marksweep"
    }
    $compileArgs += @(
        "--dy-std"
        "--import-path", $stdxDir
        "-L", $stdxDir
        "--link-option", (Join-Path $stdxDir "libstdx.net.http$linkExtension")
        "--link-option", (Join-Path $stdxDir "libstdx.encoding.json$linkExtension")
        "--link-option", (Join-Path $stdxDir "libstdx.encoding.json.stream$linkExtension")
        (Join-Path $caseDir "benchmark.cj")
        $transform
        (Join-Path $caseDir "persistent_baseline.cj")
        "-o", $benchmark
    )
    $childEnvironment = Get-CangjieEnvironment $SdkRoot $stdxDir
    $compileRc = Invoke-Captured (Join-Path $SdkRoot "bin\cjc.exe") $compileArgs $OutputDirectory `
        (Join-Path $OutputDirectory "compile.stdout.txt") (Join-Path $OutputDirectory "compile.stderr.txt") $childEnvironment
    Write-ExitCode (Join-Path $OutputDirectory "compile.exitcode") $compileRc
    if ($compileRc -ne 0 -or -not (Test-Path -LiteralPath $benchmark -PathType Leaf)) {
        Write-Host "[INFO] $Label compile rc=$compileRc"
        return $(if ($compileRc -ne 0) { $compileRc } else { 1 })
    }

    $childEnvironment.cjHeapSize = $heapSize
    $childEnvironment.CASE25_PAYLOAD = $payload
    $childEnvironment.CASE25_CONCURRENCY = $concurrency
    $childEnvironment.CASE25_DURATION_MS = $durationMs
    $childEnvironment.CASE25_PERSISTENT_BYTES = $persistentBytes
    $childEnvironment.CASE25_PERSISTENT_WORKERS = $persistentWorkers
    $childEnvironment.CASE25_SERVER_PORT = $Port
    $childEnvironment.CASE25_PAYLOAD_DIR = $payloadDir
    $childEnvironment.CASE25_CLIENT_MODE = $clientMode
    $childEnvironment.CASE25_SEND_INTERVAL_MS = $sendIntervalMs
    $childEnvironment.CASE25_MAX_INFLIGHT = $maxInflight
    $childEnvironment.MRT_REPORT = Join-Path $OutputDirectory "gc.log"
    $childEnvironment.MRT_LOG_FILE_SIZE = "200mb"
    if ($procs) {
        $childEnvironment.cjProcessorNum = $procs
    }
    if ($IsMsgc) {
        $childEnvironment.cjHeapGrowth = $msgcHeapGrowth
        $childEnvironment.cjScavengeGrowth = $msgcScavengeGrowth
        $childEnvironment.cjMSMinGCThreshold = $msgcMinGcThreshold
    }
    $runRc = Start-MonitoredProcess $benchmark @() $OutputDirectory `
        (Join-Path $OutputDirectory "stdout.txt") (Join-Path $OutputDirectory "stderr.txt") `
        (Join-Path $OutputDirectory "rss.csv") $childEnvironment $processTimeoutMs
    Write-ExitCode (Join-Path $OutputDirectory "run.exitcode") $runRc
    Write-Host "[INFO] $Label rc=$runRc"
    return $runRc
}

function Get-SharedBenchmarkEnvironment {
    param([int]$Port)

    return @{
        CASE27_PAYLOAD = $payload
        CASE27_CONCURRENCY = $concurrency
        CASE27_DURATION_MS = $durationMs
        CASE27_PERSISTENT_BYTES = $persistentBytes
        CASE27_PERSISTENT_WORKERS = $persistentWorkers
        CASE27_SERVER_PORT = $Port
        CASE27_PAYLOAD_DIR = $payloadDir
        CASE27_CLIENT_MODE = $clientMode
        CASE27_SEND_INTERVAL_MS = $sendIntervalMs
        CASE27_MAX_INFLIGHT = $maxInflight
    }
}

function Invoke-GoSide {
    param([string]$OutputDirectory, [int]$Port)

    Write-Metadata (Join-Path $OutputDirectory "run_metadata.env") "Go" "N/A" "N/A" $Port
    $version = (& $goBin version 2>&1 | Out-String).Trim()
    @("go_bin=$goBin", "go_version=$version", "gomaxprocs=$procs", "go_godebug=$goGodebug", "go_gomemlimit=$goMemLimit") |
        Add-Content -LiteralPath (Join-Path $OutputDirectory "run_metadata.env") -Encoding ASCII
    Copy-Item -LiteralPath (Join-Path $payloadDir "$payload.json") -Destination (Join-Path $OutputDirectory "payload.snapshot.json")
    $benchmark = Join-Path $OutputDirectory "benchmark_go.exe"
    $compileEnvironment = @{ GOCACHE = Join-Path $env:TEMP "case25-go-build-cache" }
    $compileRc = Invoke-Captured $goBin @("build", "-o", $benchmark, ".") (Join-Path $caseDir "go_benchmark") `
        (Join-Path $OutputDirectory "compile.stdout.txt") (Join-Path $OutputDirectory "compile.stderr.txt") $compileEnvironment
    Write-ExitCode (Join-Path $OutputDirectory "compile.exitcode") $compileRc
    if ($compileRc -ne 0) {
        Write-Host "[INFO] Go compile rc=$compileRc"
        return $compileRc
    }

    $runEnvironment = Get-SharedBenchmarkEnvironment $Port
    $runEnvironment.GODEBUG = $goGodebug
    $runEnvironment.GOMEMLIMIT = $goMemLimit
    if ($goGogc) { $runEnvironment.GOGC = $goGogc }
    if ($procs) { $runEnvironment.GOMAXPROCS = $procs }
    $runRc = Start-MonitoredProcess $benchmark @() $OutputDirectory `
        (Join-Path $OutputDirectory "stdout.txt") (Join-Path $OutputDirectory "gc.log") `
        (Join-Path $OutputDirectory "rss.csv") $runEnvironment $processTimeoutMs
    Write-ExitCode (Join-Path $OutputDirectory "run.exitcode") $runRc
    Write-Host "[INFO] Go rc=$runRc"
    return $runRc
}

function Invoke-JavaSide {
    param([string]$OutputDirectory, [int]$Port)

    Write-Metadata (Join-Path $OutputDirectory "run_metadata.env") "Java" "N/A" "N/A" $Port
    $version = (& $javacBin -version | Out-String).Trim()
    @("java_bin=$javaBin", "javac_bin=$javacBin", "java_opts=-Xmx$javaHeap", "java_active_processor_count=$(if ($procs) { $procs } else { 'host-default' })", "java_version=$version") |
        Add-Content -LiteralPath (Join-Path $OutputDirectory "run_metadata.env") -Encoding ASCII
    Copy-Item -LiteralPath (Join-Path $caseDir "java_benchmark\Case27HttpComplexJsonBenchmark.java") -Destination (Join-Path $OutputDirectory "Case27HttpComplexJsonBenchmark.java.snapshot")
    Copy-Item -LiteralPath (Join-Path $caseDir "java_benchmark\Case27HttpComplexJsonBenchmarkTest.java") -Destination (Join-Path $OutputDirectory "Case27HttpComplexJsonBenchmarkTest.java.snapshot")
    Copy-Item -LiteralPath $jacksonCoreJar -Destination (Join-Path $OutputDirectory "jackson-core-2.17.2.jar.snapshot")
    Copy-Item -LiteralPath (Join-Path $payloadDir "$payload.json") -Destination (Join-Path $OutputDirectory "payload.snapshot.json")

    $classes = Join-Path $OutputDirectory "classes"
    New-Item -ItemType Directory -Path $classes | Out-Null
    $compileArgs = @(
        "-cp", $jacksonCoreJar
        "-d", $classes
        (Join-Path $caseDir "java_benchmark\Case27HttpComplexJsonBenchmark.java")
        (Join-Path $caseDir "java_benchmark\Case27HttpComplexJsonBenchmarkTest.java")
    )
    $compileRc = Invoke-Captured $javacBin $compileArgs $OutputDirectory `
        (Join-Path $OutputDirectory "compile.stdout.txt") (Join-Path $OutputDirectory "compile.stderr.txt") @{}
    Write-ExitCode (Join-Path $OutputDirectory "compile.exitcode") $compileRc
    if ($compileRc -ne 0) {
        Write-Host "[INFO] Java compile rc=$compileRc"
        return $compileRc
    }

    $classPath = "$classes;$jacksonCoreJar"
    $commonArgs = @("--add-modules", "jdk.httpserver")
    if ($procs) { $commonArgs += "-XX:ActiveProcessorCount=$procs" }
    $commonArgs += @("-Xmx$javaHeap", "-cp", $classPath)
    $runEnvironment = Get-SharedBenchmarkEnvironment $Port
    $testRc = Invoke-Captured $javaBin ($commonArgs + "Case27HttpComplexJsonBenchmarkTest") (Join-Path $caseDir "java_benchmark") `
        (Join-Path $OutputDirectory "test.stdout.txt") (Join-Path $OutputDirectory "test.stderr.txt") $runEnvironment
    Write-ExitCode (Join-Path $OutputDirectory "test.exitcode") $testRc
    if ($testRc -ne 0) {
        Write-Host "[INFO] Java test rc=$testRc"
        return $testRc
    }

    $runArgs = @("--add-modules", "jdk.httpserver")
    if ($procs) { $runArgs += "-XX:ActiveProcessorCount=$procs" }
    $runArgs += @("-Xmx$javaHeap", "-Xlog:gc*:file=gc.log:time,uptime,level,tags", "-cp", $classPath, "Case27HttpComplexJsonBenchmark")
    $runRc = Start-MonitoredProcess $javaBin $runArgs $OutputDirectory `
        (Join-Path $OutputDirectory "stdout.txt") (Join-Path $OutputDirectory "stderr.txt") `
        (Join-Path $OutputDirectory "rss.csv") $runEnvironment $processTimeoutMs
    Write-ExitCode (Join-Path $OutputDirectory "run.exitcode") $runRc
    Write-Host "[INFO] Java rc=$runRc"
    return $runRc
}

$msgcSdkRoot = Get-Setting "MSGC_SDK_ROOT" (Join-Path $workspaceRoot "cangjie_compiler_CMSGC\output\msgc")
$msgcStdxRoot = Get-Setting "MSGC_STDX_ROOT" (Join-Path $msgcSdkRoot $platformDir)
$copygcPackageRoot = Get-Setting "COPYGC_PACKAGE_ROOT" "C:\cangjie-pkg\main\20260827"
$copygcSdkRoot = Get-Setting "COPYGC_SDK_ROOT" (Join-Path $copygcPackageRoot "cangjie")
$copygcStdxRoot = Get-Setting "COPYGC_STDX_ROOT" (Join-Path $copygcPackageRoot $platformDir)

$payload = Get-Setting "CASE25_PAYLOAD" "100K"
$concurrency = Get-Setting "CASE25_CONCURRENCY" "1000"
$durationMs = Get-Setting "CASE25_DURATION_MS" "60000"
$processTimeoutMs = Get-Setting "CASE25_PROCESS_TIMEOUT_MS" ([string]([long]$durationMs + 120000))
$heapSize = Get-Setting "HEAP_SIZE" "16gb"
$procs = Get-Setting "PROCS" ""
$msgcHeapGrowth = Get-Setting "MSGC_HEAP_GROWTH" "1"
$msgcScavengeGrowth = Get-Setting "MSGC_SCAVENGE_GROWTH" "1.10"
$msgcMinGcThreshold = Get-Setting "MSGC_MIN_GC_THRESHOLD" "20mb"
$clientMode = Get-Setting "CASE25_CLIENT_MODE" "sync"
$sendIntervalMs = Get-Setting "CASE25_SEND_INTERVAL_MS" "1"
$maxInflight = Get-Setting "CASE25_MAX_INFLIGHT" "32"
$persistentInput = Get-Setting "CASE25_PERSISTENT_BYTES" "0"
$persistentBytes = ConvertTo-ByteSize $persistentInput
$persistentWorkers = Get-Setting "CASE25_PERSISTENT_WORKERS" "8"
$copygcEnabled = (Get-Setting "COPYGC_ENABLE" "1") -eq "1"
$msgcEnabled = (Get-Setting "MSGC_ENABLE" "1") -eq "1"
$goEnabled = (Get-Setting "GO_ENABLE" "1") -eq "1"
$javaEnabled = (Get-Setting "JAVA_ENABLE" "1") -eq "1"
$goGodebug = Get-Setting "GO_GODEBUG" "gctrace=1"
$goGogc = Get-Setting "GO_GOGC" ""
$goMemLimit = Get-Setting "GO_GOMEMLIMIT" "16GiB"
$goBin = Resolve-Tool "GO_BIN" "C:\Program Files\Go\bin\go.exe"
$javaBin = Resolve-Tool "JAVA_BIN" "java.exe"
$javacBin = Resolve-Tool "JAVAC_BIN" "javac.exe"
$pythonBin = Resolve-Tool "PYTHON_BIN" "python.exe"
$javaHeap = ConvertTo-JavaHeap $heapSize
$jacksonCoreJar = Join-Path $caseDir "java_benchmark\lib\jackson-core-2.17.2.jar"
$payloadDir = Join-Path $caseDir "payloads"

if (-not (Test-Path -LiteralPath (Join-Path $payloadDir "$payload.json") -PathType Leaf)) {
    throw "Payload not found: $payloadDir\$payload.json"
}
if ($procs -and ($procs -notmatch '^[1-9][0-9]*$' -or [int]$procs -gt [Environment]::ProcessorCount)) {
    throw "PROCS must be between 1 and $([Environment]::ProcessorCount), got '$procs'"
}
if ($persistentWorkers -notmatch '^[1-9][0-9]*$') {
    throw "CASE25_PERSISTENT_WORKERS must be a positive integer, got '$persistentWorkers'"
}
if ($processTimeoutMs -notmatch '^[1-9][0-9]*$') {
    throw "CASE25_PROCESS_TIMEOUT_MS must be a positive integer, got '$processTimeoutMs'"
}

if ($copygcEnabled -and -not (Test-CangjieSdk "CopyGC" $copygcSdkRoot $copygcStdxRoot $false)) { $copygcEnabled = $false }
if ($msgcEnabled -and -not (Test-CangjieSdk "MSGC" $msgcSdkRoot $msgcStdxRoot $true)) { $msgcEnabled = $false }
if ($goEnabled -and -not (Test-Path -LiteralPath $goBin -PathType Leaf)) {
    Write-Warning "Skipping Go: executable unavailable: $goBin"
    $goEnabled = $false
}
if ($javaEnabled -and ((-not (Get-Command $javaBin -ErrorAction SilentlyContinue)) -or (-not (Get-Command $javacBin -ErrorAction SilentlyContinue)) -or (-not (Test-Path -LiteralPath $jacksonCoreJar -PathType Leaf)))) {
    Write-Warning "Skipping Java: java, javac, or Jackson Core is unavailable"
    $javaEnabled = $false
}
if (-not ($copygcEnabled -or $msgcEnabled -or $goEnabled -or $javaEnabled)) {
    throw "No runnable implementation remains after dependency checks"
}

$dateTag = Get-Date -Format "yyyyMMdd"
$payloadTag = $payload.ToLowerInvariant()
$persistentMiB = [Math]::Floor($persistentBytes / 1MB)
$procsTag = if ($procs) { $procs } else { "Auto" }
$base = Join-Path $caseDir "result\${dateTag}_${persistentMiB}M_c${concurrency}_${payloadTag}_HeapGrow${msgcHeapGrowth}_Scavenge${msgcScavengeGrowth}_Procs${procsTag}_compare_windows"
$compareDir = $base
$suffix = 1
while (Test-Path -LiteralPath $compareDir) {
    $compareDir = "${base}_$suffix"
    $suffix++
}
New-Item -ItemType Directory -Path $compareDir | Out-Null

$sideDirectories = @{}
foreach ($side in @("copygc", "msgc", "go", "java")) {
    $enabled = Get-Variable -Name "${side}Enabled" -ValueOnly
    if ($enabled) {
        $sideDirectories[$side] = Join-Path $compareDir $side
        New-Item -ItemType Directory -Path $sideDirectories[$side] | Out-Null
    }
}

Write-Host "[INFO] compare dir: $compareDir"
Write-Host "[INFO] platform: $platformDir"
Write-Host "[INFO] config: payload=$payload concurrency=$concurrency duration=${durationMs}ms heap=$heapSize persistent=$persistentInput procs=$(if ($procs) { $procs } else { 'auto' })"
Write-Host "[INFO] SDK MSGC=$msgcSdkRoot"
Write-Host "[INFO] SDK CopyGC=$copygcSdkRoot"
Write-Host "[INFO] Go=$goBin Java=$javaBin javac=$javacBin"

$results = [ordered]@{}
if ($copygcEnabled) { $results.CopyGC = Invoke-CangjieSide "CopyGC" $sideDirectories.copygc $copygcSdkRoot $copygcStdxRoot 18226 $false }
if ($msgcEnabled) { $results.MSGC = Invoke-CangjieSide "MSGC" $sideDirectories.msgc $msgcSdkRoot $msgcStdxRoot 18230 $true }
if ($goEnabled) { $results.Go = Invoke-GoSide $sideDirectories.go 18234 }
if ($javaEnabled) { $results.Java = Invoke-JavaSide $sideDirectories.java 18238 }

if ($results.Count -ge 2 -and (Get-Command $pythonBin -ErrorAction SilentlyContinue)) {
    $compareArgs = @((Join-Path $scriptDir "compare_results.py"))
    foreach ($entry in $results.GetEnumerator()) {
        $compareArgs += @("--run", "$($entry.Key)=$($sideDirectories[$entry.Key.ToLowerInvariant()])")
    }
    $compareArgs += @("--output", (Join-Path $compareDir "compare_report.md"))
    & $pythonBin -X utf8 @compareArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "compare_results.py failed with exit code $LASTEXITCODE"
    }
}

Write-Host "[DONE] compare_dir=$compareDir"
$failed = @($results.Values | Where-Object { $_ -ne 0 } | Select-Object -First 1)
if ($failed.Count -gt 0) {
    exit $failed[0]
}
exit 0
