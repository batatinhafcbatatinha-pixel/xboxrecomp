#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
if [ "$(uname -m)" != x86_64 ]; then
  exec arch -x86_64 /bin/bash "$0" "$@"
fi
exec "${PYTHON:-/usr/local/bin/python3}" "$here/../../tools/posix/profile.py" build --root "$here" "$@"
