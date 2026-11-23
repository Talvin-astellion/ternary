#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

require_file "$KERNEL_IMAGE"
require_file "$DTB"
require_file "$SD_INITRAMFS"
require_file "$EMULATOR"

RUN_TIMEOUT="${RUN_TIMEOUT:-900}"
MAX_INSN="${MAX_INSN:-20000000000}"
TIME_MODE="${TIME_MODE:-inst}"
KERNEL_ADDR="${KERNEL_ADDR:-0x80000000}"

EMU_ARGS=(
  --kernel "$KERNEL_IMAGE"
  --kernel-addr "$KERNEL_ADDR"
  --dtb "$DTB"
  --initrd "$SD_INITRAMFS"
  --max-insn "$MAX_INSN"
  --quiet
)
if [[ "$TIME_MODE" == inst ]]; then
  EMU_ARGS+=(--time-inst)
elif [[ "$TIME_MODE" != host ]]; then
  die "TIME_MODE must be 'host' or 'inst'"
fi

mkdir -p "$BUILD_DIR"
log "booting systemd initrd under $EMULATOR"
set +e
timeout "$RUN_TIMEOUT" "$EMULATOR" "${EMU_ARGS[@]}" > "$SD_LOG" 2>&1
rc=$?
set -e

for marker in 'Linux version' 'SYSTEMD_OK'; do
  grep -q "$marker" "$SD_LOG" ||
    die "marker not found: $marker (log: $SD_LOG)"
done

log "systemd PID 1 OK: $SD_LOG (emulator rc=$rc)"
