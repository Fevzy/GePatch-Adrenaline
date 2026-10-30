#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
export PSPDEV="${PSPDEV:-/home/fevzy/pspdev}"
export PATH="$PSPDEV/bin:$PATH"
command -v psp-gcc >/dev/null
command -v psp-config >/dev/null
make -B -j"${GP_JOBS:-2}" GP_TRACE="${GP_TRACE:-0}" GP_LOG="${GP_LOG:-0}" GP_PROFILE="${GP_PROFILE:-0}" 2>&1 | tee build_psp.log
test -s ge_patch.prx
sha256sum ge_patch.prx
