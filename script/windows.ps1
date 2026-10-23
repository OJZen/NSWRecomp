param(
    [ValidateSet('test', 'inspect', 'generate', 'check-c', 'probe', 'build', 'load-only', 'startup', 'vulkan-probe')]
    [string]$Action = 'test',
    [string]$Exefs,
    [string]$Profile,
    [string]$Module = 'rtld',
    [ValidateSet('_init_libc0', 'nnosInitialize', '_init_libc1', 'nninitInitializeSdkModule', 'nninitStartup', '_init_libc2', 'nnMain')]
    [string]$Through = 'nninitInitializeSdkModule',
    [ValidateRange(1, 600)][int]$Seconds = 30,
    [ValidateSet('Kernel', 'BasicServices', 'Vulkan')][string]$Runtime = 'Kernel',
    [ValidateRange(-1, 31)][int]$VulkanAdapter = -1,
    [string]$NpdmSha256,
    [string]$Romfs,
    [string]$RomfsProfile,
    [switch]$ScratchFilesystem,
    [switch]$VulkanVideo,
    [switch]$Window,
    [ValidateRange(1, 8)][int]$Jobs = 8,
    [ValidateSet('1', '2')][string]$OptLevel = '2',
    [string]$Python,
    [string]$ClangBin
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (!$Python) { $Python = Join-Path $root 'work\venv\Scripts\python.exe' }
$names = 'PATH', 'TMP', 'TEMP', 'TMPDIR', 'PYTHONDONTWRITEBYTECODE', 'NSWRECOMP_PROFILE', 'NSWRECOMP_OPT_LEVEL'
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
Push-Location $root
try {
    New-Item -ItemType Directory -Force work\tmp, work\logs | Out-Null
    $env:TMP = $env:TEMP = $env:TMPDIR = Join-Path $root 'work\tmp'
    $env:PYTHONDONTWRITEBYTECODE = '1'
    $env:NSWRECOMP_OPT_LEVEL = $OptLevel
    if ($ClangBin) { $env:PATH = "$(Resolve-Path -LiteralPath $ClangBin);$env:PATH" }
    if ($Profile) { $env:NSWRECOMP_PROFILE = (Resolve-Path -LiteralPath $Profile).Path }
    if ($Action -eq 'test') {
        & $Python tools/test_core.py
    } elseif ($Action -eq 'vulkan-probe') {
        & $Python tools/vulkan_probe.py --adapter $VulkanAdapter
    } else {
        if (!$Exefs) { throw '-Exefs is required for this action' }
        switch ($Action) {
            'build' { & $Python tools/build.py --exefs $Exefs --jobs $Jobs }
            'load-only' { & $Python tools/load.py --exefs $Exefs }
            'startup' {
                $modeArgs = if ($Runtime -in @('BasicServices', 'Vulkan')) {
                    if (!$NpdmSha256) { throw '-NpdmSha256 is required for BasicServices or Vulkan' }
                    @('--basic-services', '--npdm-sha256', $NpdmSha256)
                } else { @('--kernel-only') }
                if ($ScratchFilesystem) { $modeArgs += '--scratch-filesystem' }
                if ($Runtime -eq 'Vulkan') { $modeArgs += @('--vulkan-headless', '--vulkan-adapter', $VulkanAdapter) }
                if ($VulkanVideo) { $modeArgs += '--vulkan-video' }
                if ($Window) { $modeArgs += '--vulkan-window' }
                if ($Romfs) {
                    if ($Runtime -eq 'Kernel' -or !$RomfsProfile) { throw '-Romfs requires BasicServices/Vulkan and -RomfsProfile' }
                    $modeArgs += @('--romfs', (Resolve-Path -LiteralPath $Romfs).Path, '--romfs-profile', (Resolve-Path -LiteralPath $RomfsProfile).Path)
                } elseif ($RomfsProfile) { throw '-RomfsProfile requires -Romfs' }
                & $Python tools/startup.py --exefs $Exefs --through $Through --seconds $Seconds @modeArgs
            }
            default { & $Python tools/recompile.py $Action --exefs $Exefs --module $Module }
        }
    }
    $result = $LASTEXITCODE
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    Pop-Location
}
exit $result
