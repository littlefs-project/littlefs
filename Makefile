ifdef BUILDDIR
# bit of a hack, but we want to make sure BUILDDIR directory structure
# is correct before any commands
$(if $(findstring n,$(MAKEFLAGS)),, $(shell mkdir -p \
	$(BUILDDIR)/ \
	$(BUILDDIR)/bd \
	$(BUILDDIR)/runners \
	$(BUILDDIR)/tests \
	$(BUILDDIR)/benches))
endif
BUILDDIR ?= .

# default to building a library
ifneq ($(wildcard test.c main.c),)
TARGET ?= $(BUILDDIR)/lfs
else
TARGET ?= $(BUILDDIR)/liblfs.a
endif

# find source files
SRC  ?= $(filter-out $(wildcard *.t.* *.b.*),$(wildcard *.c))
OBJ  := $(SRC:%.c=$(BUILDDIR)/%.o)
DEP  := $(SRC:%.c=$(BUILDDIR)/%.d)
ASM  := $(SRC:%.c=$(BUILDDIR)/%.s)
CI   := $(SRC:%.c=$(BUILDDIR)/%.ci)
GCDA := $(SRC:%.c=$(BUILDDIR)/%.t.a.gcda)

TESTS ?= $(wildcard tests/*.toml)
TEST_SRC ?= $(SRC) \
		$(filter-out $(wildcard bd/*.t.* bd/*.b.*),$(wildcard bd/*.c)) \
		runners/test_runner.c
TEST_RUNNER ?= $(BUILDDIR)/runners/test_runner
TEST_C     := $(TESTS:%.toml=$(BUILDDIR)/%.t.c) \
		$(TEST_SRC:%.c=$(BUILDDIR)/%.t.c)
TEST_A     := $(TEST_C:%.t.c=%.t.a.c)
TEST_OBJ   := $(TEST_A:%.t.a.c=%.t.a.o)
TEST_DEP   := $(TEST_A:%.t.a.c=%.t.a.d)
TEST_CI	   := $(TEST_A:%.t.a.c=%.t.a.ci)
TEST_GCNO  := $(TEST_A:%.t.a.c=%.t.a.gcno)
TEST_GCDA  := $(TEST_A:%.t.a.c=%.t.a.gcda)
TEST_PERF  := $(TEST_RUNNER:%=%.perf)
TEST_TRACE := $(TEST_RUNNER:%=%.trace)
TEST_CSV   := $(TEST_RUNNER:%=%.csv)

BENCHES ?= $(wildcard benches/*.toml)
BENCH_SRC ?= $(SRC) \
		$(filter-out $(wildcard bd/*.t.* bd/*.b.*),$(wildcard bd/*.c)) \
		runners/bench_runner.c
BENCH_RUNNER ?= $(BUILDDIR)/runners/bench_runner
BENCH_C     := $(BENCHES:%.toml=$(BUILDDIR)/%.b.c) \
		$(BENCH_SRC:%.c=$(BUILDDIR)/%.b.c)
BENCH_A     := $(BENCH_C:%.b.c=%.b.a.c)
BENCH_OBJ   := $(BENCH_A:%.b.a.c=%.b.a.o)
BENCH_DEP   := $(BENCH_A:%.b.a.c=%.b.a.d)
BENCH_CI    := $(BENCH_A:%.b.a.c=%.b.a.ci)
BENCH_GCNO  := $(BENCH_A:%.b.a.c=%.b.a.gcno)
BENCH_GCDA  := $(BENCH_A:%.b.a.c=%.b.a.gcda)
BENCH_PERF  := $(BENCH_RUNNER:%=%.perf)
BENCH_TRACE := $(BENCH_RUNNER:%=%.trace)
BENCH_CSV   := $(BENCH_RUNNER:%=%.csv)

# overridable tools/flags
CC            ?= gcc
AR            ?= ar
SIZE          ?= size
CTAGS         ?= ctags
NM            ?= nm
OBJDUMP       ?= objdump
VALGRIND      ?= valgrind
GDB           ?= gdb
PERF          ?= perf
PRETTYASSERTS ?= ./scripts/prettyasserts.py

CFLAGS += -fcallgraph-info=su
CFLAGS += -g3
CFLAGS += -I.
CFLAGS += -std=c99 -Wall -Wextra -pedantic
# compiler bug: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=101854
CFLAGS += -Wno-stringop-overflow
CFLAGS += -ftrack-macro-expansion=0
ifdef DEBUG
CFLAGS += -O0
else
CFLAGS += -Os
endif
ifdef TRACE
CFLAGS += -DLFS_YES_TRACE
endif
ifdef COVGEN
CFLAGS += --coverage
endif
ifdef PERFGEN
CFLAGS += -fno-omit-frame-pointer
endif
ifdef PERFBDGEN
CFLAGS += -fno-omit-frame-pointer
endif

TEST_CFLAGS += -Wno-unused-function
TEST_CFLAGS += -Wno-format-overflow

BENCH_CFLAGS += -Wno-unused-function
BENCH_CFLAGS += -Wno-format-overflow

ifdef VERBOSE
CODEFLAGS    += -v
DATAFLAGS    += -v
STACKFLAGS   += -v
STRUCTSFLAGS += -v
COVFLAGS     += -v
PERFFLAGS    += -v
PERFBDFLAGS  += -v
endif
# forward -j flag
PERFFLAGS   += $(filter -j%,$(MAKEFLAGS))
PERFBDFLAGS += $(filter -j%,$(MAKEFLAGS))
ifneq ($(NM),nm)
CODEFLAGS += --nm-path="$(NM)"
DATAFLAGS += --nm-path="$(NM)"
endif
ifneq ($(OBJDUMP),objdump)
CODEFLAGS    += --objdump-path="$(OBJDUMP)"
DATAFLAGS    += --objdump-path="$(OBJDUMP)"
STRUCTSFLAGS += --objdump-path="$(OBJDUMP)"
PERFFLAGS    += --objdump-path="$(OBJDUMP)"
PERFBDFLAGS  += --objdump-path="$(OBJDUMP)"
endif
ifneq ($(PERF),perf)
PERFFLAGS += --perf-path="$(PERF)"
endif

TESTFLAGS  += -b
BENCHFLAGS += -b
# forward -j flag
TESTFLAGS  += $(filter -j%,$(MAKEFLAGS))
BENCHFLAGS += $(filter -j%,$(MAKEFLAGS))
ifdef PERFGEN
TESTFLAGS  += -p $(TEST_PERF)
BENCHFLAGS += -p $(BENCH_PERF)
endif
ifdef PERFBDGEN
TESTFLAGS += -t $(TEST_TRACE) --trace-backtrace --trace-freq=100
endif
ifdef PERFBDGEN
BENCHFLAGS += -t $(BENCH_TRACE) --trace-backtrace --trace-freq=100
endif
ifdef TESTMARKS
TESTFLAGS += -o $(TEST_CSV)
endif
ifdef BENCHMARKS
BENCHFLAGS += -o $(BENCH_CSV)
endif
ifdef VERBOSE
TESTFLAGS   += -v
TESTCFLAGS  += -v
BENCHFLAGS  += -v
BENCHCFLAGS += -v
endif
ifdef EXEC
TESTFLAGS  += --exec="$(EXEC)"
BENCHFLAGS += --exec="$(EXEC)"
endif
ifneq ($(GDB),gdb)
TESTFLAGS  += --gdb-path="$(GDB)"
BENCHFLAGS += --gdb-path="$(GDB)"
endif
ifneq ($(VALGRIND),valgrind)
TESTFLAGS  += --valgrind-path="$(VALGRIND)"
BENCHFLAGS += --valgrind-path="$(VALGRIND)"
endif
ifneq ($(PERF),perf)
TESTFLAGS  += --perf-path="$(PERF)"
BENCHFLAGS += --perf-path="$(PERF)"
endif


# top-level commands

## Build littlefs
.PHONY: all build
all build: $(TARGET)

## Build assembly files
.PHONY: asm
asm: $(ASM)

## Find the total size
.PHONY: size
size: $(OBJ)
	$(SIZE) -t $^

## Generate a ctags file
.PHONY: tags ctags
tags ctags:
	$(strip $(CTAGS) \
		--totals --fields=+n --c-types=+p \
		$(shell find -H -name '*.h') $(SRC))

## Show this help text
.PHONY: help
help:
	@$(strip awk '/^## / { \
			sub(/^## /,""); \
			getline rule; \
			while (rule ~ /^(#|\.PHONY|ifdef|ifndef)/) getline rule; \
			gsub(/:.*/, "", rule); \
			if (length(rule) <= 21) { \
				printf "%2s%-21s %s\n", "", rule, $$0; \
			} else { \
				printf "%2s%s\n", "", rule; \
				printf "%24s%s\n", "", $$0; \
			} \
		}' $(MAKEFILE_LIST))

## Find the per-function code size
.PHONY: code
code: CODEFLAGS+=-S
code: $(OBJ) $(BUILDDIR)/lfs.code.csv
	./scripts/code.py $(OBJ) $(CODEFLAGS)

## Compare per-function code size
.PHONY: code-diff
code-diff: $(OBJ)
	./scripts/code.py $^ $(CODEFLAGS) -d $(BUILDDIR)/lfs.code.csv

## Find the per-function data size
.PHONY: data
data: DATAFLAGS+=-S
data: $(OBJ) $(BUILDDIR)/lfs.data.csv
	./scripts/data.py $(OBJ) $(DATAFLAGS)

## Compare per-function data size
.PHONY: data-diff
data-diff: $(OBJ)
	./scripts/data.py $^ $(DATAFLAGS) -d $(BUILDDIR)/lfs.data.csv

## Find the per-function stack usage
.PHONY: stack
stack: STACKFLAGS+=-S
stack: $(CI) $(BUILDDIR)/lfs.stack.csv
	./scripts/stack.py $(CI) $(STACKFLAGS)

## Compare per-function stack usage
.PHONY: stack-diff
stack-diff: $(CI)
	./scripts/stack.py $^ $(STACKFLAGS) -d $(BUILDDIR)/lfs.stack.csv

## Find function sizes
.PHONY: funcs
funcs: SUMMARYFLAGS+=-S
funcs: SHELL=/bin/bash
funcs: $(BUILDDIR)/lfs.code.csv $(BUILDDIR)/lfs.stack.csv
	$(strip ./scripts/csv.py \
		<(./scripts/csv.py $(BUILDDIR)/lfs.code.csv \
			-fcode=size -q $(SUMMARYFLAGS) -o-) \
		<(./scripts/csv.py $(BUILDDIR)/lfs.stack.csv \
			-fstack='max(limit)' -q $(SUMMARYFLAGS) -o-) \
		-bfunction -fcode -fstack='max(stack)' \
		$(SUMMARYFLAGS))

## Compare function sizes
.PHONY: funcs-diff
funcs-diff: SHELL=/bin/bash
funcs-diff: $(OBJ) $(CI)
	$(strip ./scripts/csv.py \
		<(./scripts/csv.py \
			<(./scripts/code.py $(OBJ) -q $(CODEFLAGS) -o-) \
			-fcode=size -q $(SUMMARYFLAGS) -o-) \
		<(./scripts/csv.py \
			<(./scripts/stack.py $(CI) -q $(STACKFLAGS) -o-) \
			-fstack='max(limit)' -q $(SUMMARYFLAGS) -o-) \
		-bfunction -fcode -fstack='max(stack)' \
		$(SUMMARYFLAGS) -d <(./scripts/csv.py \
			<(./scripts/csv.py $(BUILDDIR)/lfs.code.csv \
				-fcode=size -q $(SUMMARYFLAGS) -o-) \
			<(./scripts/csv.py $(BUILDDIR)/lfs.stack.csv \
				-fstack='max(limit)' -q $(SUMMARYFLAGS) -o-) \
			-fcode -fstack='max(stack)' \
			-q $(SUMMARYFLAGS) -o-))

## Find struct sizes
.PHONY: structs
structs: STRUCTSFLAGS+=-S
structs: $(OBJ) $(BUILDDIR)/lfs.structs.csv
	./scripts/structs.py $(OBJ) $(STRUCTSFLAGS)

## Compare struct sizes
.PHONY: structs-diff
structs-diff: $(OBJ)
	./scripts/structs.py $^ $(STRUCTSFLAGS) -d $(BUILDDIR)/lfs.structs.csv

## Find the line/branch coverage after a test run
.PHONY: cov
cov: COVFLAGS+=-s
cov: $(GCDA) $(BUILDDIR)/lfs.cov.csv
	$(strip ./scripts/cov.py $(GCDA) \
		$(patsubst %,-F%,$(SRC)) \
		$(COVFLAGS))

## Compare line/branch coverage
.PHONY: cov-diff
cov-diff: $(GCDA)
	$(strip ./scripts/cov.py $^ \
		$(patsubst %,-F%,$(SRC)) \
		$(COVFLAGS) -d $(BUILDDIR)/lfs.cov.csv)

## Find the perf results after bench run with PERFGEN
.PHONY: perf
perf: PERFFLAGS+=-S
perf: $(BENCH_PERF) $(BUILDDIR)/lfs.perf.csv
	$(strip ./scripts/perf.py $(BENCH_PERF) \
		$(patsubst %,-F%,$(SRC)) \
		$(PERFFLAGS))

## Compare perf results
.PHONY: perf-diff
perf-diff: $(BENCH_PERF)
	$(strip ./scripts/perf.py $^ \
		$(patsubst %,-F%,$(SRC)) \
		$(PERFFLAGS) -d $(BUILDDIR)/lfs.perf.csv)

## Find the perfbd results after a bench run
.PHONY: perfbd
perfbd: PERFBDFLAGS+=-S
perfbd: $(BENCH_TRACE) $(BUILDDIR)/lfs.perfbd.csv
	$(strip ./scripts/perfbd.py $(BENCH_RUNNER) $(BENCH_TRACE) \
		$(patsubst %,-F%,$(SRC)) \
		$(PERFBDFLAGS))

## Compare perfbd results
.PHONY: perfbd-diff
perfbd-diff: $(BENCH_TRACE)
	$(strip ./scripts/perfbd.py $(BENCH_RUNNER) $^ \
		$(patsubst %,-F%,$(SRC)) \
		$(PERFBDFLAGS) -d $(BUILDDIR)/lfs.perfbd.csv)

## Find a summary of compile-time sizes
.PHONY: summary sizes
summary sizes: SHELL=/bin/bash
summary sizes: \
		$(BUILDDIR)/lfs.code.csv \
		$(BUILDDIR)/lfs.data.csv \
		$(BUILDDIR)/lfs.stack.csv \
		$(BUILDDIR)/lfs.structs.csv
	$(strip ./scripts/csv.py \
		<(./scripts/csv.py $(BUILDDIR)/lfs.code.csv \
			-fcode=size -q $(SUMMARYFLAGS) -o-) \
		<(./scripts/csv.py $(BUILDDIR)/lfs.data.csv \
			-fdata=size -q $(SUMMARYFLAGS) -o-) \
		<(./scripts/csv.py $(BUILDDIR)/lfs.stack.csv \
			-fstack='max(limit)' -q $(SUMMARYFLAGS) -o-) \
		<(./scripts/csv.py $(BUILDDIR)/lfs.structs.csv \
			-fstructs=size -q $(SUMMARYFLAGS) -o-) \
		-bfunction -fcode -fdata -fstack='max(stack)' -fstructs \
		-Y $(SUMMARYFLAGS))

## Compare compile-time sizes
.PHONY: summary-diff sizes-diff
summary-diff sizes-diff: SHELL=/bin/bash
summary-diff sizes-diff: $(OBJ) $(CI)
	$(strip ./scripts/csv.py \
		<(./scripts/csv.py \
			<(./scripts/code.py $(OBJ) -q $(CODEFLAGS) -o-) \
			-fcode=size -q $(SUMMARYFLAGS) -o-) \
		<(./scripts/csv.py \
			<(./scripts/data.py $(OBJ) -q $(DATAFLAGS) -o-) \
			-fdata=size -q $(SUMMARYFLAGS) -o-) \
		<(./scripts/csv.py \
			<(./scripts/stack.py $(CI) -q $(STACKFLAGS) -o-) \
			-fstack='max(limit)' -q $(SUMMARYFLAGS) -o-) \
		<(./scripts/csv.py \
			<(./scripts/structs.py $(OBJ) -q $(STRUCTSFLAGS) -o-) \
			-fstructs=size -q $(SUMMARYFLAGS) -o-) \
		-bfunction -fcode -fdata -fstack='max(stack)' -fstructs \
		-Y -p $(SUMMARYFLAGS) -d <(./scripts/csv.py \
			<(./scripts/csv.py $(BUILDDIR)/lfs.code.csv \
				-fcode=size -q $(SUMMARYFLAGS) -o-) \
			<(./scripts/csv.py $(BUILDDIR)/lfs.data.csv \
				-fdata=size -q $(SUMMARYFLAGS) -o-) \
			<(./scripts/csv.py $(BUILDDIR)/lfs.stack.csv \
				-fstack='max(limit)' -q $(SUMMARYFLAGS) -o-) \
			<(./scripts/csv.py $(BUILDDIR)/lfs.structs.csv \
				-fstructs=size -q $(SUMMARYFLAGS) -o-) \
			-fcode -fdata -fstack='max(stack)' -fstructs \
			-q $(SUMMARYFLAGS) -o-))

## Build the test-runner
.PHONY: test-runner build-tests
test-runner build-tests: CFLAGS+=$(TEST_CFLAGS)
# note we remove some binary dependent files during compilation,
# otherwise it's way to easy to end up with outdated results
test-runner build-tests: $(TEST_RUNNER)
ifdef COVGEN
	rm -f $(TEST_GCDA)
endif
ifdef PERFGEN
	rm -f $(TEST_PERF)
endif
ifdef PERFBDGEN
	rm -f $(TEST_TRACE)
endif

## Run the tests, -j enables parallel tests
.PHONY: test
test: test-runner
	./scripts/test.py -R$(TEST_RUNNER) $(TESTFLAGS)

## List the tests
.PHONY: test-list list-tests
test-list list-tests: test-runner
	./scripts/test.py -R$(TEST_RUNNER) $(TESTFLAGS) -l

## Summarize the test results
.PHONY: testmarks
testmarks: SUMMARYFLAGS+=-spassed -Stime
testmarks: $(TEST_CSV) $(BUILDDIR)/lfs.test.csv
	$(strip ./scripts/csv.py $(TEST_CSV) \
		-bsuite -fpassed -ftime \
		$(SUMMARYFLAGS))

## Compare test results against a previous run
.PHONY: testmarks-diff
testmarks-diff: $(TEST_CSV)
	$(strip ./scripts/csv.py $^ \
		-bsuite -fpassed -ftime \
		$(SUMMARYFLAGS) -d $(BUILDDIR)/lfs.test.csv)

## Build the bench-runner
.PHONY: bench-runner build-benches
bench-runner build-benches: CFLAGS+=$(BENCH_CFLAGS)
# note we remove some binary dependent files during compilation,
# otherwise it's way to easy to end up with outdated results
bench-runner build-benches: $(BENCH_RUNNER)
ifdef COVGEN
	rm -f $(BENCH_GCDA)
endif
ifdef PERFGEN
	rm -f $(BENCH_PERF)
endif
ifdef PERFBDGEN
	rm -f $(BENCH_TRACE)
endif

## Run the benches, -j enables parallel benches
.PHONY: bench
bench: bench-runner
	./scripts/bench.py -R$(BENCH_RUNNER) $(BENCHFLAGS)

## List the benches
.PHONY: bench-list list-benches
bench-list list-benches: bench-runner
	./scripts/bench.py -R$(BENCH_RUNNER) $(BENCHFLAGS) -l

## Summarize the bench results
.PHONY: benchmarks
benchmarks: SUMMARYFLAGS+=-Serased -Sproged -Sreaded
benchmarks: $(BENCH_CSV) $(BUILDDIR)/lfs.bench.csv
	$(strip ./scripts/csv.py $(BENCH_CSV) \
		-bsuite -freaded -fproged -ferased \
		$(SUMMARYFLAGS))

## Compare bench results against a previous run
.PHONY: benchmarks-diff
benchmarks-diff: $(BENCH_CSV)
	$(strip ./scripts/csv.py $^ \
		-bsuite -freaded -fproged -ferased \
		$(SUMMARYFLAGS) -d $(BUILDDIR)/lfs.bench.csv)



# low-level rules
-include $(DEP)
-include $(TEST_DEP)
.SUFFIXES:
.SECONDARY:

$(BUILDDIR)/lfs: $(OBJ)
	$(CC) $(CFLAGS) $^ $(LFLAGS) -o $@

$(BUILDDIR)/liblfs.a: $(OBJ)
	$(AR) rcs $@ $^

$(BUILDDIR)/lfs.code.csv: $(OBJ)
	./scripts/code.py $^ -q $(CODEFLAGS) -o $@

$(BUILDDIR)/lfs.data.csv: $(OBJ)
	./scripts/data.py $^ -q $(DATAFLAGS) -o $@

$(BUILDDIR)/lfs.stack.csv: $(CI)
	./scripts/stack.py $^ -q $(STACKFLAGS) -o $@

$(BUILDDIR)/lfs.structs.csv: $(OBJ)
	./scripts/structs.py $^ -q $(STRUCTSFLAGS) -o $@

$(BUILDDIR)/lfs.cov.csv: $(GCDA)
	$(strip ./scripts/cov.py $^ \
		$(patsubst %,-F%,$(SRC)) \
		-q $(COVFLAGS) -o $@)

$(BUILDDIR)/lfs.perf.csv: $(BENCH_PERF)
	$(strip ./scripts/perf.py $^ \
		$(patsubst %,-F%,$(SRC)) \
		-q $(PERFFLAGS) -o $@)

$(BUILDDIR)/lfs.perfbd.csv: $(BENCH_TRACE)
	$(strip ./scripts/perfbd.py $(BENCH_RUNNER) $^ \
		$(patsubst %,-F%,$(SRC)) \
		-q $(PERFBDFLAGS) -o $@)

$(BUILDDIR)/lfs.test.csv: $(TEST_CSV)
	cp $^ $@

$(BUILDDIR)/lfs.bench.csv: $(BENCH_CSV)
	cp $^ $@

$(BUILDDIR)/runners/test_runner: $(TEST_OBJ)
	$(CC) $(CFLAGS) $^ $(LFLAGS) -o $@

$(BUILDDIR)/runners/bench_runner: $(BENCH_OBJ)
	$(CC) $(CFLAGS) $^ $(LFLAGS) -o $@

# our main build rule generates .o, .d, and .ci files, the latter
# used for stack analysis
$(BUILDDIR)/%.o $(BUILDDIR)/%.ci: %.c
	$(CC) -c -MMD $(CFLAGS) $< -o $(BUILDDIR)/$*.o

$(BUILDDIR)/%.s: %.c
	$(CC) -S $(CFLAGS) $< -o $@

$(BUILDDIR)/%.a.c: %.c
	$(PRETTYASSERTS) -Plfs_ $< -o $@

$(BUILDDIR)/%.a.c: $(BUILDDIR)/%.c
	$(PRETTYASSERTS) -Plfs_ $< -o $@

$(BUILDDIR)/%.t.c: %.toml
	./scripts/test.py -c $< $(TESTCFLAGS) -o $@

$(BUILDDIR)/%.t.c: %.c $(TESTS)
	./scripts/test.py -c $(TESTS) -s $< $(TESTCFLAGS) -o $@

$(BUILDDIR)/%.b.c: %.toml
	./scripts/bench.py -c $< $(BENCHCFLAGS) -o $@

$(BUILDDIR)/%.b.c: %.c $(BENCHES)
	./scripts/bench.py -c $(BENCHES) -s $< $(BENCHCFLAGS) -o $@

## Clean everything
.PHONY: clean
clean:
	rm -f $(BUILDDIR)/lfs
	rm -f $(BUILDDIR)/liblfs.a
	rm -f $(BUILDDIR)/lfs.code.csv
	rm -f $(BUILDDIR)/lfs.data.csv
	rm -f $(BUILDDIR)/lfs.stack.csv
	rm -f $(BUILDDIR)/lfs.structs.csv
	rm -f $(BUILDDIR)/lfs.cov.csv
	rm -f $(BUILDDIR)/lfs.perf.csv
	rm -f $(BUILDDIR)/lfs.perfbd.csv
	rm -f $(BUILDDIR)/lfs.test.csv
	rm -f $(BUILDDIR)/lfs.bench.csv
	rm -f $(OBJ)
	rm -f $(DEP)
	rm -f $(ASM)
	rm -f $(CI)
	rm -f $(TEST_RUNNER)
	rm -f $(TEST_A)
	rm -f $(TEST_C)
	rm -f $(TEST_OBJ)
	rm -f $(TEST_DEP)
	rm -f $(TEST_CI)
	rm -f $(TEST_GCNO)
	rm -f $(TEST_GCDA)
	rm -f $(TEST_PERF)
	rm -f $(TEST_TRACE)
	rm -f $(TEST_CSV)
	rm -f $(BENCH_RUNNER)
	rm -f $(BENCH_A)
	rm -f $(BENCH_C)
	rm -f $(BENCH_OBJ)
	rm -f $(BENCH_DEP)
	rm -f $(BENCH_CI)
	rm -f $(BENCH_GCNO)
	rm -f $(BENCH_GCDA)
	rm -f $(BENCH_PERF)
	rm -f $(BENCH_TRACE)
	rm -f $(BENCH_CSV)
