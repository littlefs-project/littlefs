# overrideable build dir, default is in-place
BUILDDIR ?= .
# overrideable target, default to building a library
ifneq ($(wildcard test.c main.c),)
TARGET ?= $(BUILDDIR)/lfs3
else
TARGET ?= $(BUILDDIR)/liblfs3.a
endif


# find source files
SRC  ?= $(filter-out %.t.c %.b.c %.a.c,$(wildcard *.c))
OBJ  := $(SRC:%.c=$(BUILDDIR)/%.o)
DEP  := $(SRC:%.c=$(BUILDDIR)/%.d)
ASM  := $(SRC:%.c=$(BUILDDIR)/%.s)
CI   := $(SRC:%.c=$(BUILDDIR)/%.ci)
GCDA := $(SRC:%.c=$(BUILDDIR)/%.t.a.gcda)

TESTS ?= $(wildcard tests/*.toml)
TEST_SRC ?= \
		$(SRC) \
		$(filter-out %.t.c %.b.c %.a.c,$(wildcard bd/*.c)) \
		runners/test_runner.c
TEST_RUNNER ?= $(BUILDDIR)/runners/test_runner
TEST_C     := \
		$(TESTS:%.toml=$(BUILDDIR)/%.t.c) \
		$(TEST_SRC:%.c=$(BUILDDIR)/%.t.c)
TEST_A     := $(TEST_C:%.t.c=%.t.a.c)
TEST_OBJ   := $(TEST_A:%.t.a.c=%.t.a.o)
TEST_DEP   := $(TEST_A:%.t.a.c=%.t.a.d)
TEST_CI    := $(TEST_A:%.t.a.c=%.t.a.ci)
TEST_GCNO  := $(TEST_A:%.t.a.c=%.t.a.gcno)
TEST_GCDA  := $(TEST_A:%.t.a.c=%.t.a.gcda)
TEST_PERF  := $(TEST_RUNNER:%=%.perf)
TEST_TRACE := $(TEST_RUNNER:%=%.trace)
TEST_CSV   := $(TEST_RUNNER:%=%.csv)

BENCHES ?= $(wildcard benches/*.toml)
BENCH_SRC ?= \
		$(SRC) \
		$(filter-out %.t.c %.b.c %.a.c,$(wildcard bd/*.c)) \
		runners/bench_runner.c
BENCH_RUNNER ?= $(BUILDDIR)/runners/bench_runner
BENCH_C     := \
		$(BENCHES:%.toml=$(BUILDDIR)/%.b.c) \
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
OBJDUMP       ?= objdump
VALGRIND      ?= valgrind
GDB           ?= gdb
PERF          ?= perf
PRETTYASSERTS ?= ./scripts/prettyasserts.py

CFLAGS += -fcallgraph-info=su
CFLAGS += -g3
CFLAGS += -I.
CFLAGS += -std=c99 -Wall -Wextra -pedantic
# labels are useful for debugging, in-function organization, etc
CFLAGS += -Wno-unused-label
# life's too short to not use this flag
CFLAGS += -Wno-unused-function
# compiler bug: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=101854
CFLAGS += -Wno-stringop-overflow
CFLAGS += -ftrack-macro-expansion=0
ifdef DEBUG
CFLAGS += -O0
else
CFLAGS += -Os
endif
ifdef TRACE
CFLAGS += -DLFS3_YES_TRACE
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

# also forward all LFS3_* environment variables
CFLAGS += $(foreach D,$(filter LFS3_%,$(.VARIABLES)),-D$D=$($D))

TEST_CFLAGS += -Wno-unused-function
TEST_CFLAGS += -Wno-format-overflow

BENCH_CFLAGS += -Wno-unused-function
BENCH_CFLAGS += -Wno-format-overflow

ifdef VERBOSE
CODEFLAGS    += -v
DATAFLAGS    += -v
STACKFLAGS   += -v
CTXFLAGS     += -v
STRUCTSFLAGS += -v
COVFLAGS     += -v
PERFFLAGS    += -v
PERFBDFLAGS  += -v
endif
# forward -j flag
PERFFLAGS   += $(filter -j%,$(MAKEFLAGS))
PERFBDFLAGS += $(filter -j%,$(MAKEFLAGS))
ifneq ($(OBJDUMP),objdump)
CODEFLAGS    += --objdump-path="$(OBJDUMP)"
DATAFLAGS    += --objdump-path="$(OBJDUMP)"
STACKFLAGS   += --objdump-path="$(OBJDUMP)"
CTXFLAGS     += --objdump-path="$(OBJDUMP)"
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
TESTFLAGS  += -p$(TEST_PERF)
BENCHFLAGS += -p$(BENCH_PERF)
endif
ifdef PERFBDGEN
TESTFLAGS  += -t$(TEST_TRACE) --trace-backtrace --trace-freq=100
BENCHFLAGS += -t$(BENCH_TRACE) --trace-backtrace --trace-freq=100
endif
ifdef TESTMARKS
TESTFLAGS  += -o$(TEST_CSV)
endif
ifdef BENCHMARKS
BENCHFLAGS += -o$(BENCH_CSV)
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

# this is a bit of a hack, but we want to make sure the BUILDDIR
# directory structure is correct before we run any commands
ifneq ($(BUILDDIR),.)
$(if $(findstring n,$(MAKEFLAGS)),, $(shell mkdir -p \
	$(BUILDDIR) \
	$(addprefix $(BUILDDIR)/,$(dir \
		$(SRC) \
		$(TESTS) \
		$(TEST_SRC) \
		$(BENCHES) \
		$(BENCH_SRC)))))
endif


# top-level commands

## Build littlefs
.PHONY: all build
all build: $(TARGET)

## Build assembly files
.PHONY: asm
asm: $(ASM)

## Find total section sizes
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
code: $(OBJ)
	./scripts/code.py $^ $(CODEFLAGS)

## Save the per-function code size
.PHONY: code-csv
code-csv: $(BUILDDIR)/lfs3.code.csv

## Compare per-function code size
.PHONY: code-diff
code-diff: $(OBJ)
	./scripts/code.py $^ $(CODEFLAGS) -d $(BUILDDIR)/lfs3.code.csv

## Find the per-function data size
.PHONY: data
data: DATAFLAGS+=-S
data: $(OBJ)
	./scripts/data.py $^ $(DATAFLAGS)

## Save the per-function data size
.PHONY: data-csv
data-csv: $(BUILDDIR)/lfs3.data.csv

## Compare per-function data size
.PHONY: data-diff
data-diff: $(OBJ)
	./scripts/data.py $^ $(DATAFLAGS) -d $(BUILDDIR)/lfs3.data.csv

## Find the per-function stack usage
.PHONY: stack
stack: STACKFLAGS+=-S
stack: $(CI)
	./scripts/stack.py $^ $(STACKFLAGS)

## Save the per-function stack usage
.PHONY: stack-csv
stack-csv: $(BUILDDIR)/lfs3.stack.csv

## Compare per-function stack usage
.PHONY: stack-diff
stack-diff: $(CI)
	./scripts/stack.py $^ $(STACKFLAGS) -d $(BUILDDIR)/lfs3.stack.csv

## Find the per-function context
.PHONY: ctx
ctx: CTXFLAGS+=-S
ctx: $(OBJ)
	./scripts/ctx.py $^ $(CTXFLAGS)

## Save the per-function context
.PHONY: ctx-csv
ctx-csv: $(BUILDDIR)/lfs3.ctx.csv

## Compare per-function context
.PHONY: ctx-diff
ctx-diff: $(CI)
	./scripts/ctx.py $^ $(CTXFLAGS) -d $(BUILDDIR)/lfs3.ctx.csv

## Find function sizes
.PHONY: funcs
funcs: SUMMARYFLAGS+=-S
funcs: SHELL=/bin/bash
funcs: $(OBJ) $(CI)
	$(strip ./scripts/csv.py \
		<(./scripts/code.py $(OBJ) $(CODEFLAGS) -o-) \
		<(./scripts/stack.py $(CI) $(STACKFLAGS) -o-) \
	    <(./scripts/ctx.py $(OBJ) $(CTXFLAGS) -o-) \
		-bfunction \
		-fcode=code_size \
		-fstack='max(stack_limit)' \
		-fctx='max(ctx_size)' \
		$(SUMMARYFLAGS))

## Save function sizes
.PHONY: funcs-csv
funcs-csv: SHELL=/bin/bash
funcs-csv: \
		$(BUILDDIR)/lfs3.code.csv \
		$(BUILDDIR)/lfs3.stack.csv \
		$(BUILDDIR)/lfs3.ctx.csv

## Compare function sizes
.PHONY: funcs-diff
funcs-diff: SHELL=/bin/bash
funcs-diff: $(OBJ) $(CI)
	$(strip ./scripts/csv.py \
		<(./scripts/code.py $(OBJ) $(CODEFLAGS) -o-) \
		<(./scripts/stack.py $(CI) $(STACKFLAGS) -o-) \
		<(./scripts/ctx.py $(OBJ) $(CTXFLAGS) -o-) \
		-bfunction \
		-fcode=code_size \
		-fstack='max(stack_limit)' \
		-fctx='max(ctx_size)' \
		-d <(./scripts/csv.py \
			$(BUILDDIR)/lfs3.code.csv \
			$(BUILDDIR)/lfs3.stack.csv \
			$(BUILDDIR)/lfs3.ctx.csv \
			-fcode_size=code_size \
			-fstack_limit='max(stack_limit)' \
			-fctx_size='max(ctx_size)' \
			-o-) \
		$(SUMMARYFLAGS))

## Find struct sizes
.PHONY: structs
structs: STRUCTSFLAGS+=-S
structs: $(OBJ)
	./scripts/structs.py $^ $(STRUCTSFLAGS)

## Save struct sizes
.PHONY: structs-csv
structs-csv: $(BUILDDIR)/lfs3.structs.csv

## Compare struct sizes
.PHONY: structs-diff
structs-diff: $(OBJ)
	./scripts/structs.py $^ $(STRUCTSFLAGS) -d $(BUILDDIR)/lfs3.structs.csv

## Find the line/branch coverage after a test run
.PHONY: cov
cov: COVFLAGS+=-s
cov: $(GCDA)
	$(strip ./scripts/cov.py $^ \
		$(patsubst %,-F%,$(SRC)) \
		$(COVFLAGS))

## Save line/branch coverage
.PHONY: cov-csv
cov-csv: $(BUILDDIR)/lfs3.cov.csv

## Compare line/branch coverage
.PHONY: cov-diff
cov-diff: $(GCDA)
	$(strip ./scripts/cov.py $^ \
		$(patsubst %,-F%,$(SRC)) \
		$(COVFLAGS) -d $(BUILDDIR)/lfs3.cov.csv)

## Find the perf results after bench run with PERFGEN
.PHONY: perf
perf: PERFFLAGS+=-S
perf: $(BENCH_PERF)
	$(strip ./scripts/perf.py $^ \
		$(patsubst %,-F%,$(SRC)) \
		$(PERFFLAGS))

## Save perf results
.PHONY: perf-csv
perf-csv: $(BUILDDIR)/lfs3.perf.csv

## Compare perf results
.PHONY: perf-diff
perf-diff: $(BENCH_PERF)
	$(strip ./scripts/perf.py $^ \
		$(patsubst %,-F%,$(SRC)) \
		$(PERFFLAGS) -d $(BUILDDIR)/lfs3.perf.csv)

## Find the perfbd results after a bench run
.PHONY: perfbd
perfbd: PERFBDFLAGS+=-S
perfbd: $(BENCH_TRACE)
	$(strip ./scripts/perfbd.py $(BENCH_RUNNER) $^ \
		$(patsubst %,-F%,$(SRC)) \
		$(PERFBDFLAGS))

## Save perfbd results
.PHONY: perfbd-csv
perfbd-csv: $(BUILDDIR)/lfs3.perfbd.csv

## Compare perfbd results
.PHONY: perfbd-diff
perfbd-diff: $(BENCH_TRACE)
	$(strip ./scripts/perfbd.py $(BENCH_RUNNER) $^ \
		$(patsubst %,-F%,$(SRC)) \
		$(PERFBDFLAGS) -d $(BUILDDIR)/lfs3.perfbd.csv)

## Find a summary of compile-time sizes
.PHONY: summary sizes
summary sizes: SHELL=/bin/bash
summary sizes: $(OBJ) $(CI)
	$(strip ./scripts/csv.py \
		<(./scripts/code.py $(OBJ) $(CODEFLAGS) -o-) \
		<(./scripts/data.py $(OBJ) $(DATAFLAGS) -o-) \
		<(./scripts/stack.py $(CI) $(STACKFLAGS) -o-) \
		<(./scripts/ctx.py $(OBJ) $(CTXFLAGS) -o-) \
		-bfunction \
		-fcode=code_size \
		-fdata=data_size \
		-fstack='max(stack_limit)' \
		-fctx='max(ctx_size)' \
		-Y $(SUMMARYFLAGS))

## Save compile-time sizes
.PHONY: summary-csv sizes-csv
summary-csv sizes-csv: SHELL=/bin/bash
summary-csv sizes-csv: \
		$(BUILDDIR)/lfs3.code.csv \
		$(BUILDDIR)/lfs3.data.csv \
		$(BUILDDIR)/lfs3.stack.csv \
		$(BUILDDIR)/lfs3.ctx.csv

## Compare compile-time sizes
.PHONY: summary-diff sizes-diff
summary-diff sizes-diff: SHELL=/bin/bash
summary-diff sizes-diff: $(OBJ) $(CI)
	$(strip ./scripts/csv.py \
		<(./scripts/csv.py \
			<(./scripts/code.py $(OBJ) $(CODEFLAGS) -o-) \
			<(./scripts/data.py $(OBJ) $(DATAFLAGS) -o-) \
			<(./scripts/stack.py $(CI) $(STACKFLAGS) -o-) \
			<(./scripts/ctx.py $(OBJ) $(CTXFLAGS) -o-) \
			-bbuild=AFTER \
			-fcode=code_size \
			-fdata=data_size \
			-fstack='max(stack_limit)' \
			-fctx='max(ctx_size)' \
			-o-) \
		<(./scripts/csv.py \
			$(BUILDDIR)/lfs3.code.csv \
			$(BUILDDIR)/lfs3.data.csv \
			$(BUILDDIR)/lfs3.stack.csv \
			$(BUILDDIR)/lfs3.ctx.csv \
			-bbuild=BEFORE \
			-fcode=code_size \
			-fdata=data_size \
			-fstack='max(stack_limit)' \
			-fctx='max(ctx_size)' \
			-o-) \
		-bbuild -cBEFORE -Q $(SUMMARYFLAGS))


## Generate a codemap svg
.PHONY: codemap
codemap: CODEMAPFLAGS+=-W1400 -H750 --dark
codemap: $(BUILDDIR)/lfs3.codemap.svg

## Generate a tiny codemap, where 1 pixel ~= 1 byte
.PHONY: codemap-tiny
codemap-tiny: CODEMAPFLAGS+=--dark
codemap-tiny: $(BUILDDIR)/lfs3.codemap_tiny.svg


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
testmarks: $(TEST_CSV)
	$(strip ./scripts/csv.py $^ \
		-bsuite \
		-fpassed=test_passed \
		-ftime=test_time \
		$(SUMMARYFLAGS))

## Save the test results
.PHONY: testmarks-csv
testmarks-csv: $(BUILDDIR)/lfs3.test.csv

## Compare test results against a previous run
.PHONY: testmarks-diff
testmarks-diff: $(TEST_CSV)
	$(strip ./scripts/csv.py $^ \
		-bsuite \
		-fpassed=test_passed \
		-ftime=test_time \
		$(SUMMARYFLAGS) -d $(BUILDDIR)/lfs3.test.csv)

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
benchmarks: $(BENCH_CSV)
	$(strip ./scripts/csv.py $^ \
		-bsuite \
		-freaded=bench_readed \
		-fproged=bench_proged \
		-ferased=bench_erased \
		$(SUMMARYFLAGS))

## Save the bench results
.PHONY: benchmarks-csv
benchmarks-csv: $(BUILDDIR)/lfs3.bench.csv

## Compare bench results against a previous run
.PHONY: benchmarks-diff
benchmarks-diff: $(BENCH_CSV)
	$(strip ./scripts/csv.py $^ \
		-bsuite \
		-freaded=bench_readed \
		-fproged=bench_proged \
		-ferased=bench_erased \
		$(SUMMARYFLAGS) -d $(BUILDDIR)/lfs3.bench.csv)



# low-level rules
-include $(DEP)
-include $(TEST_DEP)
-include $(BENCH_DEP)
.SUFFIXES:
.SECONDARY:

$(BUILDDIR)/lfs3: $(OBJ)
	$(CC) $(CFLAGS) $^ $(LFLAGS) -o$@

$(BUILDDIR)/liblfs3.a: $(OBJ)
	$(AR) rcs $@ $^

$(BUILDDIR)/lfs3.code.csv: $(OBJ)
	./scripts/code.py $^ $(CODEFLAGS) -o$@

$(BUILDDIR)/lfs3.data.csv: $(OBJ)
	./scripts/data.py $^ $(DATAFLAGS) -o$@

$(BUILDDIR)/lfs3.stack.csv: $(CI)
	./scripts/stack.py $^ $(STACKFLAGS) -o$@

$(BUILDDIR)/lfs3.ctx.csv: $(OBJ)
	./scripts/ctx.py $^ $(CTXFLAGS) -o$@

$(BUILDDIR)/lfs3.structs.csv: $(OBJ)
	./scripts/structs.py $^ $(STRUCTSFLAGS) -o$@

$(BUILDDIR)/lfs3.cov.csv: $(GCDA)
	$(strip ./scripts/cov.py $^ \
		$(patsubst %,-F%,$(SRC)) \
		$(COVFLAGS) -o$@)

$(BUILDDIR)/lfs3.perf.csv: $(BENCH_PERF)
	$(strip ./scripts/perf.py $^ \
		$(patsubst %,-F%,$(SRC)) \
		$(PERFFLAGS) -o$@)

$(BUILDDIR)/lfs3.perfbd.csv: $(BENCH_TRACE)
	$(strip ./scripts/perfbd.py $(BENCH_RUNNER) $^ \
		$(patsubst %,-F%,$(SRC)) \
		$(PERFBDFLAGS) -o$@)

$(BUILDDIR)/lfs3.codemap.svg: $(OBJ) $(CI)
	$(strip ./scripts/codemapsvg.py $^ $(CODEMAPFLAGS) -o$@ \
		&& ./scripts/codemap.py $^ --no-header)

$(BUILDDIR)/lfs3.codemap_tiny.svg: $(OBJ) $(CI)
	$(strip ./scripts/codemapsvg.py $^ --tiny $(CODEMAPFLAGS) -o$@ \
		&& ./scripts/codemap.py $^ --no-header)

$(BUILDDIR)/lfs3.test.csv: $(TEST_CSV)
	cp $^ $@

$(BUILDDIR)/lfs3.bench.csv: $(BENCH_CSV)
	cp $^ $@

$(BUILDDIR)/runners/test_runner: $(TEST_OBJ)
	$(CC) $(CFLAGS) $^ $(LFLAGS) -o$@

$(BUILDDIR)/runners/bench_runner: $(BENCH_OBJ)
	$(CC) $(CFLAGS) $^ $(LFLAGS) -o$@

# our main build rule generates .o, .d, and .ci files, the latter
# used for stack analysis
$(BUILDDIR)/%.o $(BUILDDIR)/%.ci: %.c
	$(CC) -c -MMD $(CFLAGS) $< -o $(BUILDDIR)/$*.o

$(BUILDDIR)/%.o $(BUILDDIR)/%.ci: $(BUILDDIR)/%.c
	$(CC) -c -MMD $(CFLAGS) $< -o $(BUILDDIR)/$*.o

$(BUILDDIR)/%.s: %.c
	$(CC) -S $(CFLAGS) $< -o$@

$(BUILDDIR)/%.s: $(BUILDDIR)/%.c
	$(CC) -S $(CFLAGS) $< -o$@

$(BUILDDIR)/%.a.c: %.c
	$(PRETTYASSERTS) -Plfs3_ $< -o$@

$(BUILDDIR)/%.a.c: $(BUILDDIR)/%.c
	$(PRETTYASSERTS) -Plfs3_ $< -o$@

$(BUILDDIR)/%.t.c: %.toml
	./scripts/test.py -c $< $(TESTCFLAGS) -o$@

$(BUILDDIR)/%.t.c: %.c $(TESTS)
	./scripts/test.py -c $(TESTS) -s $< $(TESTCFLAGS) -o$@

$(BUILDDIR)/%.b.c: %.toml
	./scripts/bench.py -c $< $(BENCHCFLAGS) -o$@

$(BUILDDIR)/%.b.c: %.c $(BENCHES)
	./scripts/bench.py -c $(BENCHES) -s $< $(BENCHCFLAGS) -o$@

## Clean everything
.PHONY: clean
clean:
	rm -f $(BUILDDIR)/lfs3
	rm -f $(BUILDDIR)/liblfs3.a
	rm -f $(BUILDDIR)/lfs3.code.csv
	rm -f $(BUILDDIR)/lfs3.data.csv
	rm -f $(BUILDDIR)/lfs3.stack.csv
	rm -f $(BUILDDIR)/lfs3.ctx.csv
	rm -f $(BUILDDIR)/lfs3.structs.csv
	rm -f $(BUILDDIR)/lfs3.cov.csv
	rm -f $(BUILDDIR)/lfs3.perf.csv
	rm -f $(BUILDDIR)/lfs3.perfbd.csv
	rm -f $(BUILDDIR)/lfs3.codemap.svg
	rm -f $(BUILDDIR)/lfs3.codemap_tiny.svg
	rm -f $(BUILDDIR)/lfs3.test.csv
	rm -f $(BUILDDIR)/lfs3.bench.csv
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
