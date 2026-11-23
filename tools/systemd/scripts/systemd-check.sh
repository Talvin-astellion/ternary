#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

MODE="${1:-report}"
missing=0

check_path() {
  local label="$1"
  local path="$2"
  local kind="$3"

  if [[ "$kind" == dir ]]; then
    if [[ -d "$path" ]]; then
      log "OK: $label"
    else
      log "MISSING: $label ($path)"
      missing=1
    fi
  else
    if [[ -f "$path" ]]; then
      log "OK: $label"
    else
      log "MISSING: $label ($path)"
      missing=1
    fi
  fi
}

check_tool() {
  local label="$1"
  local tool="$2"

  if command -v "$tool" >/dev/null 2>&1; then
    log "OK: $label"
  else
    log "MISSING: $label ($tool)"
    missing=1
  fi
}

check_path "systemd source" "$SYSTEMD_SRC/meson.build" file
check_path "systemd PID 1 source" "$SYSTEMD_SRC/src/core/main.c" file
check_tool "meson" "$MESON"
check_tool "ninja" "$NINJA"
check_tool "cpio" cpio
check_tool "gzip" gzip
check_tool "awk" awk

for tool in gcc ar objcopy strip; do
  check_tool "cross $tool" "${CROSS_COMPILE}${tool}"
done

tmp_c="$(mktemp --suffix=.c /tmp/opencode/systemd-cross-XXXXXX)"
printf 'int main(void) { return 0; }\n' > "$tmp_c"
if "$(cross_cc)" $(cross_flags) -c "$tmp_c" -o /dev/null >/dev/null 2>&1; then
  log "OK: cross compiler supports rv32ima/ilp32 and 64-bit time_t"
else
  log "MISSING: cross compiler cannot build rv32ima/ilp32 with 64-bit time_t"
  missing=1
fi
rm -f "$tmp_c"

check_path "kernel Image" "$KERNEL_IMAGE" file
check_path "emulator DTB" "$DTB" file

if [[ -f "$KERNEL_CONFIG" ]]; then
  for option in CONFIG_MMU=y CONFIG_BLK_DEV_INITRD=y CONFIG_RD_GZIP=y \
                CONFIG_CGROUPS=y CONFIG_PROC_FS=y CONFIG_SYSFS=y; do
    if grep -q "^$option\$" "$KERNEL_CONFIG"; then
      log "OK: kernel $option"
    else
      log "MISSING: kernel $option in $KERNEL_CONFIG"
      missing=1
    fi
  done
else
  log "MISSING: kernel config ($KERNEL_CONFIG)"
  missing=1
fi

if [[ -x "$EMULATOR" ]]; then
  log "OK: emulator $EMULATOR"
else
  log "MISSING: executable emulator ($EMULATOR); run make -C emu linux"
  missing=1
fi

if (( missing )); then
  if [[ "$MODE" == require ]]; then
    die "prerequisites are missing; see docs/systemd-initrd.md"
  fi
  log "READY=0"
  exit 0
fi

log "READY=1"
