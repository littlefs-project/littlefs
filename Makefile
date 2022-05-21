ifdef BUILDDIR
# make sure BUILDDIR ends with a slash
override BUILDDIR := $(BUILDDIR)/
# bit of a hack, but we want to make sure BUILDDIR directory structure
# is correct before any commands
$(if $(findstring n,$(MAKEFLAGS)),, $(shell mkdir -p \
	$(BUILDDIR) \
	$(BUILDDIR)bd \
	$(BUILDDIR)runners \
	$(BUILDDIR)tests))
endif

# overridable target/src/tools/flags/etc
ifneq ($(wildcard test.c main.c),)
TARGET ?= $(BUILDDIR)lfs
else
TARGET ?= $(BUILDDIR)lfs.a
endif


CC      ?= gcc
AR      ?= ar
SIZE    ?= size
CTAGS   ?= ctags
NM      ?= nm
OBJDUMP ?= objdump
LCOV    ?= lcov

SRC ?= $(filter-out $(wildcard *.*.c),$(wildcard *.c))
OBJ := $(SRC:%.c=$(BUILDDIR)%.o)
DEP := $(SRC:%.c=$(BUILDDIR)%.d)
ASM := $(SRC:%.c=$(BUILDDIR)%.s)
CGI := $(SRC:%.c=$(BUILDDIR)%.ci)
TAGCDA := $(SRC:%.c=$(BUILDDIR)%.t.a.gcda)

TESTS ?= $(wildcard tests/*.toml)
TEST_SRC ?= $(SRC) \
		$(filter-out $(wildcard bd/*.*.c),$(wildcard bd/*.c)) \
		runners/test_runner.c
TEST_TSRC := $(TESTS:%.toml=$(BUILDDIR)%.t.c) $(TEST_SRC:%.c=$(BUILDDIR)%.t.c)
TEST_TASRC := $(TEST_TSRC:%.t.c=%.t.a.c)
TEST_TAOBJ := $(TEST_TASRC:%.t.a.c=%.t.a.o)
TEST_TADEP := $(TEST_TASRC:%.t.a.c=%.t.a.d)
TEST_TAGCNO := $(TEST_TASRC:%.t.a.c=%.t.a.gcno)
TEST_TAGCDA := $(TEST_TASRC:%.t.a.c=%.t.a.gcda)

ifdef DEBUG
override CFLAGS += -O0
else
override CFLAGS += -Os
endif
ifdef TRACE
override CFLAGS += -DLFS_YES_TRACE
endif
override CFLAGS += -g3
override CFLAGS += -I.
override CFLAGS += -std=c99 -Wall -pedantic
override CFLAGS += -Wextra -Wshadow -Wjump-misses-init -Wundef
override CFLAGS += -ftrack-macro-expansion=0

override TESTFLAGS += -b
# forward -j flag
override TESTFLAGS += $(filter -j%,$(MAKEFLAGS))
ifdef VERBOSE
override TESTFLAGS     += -v
override CODEFLAGS     += -v
override DATAFLAGS     += -v
override STACKFLAGS    += -v
override STRUCTSFLAGS  += -v
override COVERAGEFLAGS += -v
override TESTFLAGS     += -v
override TESTCFLAGS    += -v
endif
ifdef EXEC
override TESTFLAGS 	   += --exec="$(EXEC)"
endif
ifdef BUILDDIR
override TESTFLAGS     += --build-dir="$(BUILDDIR:/=)"
override CODEFLAGS     += --build-dir="$(BUILDDIR:/=)"
override DATAFLAGS     += --build-dir="$(BUILDDIR:/=)"
override STACKFLAGS    += --build-dir="$(BUILDDIR:/=)"
override STRUCTSFLAGS  += --build-dir="$(BUILDDIR:/=)"
override COVERAGEFLAGS += --build-dir="$(BUILDDIR:/=)"
endif
ifneq ($(NM),nm)
override CODEFLAGS += --nm-tool="$(NM)"
override DATAFLAGS += --nm-tool="$(NM)"
endif
ifneq ($(OBJDUMP),objdump)
override STRUCTSFLAGS += --objdump-tool="$(OBJDUMP)"
endif


# commands
.PHONY: all build
all build: $(TARGET)

.PHONY: asm
asm: $(ASM)

.PHONY: size
size: $(OBJ)
	$(SIZE) -t $^

.PHONY: tags
tags:
	$(CTAGS) --totals --c-types=+p $(shell find -H -name '*.h') $(SRC)

.PHONY: test-runner
test-runner: override CFLAGS+=--coverage
test-runner: $(BUILDDIR)runners/test_runner

.PHONY: test
test: test-runner
	rm -f $(TEST_TAGCDA)
	./scripts/test.py --runner=$(BUILDDIR)runners/test_runner $(TESTFLAGS)

.PHONY: test-list
test-list: test-runner
	./scripts/test.py --runner=$(BUILDDIR)runners/test_runner $(TESTFLAGS) -l

.PHONY: code
code: $(OBJ)
	./scripts/code.py $^ -S $(CODEFLAGS)

.PHONY: data
data: $(OBJ)
	./scripts/data.py $^ -S $(DATAFLAGS)

.PHONY: stack
stack: $(CGI)
	./scripts/stack.py $^ -S $(STACKFLAGS)

.PHONY: structs
structs: $(OBJ)
	./scripts/structs.py $^ -S $(STRUCTSFLAGS)

.PHONY: coverage
coverage: $(TAGCDA)
	./scripts/coverage.py $^ -s $(COVERAGEFLAGS)

.PHONY: summary
summary: $(BUILDDIR)lfs.csv
	./scripts/summary.py -Y $^ $(SUMMARYFLAGS)


# rules
-include $(DEP)
-include $(TEST_TADEP)
.SUFFIXES:
.SECONDARY:

$(BUILDDIR)lfs: $(OBJ)
	$(CC) $(CFLAGS) $^ $(LFLAGS) -o $@

$(BUILDDIR)lfs.a: $(OBJ)
	$(AR) rcs $@ $^

$(BUILDDIR)lfs.csv: $(OBJ) $(CGI)
	./scripts/code.py $(OBJ) -q $(CODEFLAGS) -o $@
	./scripts/data.py $(OBJ) -q -m $@ $(DATAFLAGS) -o $@
	./scripts/stack.py $(CGI) -q -m $@ $(STACKFLAGS) -o $@
	./scripts/structs.py $(OBJ) -q -m $@ $(STRUCTSFLAGS) -o $@

$(BUILDDIR)runners/test_runner: $(TEST_TAOBJ)
	$(CC) $(CFLAGS) $^ $(LFLAGS) -o $@

# our main build rule generates .o, .d, and .ci files, the latter
# used for stack analysis
$(BUILDDIR)%.o $(BUILDDIR)%.ci: %.c
	$(CC) -c -MMD -fcallgraph-info=su $(CFLAGS) $< -o $(BUILDDIR)$*.o

$(BUILDDIR)%.s: %.c
	$(CC) -S $(CFLAGS) $< -o $@

$(BUILDDIR)%.a.c: %.c
	./scripts/explode_asserts.py $< -o $@

$(BUILDDIR)%.a.c: $(BUILDDIR)%.c
	./scripts/explode_asserts.py $< -o $@

$(BUILDDIR)%.t.c: %.toml
	./scripts/test.py -c $< $(TESTCFLAGS) -o $@

$(BUILDDIR)%.t.c: %.c $(TESTS)
	./scripts/test.py -c $(TESTS) -s $< $(TESTCFLAGS) -o $@

# clean everything
.PHONY: clean
clean:
	rm -f $(BUILDDIR)lfs
	rm -f $(BUILDDIR)lfs.a
	rm -f $(BUILDDIR)lfs.csv
	rm -f $(BUILDDIR)runners/test_runner
	rm -f $(OBJ)
	rm -f $(DEP)
	rm -f $(ASM)
	rm -f $(CGI)
	rm -f $(TEST_TSRC)
	rm -f $(TEST_TASRC)
	rm -f $(TEST_TAOBJ)
	rm -f $(TEST_TADEP)
	rm -f $(TEST_TAGCNO)
	rm -f $(TEST_TAGCDA)
