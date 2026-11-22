# Quick Makefile

PKG=tern-cpu-emu
GUEST=tern-linux
SRCEXT=.c
SRCS=$(filter-out rv32.c,$(wildcard *$(SRCEXT)))
STRIP=strip
CC=gcc
CFLAGS=-Wall -Ofast
LIBS=
#PREFIX=/usr/local
#CONFIGDIR=/etc
DEPDIR=deps
DEPS=$(SRCS:%$(SRCEXT)=$(DEPDIR)/%.d)
OBJDIR=obj
OBJS=$(SRCS:%$(SRCEXT)=$(OBJDIR)/%.o)
ASMDIR=asm
ASMS=$(SRCS:%$(SRCEXT)=$(ASMDIR)/%.s)
DOCDIR=doc
DOCCMD=doxygen
DOCCFG=Doxyfile
SED=sed
MKDIR=@mkdir

$(PKG): $(OBJS)
	@echo "Generating $@"
	$(CC) $(LIBS) $(CFLAGS) $(OBJS) -o $(PKG)
	$(STRIP) $(PKG)

pkg: $(PKG) $(GUEST)

linux: $(GUEST)

$(GUEST): rv32.c
	@echo "Generating $@"
	$(CC) $(CFLAGS) rv32.c -o $@
	$(STRIP) $@


tern-test: ternary.c ternary.h
	@echo "Generating $@"
	$(CC) $(LIBS) $(CFLAGS) ternary.c -DTERNARY_MATH_MAIN=1 -o $@
	$(STRIP) $@


$(OBJDIR):
	$(MKDIR) $(OBJDIR)

$(OBJDIR)/%.o: %$(SRCEXT) | $(OBJDIR)
	@echo "CC $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(ASMDIR):
	$(MKDIR) $(ASMDIR)

$(ASMDIR)/%.s: %$(SRCEXT) | $(ASMDIR)
	$(CC) $(CFLAGS) -S -fverbose-asm -c $< -o $@

$(DEPDIR):
	$(MKDIR) $(DEPDIR)

$(DEPDIR)/%.d: %$(SRCEXT) | $(DEPDIR)
	$(CC) $(CFLAGS) -c $< -MM -o $@
	$(SED) -i 's+\($*\)\.o[ :]*+$(OBJDIR)/\1.o $@ : +g' $@

asms: $(ASMS)


CORE_SRCS=cpu.c mem.c disassem.c ternary.c tml.c tml_log.c tml_policy.c sha256.c trace.c taint.c symbolic.c
TESTBIN=test-core
TMLTESTBIN=test-tml
PHASE2TESTBIN=test-phase2
TEST_SRCS=tests/test_core.c $(CORE_SRCS)
TML_TEST_SRCS=tests/test_tml.c $(CORE_SRCS)
PHASE2_TEST_SRCS=tests/test_phase2.c $(CORE_SRCS)
BENCH_SRCS=tests/bench_cpu.c $(CORE_SRCS)
BENCH_HEADERS=$(wildcard *.h)
BENCH_CYCLES?=1000000

bench-cpu: $(BENCH_SRCS) $(BENCH_HEADERS)
	$(CC) $(CFLAGS) $(BENCH_SRCS) -o $@

bench-cpu-uncached: $(BENCH_SRCS) $(BENCH_HEADERS)
	$(CC) $(CFLAGS) -DTERNARY_DECODE_CACHE=0 $(BENCH_SRCS) -o $@

.PHONY: bench
bench: bench-cpu bench-cpu-uncached
	@echo "Decode cache disabled:"
	./bench-cpu-uncached $(BENCH_CYCLES)
	@echo "Decode cache enabled:"
	./bench-cpu $(BENCH_CYCLES)

$(TESTBIN): $(TEST_SRCS) cpu.h mem.h ternary.h disassem.h tests/test_core.c
	@echo "Generating $@"
	$(CC) $(CFLAGS) $(TEST_SRCS) -o $@

$(TMLTESTBIN): $(TML_TEST_SRCS) cpu.h mem.h ternary.h tml.h tml_policy.h tests/test_tml.c
	@echo "Generating $@"
	$(CC) $(CFLAGS) $(TML_TEST_SRCS) -o $@

$(PHASE2TESTBIN): $(PHASE2_TEST_SRCS) cpu.h mem.h trace.h taint.h symbolic.h tests/test_phase2.c
	@echo "Generating $@"
	$(CC) $(CFLAGS) $(PHASE2_TEST_SRCS) -o $@

test: $(TESTBIN) $(TMLTESTBIN) $(PHASE2TESTBIN)
	./$(TESTBIN)
	./$(TMLTESTBIN)
	./$(PHASE2TESTBIN)

sanitize:
	$(CC) -g -O1 -Wall -Wextra -fsanitize=address,undefined \
		$(TEST_SRCS) -o $(TESTBIN)-san
	$(CC) -g -O1 -Wall -Wextra -fsanitize=address,undefined \
		$(TML_TEST_SRCS) -o $(TMLTESTBIN)-san
	$(CC) -g -O1 -Wall -Wextra -fsanitize=address,undefined \
		$(PHASE2_TEST_SRCS) -o $(PHASE2TESTBIN)-san
	./$(TESTBIN)-san
	./$(TMLTESTBIN)-san
	./$(PHASE2TESTBIN)-san

#install: $(PKG)
#	cp $(PKG) $(PREFIX)/bin/
#	cp $(PKG)rc $(CONFIGDIR)/

clean-asm:
	rm -rf $(ASMDIR)

clean-obj:
	rm -rf $(OBJDIR)

clean-dep:
	rm -rf $(DEPDIR)

clean: clean-obj clean-asm
	rm -rf *~

clean-pkg: clean clean-dep
	rm -rf $(PKG) $(GUEST) tern-test $(TESTBIN) $(TESTBIN)-san $(TMLTESTBIN) $(TMLTESTBIN)-san $(PHASE2TESTBIN) $(PHASE2TESTBIN)-san bench-cpu bench-cpu-uncached

rebuild: clean-pkg pkg

$(DOCCFG):
	$(DOCCMD) -g $(DOCCFG)
	$(SED) -i 's+^OUTPUT_DIRECTORY *=.*$$+OUTPUT_DIRECTORY       = $(DOCDIR)+g' $(DOCCFG)
	$(SED) -i 's+^PROJECT_NAME *=.*$$+PROJECT_NAME           = "$(PKG)"+g' $(DOCCFG)
	$(SED) -i 's+^GENERATE_LATEX *=.*$$+GENERATE_LATEX         = NO+g' $(DOCCFG)
	$(SED) -i 's+^SOURCE_BROWSER *=.*$$+SOURCE_BROWSER         = YES+g' $(DOCCFG)
	$(SED) -i 's+^GENERATE_TREEVIEW *=.*$$+GENERATE_TREEVIEW      = YES+g' $(DOCCFG)
#	$(SED) -i 's+^PLANTUML_JAR_PATH *=.*$$+PLANTUML_JAR_PATH      = /usr/share/plantuml/plantuml.jar+g' $(DOCCFG)

docs: $(DOCCFG)
	$(DOCCMD) $(DOCCFG)

clean-docs:
	rm -rf $(DOCDIR)

rebuild-docs: clean-docs docs

all: $(PKG) docs

clean-all: clean-pkg clean-docs

rebuild-all: clean-all all

include $(DEPS)

#gcc -Wall -Os -fgcse-after-reload -fipa-cp-clone -floop-interchange -floop-unroll-and-jam -fpeel-loops -fpredictive-commoning -fsplit-loops -fsplit-paths -ftree-loop-distribution -ftree-loop-vectorize -ftree-partial-pre -ftree-slp-vectorize -funswitch-loops -fvect-cost-model -fvect-cost-model=dynamic -fversion-loops-for-strides main.c

#gcc -Wall -Os -fassociative-math -fcx-limited-range -ffinite-math-only -fgcse-after-reload -fipa-cp-clone -floop-interchange -floop-unroll-and-jam -foptimize-strlen -fpeel-loops -fpredictive-commoning -freciprocal-math -fsplit-loops -fsplit-paths -ftree-loop-distribute-patterns -ftree-loop-distribution -ftree-loop-vectorize -ftree-partial-pre -ftree-slp-vectorize -funsafe-math-optimizations -funswitch-loops -fno-math-errno -fno-signed-zeros -fno-trapping-math -fvect-cost-model=dynamic main.c

$(V).SILENT:
