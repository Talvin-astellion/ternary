#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

MODE="${1:-report}"

if [[ "$MODE" == manifest ]]; then
  MANIFEST="$(manifest_path)"
  require_file "$MANIFEST"
  require_file "$EMULATOR"
  require_file "$KERNEL_IMAGE"
  require_file "$DTB"
  require_file "$SD_INITRAMFS"

  manifest_field() {
    sed -n "s/^$1=//p" "$MANIFEST" | head -n 1
  }

  verify_manifest_value() {
    local key="$1" current="$2" expected

    expected="$(manifest_field "$key")"
    [[ -n "$expected" ]] || die "manifest missing $key in $MANIFEST"
    if [[ "$expected" != "$current" ]]; then
      die "manifest mismatch for $key (recorded: $expected, current: $current)"
    fi
    log "OK: manifest $key"
  }

  verify_manifest_sha() {
    verify_manifest_value "$1_sha256" "$(sha256_of "$2")"
  }

  if [[ "$(manifest_field run_verified)" != 1 ]]; then
    die "manifest does not describe a successful run: $MANIFEST"
  fi

  verify_manifest_sha emulator "$EMULATOR"
  verify_manifest_sha kernel_image "$KERNEL_IMAGE"
  verify_manifest_sha dtb "$DTB"
  verify_manifest_sha initrd "$SD_INITRAMFS"
  verify_manifest_value kernel_release "$(kernel_release)"
  if command -v "$(cross_cc)" >/dev/null 2>&1; then
    verify_manifest_value cross_cc_version "$(cross_cc_version)"
  else
    log "SKIP: manifest cross_cc_version (compiler not on PATH)"
  fi

  log "MANIFEST_OK"
  exit 0
fi

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
if [[ -f "$DTB" ]]; then
  log "OK: emulator DTB"
else
  log "MISSING: emulator DTB ($DTB); run make -C tools/systemd dtb"
  missing=1
fi

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
