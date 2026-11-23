#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
python="${PYTHON:-python3}"
if [ "$(uname -s)" = Darwin ]; then
  if [ "$(uname -m)" != x86_64 ]; then
    exec arch -x86_64 /bin/bash "$0" "$@"
  fi
  python="${PYTHON:-/usr/local/bin/python3}"
fi
exec "$python" "$here/../../tools/posix/profile.py" play --root "$here" "$@"
