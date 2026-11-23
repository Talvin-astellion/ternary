SD_TOOLS := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
SCRIPTS := $(SD_TOOLS)scripts

.PHONY: help check verify dtb build initrd run all clean

help:
	@echo "Targets:"
	@echo "  check   report systemd-initrd prerequisites (non-blocking)"
	@echo "  verify  fail if prerequisites are missing"
	@echo "  dtb     build the Sv32 DTB used by the emulator"
	@echo "  build   cross-build systemd from ~/System/systemd"
	@echo "  initrd  assemble systemd initramfs with PID 1 smoke unit"
	@echo "  run     boot the initramfs under emu/tern-linux"
	@echo "  all     build + initrd + run"

check:
	bash "$(SCRIPTS)/systemd-check.sh" report

verify:
	bash "$(SCRIPTS)/systemd-check.sh" require

dtb:
	bash "$(SCRIPTS)/systemd-dtb.sh"

build: dtb
	bash "$(SCRIPTS)/systemd-build.sh"

initrd:
	bash "$(SCRIPTS)/systemd-initrd.sh"

run:
	bash "$(SCRIPTS)/systemd-run.sh"

all: build initrd run

clean:
	rm -rf "$(SD_TOOLS)build"
