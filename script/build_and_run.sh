#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export TMPDIR="$PWD/work/tmp/" PYTHONDONTWRITEBYTECODE=1
exec work/venv/bin/python tools/run.py "$@"
