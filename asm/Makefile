# Makefile to help with assembly

OUTDIR=out
SRCDIR=examples
#PP=yamp.py
PP=gcc -E -xc
PPFLAGS=--include assembler.inc --include aliases.ymp
MKDIR=@mkdir
PYTHON=python3
EMUDIR=../emu/
EMUEXE=tern-cpu-emu
EMU=$(EMUDIR)$(EMUEXE)
EMUDEPS=$(wildcard $(EMUDIR)*.c) $(wildcard $(EMUDIR)*.h)
TASFILES=$(wildcard $(SRCDIR)/*.tas)
TRTFILES=$(TASFILES:$(SRCDIR)/%.tas=$(OUTDIR)/%.trt)

default: test

$(OUTDIR):
	$(MKDIR) $(OUTDIR)

# Rule to assemble Ternary Assembly into loadable Trit files
$(OUTDIR)/%.trt: $(SRCDIR)/%.tas assembler.inc aliases.ymp | $(OUTDIR)
	$(PP) $(PPFLAGS) $< -o $@

# Rule to run a Trit file in the emulator
%: $(OUTDIR)/%.trt $(EMU)
	$(EMU) $<

all: $(TRTFILES)

test: all $(EMU)
	$(PYTHON) tests/smoke.py

clean:
	rm -rf $(OUTDIR)

clean-all: clean

$(EMU): $(EMUDEPS)
	$(MAKE) -C $(EMUDIR) $(EMUEXE)

.PRECIOUS: $(OUTDIR)/%.trt

.PHONY: default all test clean clean-all
