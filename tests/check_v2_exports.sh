#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
library=${1:?V2 library required}
# Reject *all* unrelated public ELF definitions, not only names ending
# in "_v1". The native engine is private to this filesystem module.
symbols=$(nm -D --defined-only --format=posix "$library" | awk '{print $1}')
if [ "$symbols" != 'infiltrator_filesystem_api_v2' ]; then
  printf 'Expected only Common V2 export, found: %s\n' "$symbols" >&2
  exit 1
fi
readelf -p .ifsmeta "$library" | grep -Fq 'IFS-MODULE:ext2:common2' ||
  { echo 'Missing Common V2 EXT2 identity' >&2; exit 1; }
