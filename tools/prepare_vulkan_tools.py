"""Build the pinned Windows shader translator using workspace-local dependencies."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile

from bounded import run
from recompile import ROOT, prepare_work

SDK_VERSION = '10.0.401'
SDK_URL = f'https://builds.dotnet.microsoft.com/dotnet/Sdk/{SDK_VERSION}/dotnet-sdk-{SDK_VERSION}-win-x64.zip'
SDK_SHA512 = '24b670ad3d923bfcf47df6c3b034152398b42f6dbc388e10d783aee1cfb5e5817d399fc0ae2a12cfa822a55e61d34830ccb15c50ef6efee437ab874bb7c79430'


def prepare():
    if os.name != 'nt': raise ValueError('this bootstrap targets Windows x86-64')
    prepare_work()
    target, archive = ROOT / 'work/tools/dotnet-win', ROOT / 'work/tmp/dotnet-sdk-win-x64.zip'
    if target.is_symlink() or archive.is_symlink(): raise ValueError('tool outputs must not be symlinks')
    if not (target / 'dotnet.exe').is_file():
        valid = False
        if archive.is_file():
            with archive.open('rb') as stream: valid = hashlib.file_digest(stream, 'sha512').hexdigest() == SDK_SHA512
        if not valid:
            digest, total, deadline = hashlib.sha512(), 0, time.monotonic() + 600
            with urllib.request.urlopen(SDK_URL, timeout=30) as response, archive.open('wb') as output:
                while chunk := response.read(1 << 20):
                    total += len(chunk)
                    if total > 512 << 20 or time.monotonic() > deadline or shutil.disk_usage(ROOT).free < 20 << 30:
                        raise RuntimeError('SDK download resource budget exceeded')
                    digest.update(chunk); output.write(chunk)
            if digest.hexdigest() != SDK_SHA512: raise ValueError('SDK SHA-512 mismatch')
        with zipfile.ZipFile(archive) as source:
            if sum(i.file_size for i in source.infolist()) > 2 << 30: raise ValueError('SDK expanded size exceeds bound')
            for item in source.infolist():
                if not (target / item.filename).resolve().is_relative_to(target.resolve()): raise ValueError('SDK archive path traversal')
            source.extractall(target)
    dotnet = target / 'dotnet.exe'
    os.environ.update(DOTNET_ROOT=str(target), DOTNET_CLI_HOME=str(ROOT / 'work/tmp/dotnet-home'),
                      NUGET_PACKAGES=str(ROOT / 'work/tmp/nuget'), APPDATA=str(ROOT / 'work/tmp/dotnet-appdata'),
                      LOCALAPPDATA=str(ROOT / 'work/tmp/dotnet-localappdata'), DOTNET_CLI_TELEMETRY_OPTOUT='1',
                      DOTNET_ADD_GLOBAL_TOOLS_TO_PATH='false', DOTNET_NOLOGO='1')
    if subprocess.check_output([str(dotnet), '--version'], timeout=10, text=True).strip() != SDK_VERSION:
        raise ValueError('unexpected local .NET SDK version')
    packages = run([sys.executable, str(ROOT / 'tools/fetch_vulkan_packages.py')], cwd=ROOT,
                   log=ROOT / 'work/logs/vulkan-packages.log', seconds=600)
    build = run([str(dotnet), 'build', str(ROOT / 'tools/vulkan_xlate/dmsh_xlate.csproj'), '-c', 'Release',
                 '--disable-build-servers', '--maxcpucount:1', '--artifacts-path', str(ROOT / 'work/build/vulkan-xlate'),
                 '-p:RyuSrc=' + str(ROOT / 'third_party/zelda-mobile/core/third_party/ryujinx'),
                 '-p:RestoreConfigFile=' + str(ROOT / 'tools/vulkan_xlate/NuGet.Config'),
                 '-p:RestoreSources=' + str(ROOT / 'work/tmp/nuget-feed'), '-p:GeneratePackageOnBuild=false', '-p:NuGetAudit=false'],
                cwd=ROOT, log=ROOT / 'work/logs/vulkan-xlate-build.log', seconds=300)
    record = dict(sdk_version=SDK_VERSION, sdk_url=SDK_URL, sdk_archive_sha512=SDK_SHA512, packages=packages, build=build)
    (ROOT / 'work/build/vulkan-xlate/preparation.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
    return record


if __name__ == '__main__':
    try: print(json.dumps(prepare(), indent=2))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error: raise SystemExit(str(error))
