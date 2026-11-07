param([string]$BuildDirectory = 'D:/CodexBuilds/mka/build-release')
# Configures (first time) and builds the MK Armageddon port through buildsup.py,
# which picks the job count from free RAM and retries with fewer jobs.
# Compiler: D:/llvm22 (complete); C:/Program Files/LLVM has no lib/clang resource headers,
# so clang-cl there falls back to MSVC's intrinsics headers (unresolved _mm_set_ss).
$ErrorActionPreference = 'Stop'
$work = Resolve-Path (Join-Path $PSScriptRoot '..')
$vc = 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207'
$kit = 'C:/Program Files (x86)/Windows Kits/10'
$py = "$work/recomp-python/Scripts/python.exe"
$cmakeDir = "$work/recomp-python/Lib/site-packages/cmake/data/bin"
$env:INCLUDE = "$vc/include;$kit/Include/10.0.26100.0/ucrt;$kit/Include/10.0.26100.0/shared;$kit/Include/10.0.26100.0/um;$kit/Include/10.0.26100.0/winrt"
$env:LIB = "$vc/lib/x64;$kit/Lib/10.0.26100.0/ucrt/x64;$kit/Lib/10.0.26100.0/um/x64"
$env:PATH = "$cmakeDir;$vc/bin/Hostx64/x64;$kit/bin/10.0.26100.0/x64;$work/recomp-python/Scripts;D:/llvm22/bin;$env:PATH"
$status = "$BuildDirectory.status"
if (-not (Test-Path "$BuildDirectory/CMakeCache.txt")) {
    & "$cmakeDir/cmake.exe" -S "$PSScriptRoot/project" -B $BuildDirectory -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$work/recomp-python/Scripts/ninja.exe" `
        '-DCMAKE_C_COMPILER=D:/llvm22/bin/clang-cl.exe' `
        -DCMAKE_BUILD_TYPE=RelWithDebInfo *> "$BuildDirectory.configure.log"
    if ($LASTEXITCODE -ne 0) { "configure FAILED" | Out-File -Append $status; exit 1 }
}
& $py "$work/ps2-perf-tools/buildsup.py" $BuildDirectory mka_recomp $status
exit $LASTEXITCODE
