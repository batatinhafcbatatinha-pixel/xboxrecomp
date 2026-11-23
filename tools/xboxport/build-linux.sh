#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
exec "${PYTHON:-python3}" "$here/xboxport.py" all "$@"
