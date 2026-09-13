$root = Split-Path -Parent $PSScriptRoot
if (-not $env:VIEWER_NVENC_LEGACY_INPUT) { $env:VIEWER_NVENC_LEGACY_INPUT = "1" }
$exe = Join-Path $root "build\Release\viewer.exe"
$out = Join-Path $root "build\viewer.out.log"
$err = Join-Path $root "build\viewer.err.log"
Write-Host "Viewer starting on http://127.0.0.1:8080 (stdout: $out, stderr: $err)"
$p = Start-Process -FilePath $exe -ArgumentList @(
  "--models", (Join-Path $root "models"),
  "--web", (Join-Path $root "web"),
  "--shaders", (Join-Path $root "build\shaders"),
  "--bind", "127.0.0.1", "--port", "8080"
) -RedirectStandardOutput $out -RedirectStandardError $err -NoNewWindow -PassThru
$p.WaitForExit()
exit $p.ExitCode
