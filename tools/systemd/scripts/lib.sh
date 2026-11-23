#!/usr/bin/env bash
set -euo pipefail

SD_SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SD_ROOT="$(cd -- "$SD_SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd -- "$SD_ROOT/../.." && pwd)"

SYSTEMD_SRC="${SYSTEMD_SRC:-$HOME/System/systemd}"
LINUX_SRC="${LINUX_SRC:-$HOME/System/linux}"
BUILD_DIR="${BUILD_DIR:-$SD_ROOT/build}"
CROSS_COMPILE="${CROSS_COMPILE:-riscv32-linux-gnu-}"
CROSS_SYSROOT="${CROSS_SYSROOT:-}"
EMULATOR="${EMULATOR:-$REPO_ROOT/emu/tern-linux}"
KERNEL_IMAGE="${KERNEL_IMAGE:-$LINUX_SRC/work/build/ternary-linux/guest/artifacts/Image}"
DTB="${DTB:-$BUILD_DIR/ternary-mmu.dtb}"
KERNEL_CONFIG="${KERNEL_CONFIG:-$LINUX_SRC/work/build/ternary-linux/guest/artifacts/config}"
MESON="${MESON:-meson}"
NINJA="${NINJA:-ninja}"
DTC="${DTC:-dtc}"

SD_BUILD="$BUILD_DIR/systemd"
SD_SYSROOT="$BUILD_DIR/sysroot"
SD_INITRD_ROOT="$BUILD_DIR/initrd-root"
SD_INITRAMFS="$BUILD_DIR/initramfs-systemd.cpio.gz"
SD_LOG="$BUILD_DIR/emulator.log"

log() {
  printf '[systemd-initrd] %s\n' "$*"
}

die() {
  printf '[systemd-initrd] ERROR: %s\n' "$*" >&2
  exit 1
}

require_file() {
  [[ -f "$1" ]] || die "missing file: $1"
}

require_dir() {
  [[ -d "$1" ]] || die "missing directory: $1"
}

require_tool() {
  command -v "$1" >/dev/null 2>&1 || die "missing tool: $1"
}

cross_flags() {
  local flags="-march=rv32ima -mabi=ilp32 -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64"
  if [[ -n "$CROSS_SYSROOT" ]]; then
    flags+=" --sysroot=$CROSS_SYSROOT"
  fi
  printf '%s' "$flags"
}

cross_cc() {
  printf '%s' "${CROSS_COMPILE}gcc"
}

sd_export_env() {
  export SYSTEMD_SRC LINUX_SRC BUILD_DIR CROSS_COMPILE CROSS_SYSROOT
  export EMULATOR KERNEL_IMAGE DTB KERNEL_CONFIG MESON NINJA DTC
  export SD_BUILD SD_SYSROOT SD_INITRD_ROOT SD_INITRAMFS SD_LOG
}
