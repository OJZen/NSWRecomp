#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p work/tools work/tmp work/logs
export TMPDIR="$PWD/work/tmp/"
export PYTHONDONTWRITEBYTECODE=1
py="$PWD/work/venv/bin/python"
if ! "$py" - <<'PY'
from pathlib import Path
import hashlib, json
try:
    info=json.loads(Path('work/tools/hactool.json').read_text())
    assert info['commit']=='1d64a83450e025622f3468c28fc4164dad2c5ef6'
    assert info['patch_sha256']==hashlib.sha256(Path('tools/hactool.patch').read_bytes()).hexdigest()
    assert info['binary_sha256']==hashlib.sha256(Path('work/tools/hactool').read_bytes()).hexdigest()
except (OSError, ValueError, KeyError, AssertionError):
    raise SystemExit(1)
PY
then
    source_dir=$(mktemp -d "$PWD/work/tmp/hactool.XXXXXX")
    trap 'rm -rf "$source_dir"' EXIT
    git init --quiet "$source_dir"
    git -C "$source_dir" fetch --quiet --depth 1 https://github.com/SciresM/hactool.git 1d64a83450e025622f3468c28fc4164dad2c5ef6
    git -C "$source_dir" checkout --quiet 1d64a83450e025622f3468c28fc4164dad2c5ef6
    git -C "$source_dir" apply "$PWD/tools/hactool.patch"
    cp "$source_dir/config.mk.template" "$source_dir/config.mk"
    make -C "$source_dir" -j1 CC=clang > work/logs/hactool-build.log 2>&1
    "$py" - "$source_dir" <<'PY'
from pathlib import Path
import hashlib, json, shutil, sys
source=Path(sys.argv[1]); target=Path('work/tools')
for source_name,target_name in [('hactool','hactool'),('LICENSE','hactool.LICENSE'),
                                ('mbedtls/LICENSE','mbedtls.LICENSE'),('mbedtls/apache-2.0.txt','mbedtls.apache-2.0.txt')]:
    shutil.copy2(source/source_name,target/target_name)
info={'upstream':'https://github.com/SciresM/hactool','commit':'1d64a83450e025622f3468c28fc4164dad2c5ef6',
      'patch_sha256':hashlib.sha256(Path('tools/hactool.patch').read_bytes()).hexdigest(),
      'binary_sha256':hashlib.sha256((target/'hactool').read_bytes()).hexdigest()}
(target/'hactool.json').write_text(json.dumps(info,indent=2)+'\n')
PY
    rm -rf "$source_dir"
    trap - EXIT
    rm -f work/logs/hactool-build.log
fi
