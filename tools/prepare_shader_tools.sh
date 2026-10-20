#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p work/tools work/tmp work/logs
export TMPDIR="$PWD/work/tmp/" PYTHONDONTWRITEBYTECODE=1
export DOTNET_ROOT="$PWD/work/tools/dotnet" DOTNET_CLI_HOME="$PWD/work/tools/dotnet-home"
export NUGET_PACKAGES="$PWD/work/tools/nuget" DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
shader_tmp=$(mktemp -d "$PWD/work/tmp/shader-tools.XXXXXX")
trap 'rm -rf "$shader_tmp"' EXIT
if [[ ! -x "$DOTNET_ROOT/dotnet" ]]; then
    curl --fail --location --retry 3 --output "$shader_tmp/dotnet.tar.gz" \
        https://builds.dotnet.microsoft.com/dotnet/Sdk/10.0.401/dotnet-sdk-10.0.401-osx-arm64.tar.gz
    printf '%s  %s\n' 69f64eb00dc045398755c440b152225d544301a345a146a16e86a56a0c52b7c94b2c331520e976dbb821f18d31930aafbd25bb85961e3517e0665414ce0cbcff "$shader_tmp/dotnet.tar.gz" | shasum -a 512 -c -
    mkdir -p "$DOTNET_ROOT"
    tar -xzf "$shader_tmp/dotnet.tar.gz" -C "$DOTNET_ROOT"
fi
[[ "$("$DOTNET_ROOT/dotnet" --version)" == 10.0.401 ]] || { echo 'Expected the pinned .NET SDK 10.0.401' >&2; exit 1; }
if [[ ! -x work/tools/spirv-cross ]]; then
    git init --quiet "$shader_tmp/spirv"
    git -C "$shader_tmp/spirv" fetch --quiet --depth 1 https://github.com/KhronosGroup/SPIRV-Cross.git aa217aeb6c9f0ace7a0ab233b28807edf45eb165
    git -C "$shader_tmp/spirv" checkout --quiet --detach FETCH_HEAD
    cmake -S "$shader_tmp/spirv" -B "$shader_tmp/build" -DCMAKE_BUILD_TYPE=Release -DSPIRV_CROSS_ENABLE_TESTS=OFF > work/logs/spirv-build.log 2>&1
    cmake --build "$shader_tmp/build" -j 1 >> work/logs/spirv-build.log 2>&1
    cp "$shader_tmp/build/spirv-cross" work/tools/spirv-cross
    cp "$shader_tmp/spirv/LICENSE" work/tools/spirv-cross.LICENSE
fi
work/venv/bin/python - <<'PY'
from pathlib import Path
import os, shutil
source = Path('third_party/zelda-mobile/core/gfx/tools/dmsh_xlate')
target = Path('work/tools/xlate-source'); target.mkdir(exist_ok=True)
code = (source / 'Program.cs').read_text()
anchor = 'false /* \u6df1\u5ea6 0..1 | depth 0..1 */'
if code.count(anchor) != 1:
    raise SystemExit('Inspect the upstream depth-mode argument before preparing the translator')
code = code.replace(anchor, 'true /* Zelda uses minus-one-to-one depth */')
anchor = 'public bool QueryHostSupportsTextureShadowLod() => true;'
if code.count(anchor) != 1:
    raise SystemExit('Inspect the upstream host capability queries before preparing the translator')
# Metal has no FP64; reuse the translator's existing double-to-float lowering.
code = code.replace(anchor, 'public bool QueryHostSupportsDepthClipControl() => false;\n        public bool QueryHostSupportsShaderFloat64() => false;\n        ' + anchor)
anchor = 'ShaderProgram p = ctx.Translate();'
if code.count(anchor) != 1:
    raise SystemExit('Inspect the upstream shader translation entry before preparing the translator')
# Linked translation initializes attributes read by the fragment stage, including unwritten vertex outputs.
code = code.replace(anchor, '''if (job["next"] is JsonObject next)
                {
                    var nextAcc = new Accessor(File.ReadAllBytes((string)next["code"]), next);
                    ctx.SetNextStage(Translator.CreateContext(0, nextAcc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags)));
                }
                ''' + anchor)
(target / 'Program.cs').write_text(code)
shutil.copy2(source / 'dmsh_xlate.csproj', target / 'dmsh_xlate.csproj')
PY
"$DOTNET_ROOT/dotnet" build work/tools/xlate-source/dmsh_xlate.csproj -c Release --disable-build-servers --maxcpucount:1 \
    --artifacts-path "$PWD/work/build/dotnet" -p:RyuSrc="$PWD/third_party/zelda-mobile/core/third_party/ryujinx" \
    > work/logs/shader-tools.log 2>&1
