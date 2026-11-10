#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
library=${1:?V2 library required}
nm -D --defined-only "$library" | grep -Eq ' [TW] infiltrator_filesystem_api_v2$'
if nm -D --defined-only "$library" | grep -Eq 'infiltrator_filesystem_.*_v1$|infiltrator_filesystem_module_v[0-9]+$'; then
  echo 'EXT2 runtime still exports an obsolete V1 module' >&2
  exit 1
fi
