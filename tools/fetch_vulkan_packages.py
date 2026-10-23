"""Fetch pinned translator package dependencies into the workspace local feed.

Uses Python HTTPS because restricted Windows .NET processes may lack Schannel
credentials. NuGet still verifies signed packages when restoring from this feed.
Run through tools/bounded.py; no global package or user configuration is changed.
"""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import io
import json
from pathlib import Path
import re
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

root = Path(__file__).resolve().parents[1]
feed = root / 'work/tmp/nuget-feed'
feed.mkdir(exist_ok=True)
vendor = root / 'third_party/zelda-mobile/core/third_party/ryujinx'
versions = {p.attrib['Include'].lower(): p.attrib['Version'] for p in ET.parse(vendor / 'Directory.Packages.props').iter('PackageVersion')}
projects = ['Ryujinx.Common', 'Ryujinx.Graphics.Shader', 'Spv.Generator']
pending = {(p.attrib['Include'].lower(), versions[p.attrib['Include'].lower()]) for project in projects
           for p in ET.parse(vendor / 'src' / project / (project + '.csproj')).iter('PackageReference')}
seen = {}

def priority(framework):
    framework = framework.lower().replace('.netstandard', 'netstandard').replace('.netcoreapp', 'netcoreapp')
    ranks = ['net10.0', 'net9.0', 'net8.0', 'net7.0', 'net6.0', 'net5.0', 'netcoreapp3.1', 'netstandard2.1', 'netstandard2.0', 'netstandard1.6', 'netstandard1.3', 'netstandard1.1', 'netstandard1.0', '']
    return ranks.index(framework) if framework in ranks else 999

def fetch(item):
    name, version = item
    path = feed / f'{name}.{version}.nupkg'
    url = f'https://api.nuget.org/v3-flatcontainer/{name}/{version}/{name}.{version}.nupkg'
    if path.is_file(): data = path.read_bytes()
    else:
        with urllib.request.urlopen(url, timeout=30) as response: data = response.read((32 << 20) + 1)
        if len(data) > (32 << 20): raise RuntimeError('package size limit')
        path.write_bytes(data)
    with zipfile.ZipFile(io.BytesIO(data)) as package:
        names = [n for n in package.namelist() if n.endswith('.nuspec')]
        if len(names) != 1: raise RuntimeError('ambiguous package metadata')
        doc = ET.fromstring(package.read(names[0]))
    # Ignore XML namespace version differences.
    for elem in doc.iter(): elem.tag = elem.tag.rsplit('}', 1)[-1]
    deps = doc.find('./metadata/dependencies')
    result = []
    if deps is not None:
        groups = list(deps.findall('group'))
        if groups:
            group = min(groups, key=lambda g: priority(g.get('targetFramework', '')))
            if priority(group.get('targetFramework', '')) == 999: raise RuntimeError(f'no compatible group {name}: {[g.attrib for g in groups]}')
            deps = group
        for dep in deps.findall('dependency'):
            ver = dep.attrib['version'].strip('[]()').split(',')[0].strip()
            if not re.fullmatch(r'[0-9a-zA-Z.+-]+', ver): raise RuntimeError('unsupported version constraint')
            result.append((dep.attrib['id'].lower(), ver.lower()))
    return item, result, {'url': url, 'sha512': hashlib.sha512(data).hexdigest(), 'bytes': len(data)}

with ThreadPoolExecutor(max_workers=8) as executor:
    while pending:
        batch = pending - seen.keys()
        pending = set()
        if len(seen) + len(batch) > 256: raise RuntimeError('dependency bound')
        for item, deps, info in executor.map(fetch, sorted(batch)):
            seen[item] = info
            pending.update(deps)
        print('packages', len(seen), 'next', len(pending - seen.keys()), flush=True)
(feed / 'download-record.json').write_text(json.dumps({f'{k[0]}/{k[1]}': v for k, v in seen.items()}, indent=2))
