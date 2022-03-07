ifdef BUILDDIR
# make sure BUILDDIR ends with a slash
override BUILDDIR := $(BUILDDIR)/
# bit of a hack, but we want to make sure BUILDDIR directory structure
# is correct before any commands
$(if $(findstring n,$(MAKEFLAGS)),, $(shell mkdir -p \
	$(BUILDDIR) \
	$(BUILDDIR)bd \
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

SRC ?= $(wildcard *.c)
OBJ := $(SRC:%.c=$(BUILDDIR)%.o)
DEP := $(SRC:%.c=$(BUILDDIR)%.d)
ASM := $(SRC:%.c=$(BUILDDIR)%.s)
CGI := $(SRC:%.c=$(BUILDDIR)%.ci)

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

ifdef VERBOSE
override TESTFLAGS     += -v
override CALLSFLAGS    += -v
override CODEFLAGS     += -v
override DATAFLAGS     += -v
override STACKFLAGS    += -v
override STRUCTSFLAGS  += -v
override COVERAGEFLAGS += -v
endif
ifdef EXEC
override TESTFLAGS += --exec="$(EXEC)"
endif
ifdef COVERAGE
override TESTFLAGS += --coverage
endif
ifdef BUILDDIR
override TESTFLAGS     += --build-dir="$(BUILDDIR:/=)"
override CALLSFLAGS    += --build-dir="$(BUILDDIR:/=)"
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

.PHONY: calls
calls: $(CGI)
	./scripts/calls.py $^ $(CALLSFLAGS)

.PHONY: test
test:
	./scripts/test.py $(TESTFLAGS)
.SECONDEXPANSION:
test%: tests/test$$(firstword $$(subst \#, ,%)).toml
	./scripts/test.py $@ $(TESTFLAGS)

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
coverage:
	./scripts/coverage.py $(BUILDDIR)tests/*.toml.info -s $(COVERAGEFLAGS)

.PHONY: summary
summary: $(OBJ) $(CGI)
	$(strip \
		  ./scripts/code.py    $(OBJ) -q      -o - $(CODEFLAGS) \
		| ./scripts/data.py    $(OBJ) -q -m - -o - $(DATAFLAGS) \
		| ./scripts/stack.py   $(CGI) -q -m - -o - $(STACKFLAGS) \
		| ./scripts/structs.py $(OBJ) -q -m - -o - $(STRUCTFLAGS) \
		$(if $(COVERAGE),\
			| ./scripts/coverage.py $(BUILDDIR)tests/*.toml.info \
				-q -m - -o - $(COVERAGEFLAGS)) \
		| ./scripts/summary.py $(SUMMARYFLAGS))


# rules
-include $(DEP)
.SUFFIXES:

$(BUILDDIR)lfs: $(OBJ)
	$(CC) $(CFLAGS) $^ $(LFLAGS) -o $@

$(BUILDDIR)%.a: $(OBJ)
	$(AR) rcs $@ $^

$(BUILDDIR)%.o: %.c
	$(CC) -c -MMD $(CFLAGS) $< -o $@

$(BUILDDIR)%.s: %.c
	$(CC) -S $(CFLAGS) $< -o $@

$(BUILDDIR)%.ci: %.c
	$(CC) -c -MMD -fcallgraph-info=su $(CFLAGS) $< -o $(@:.ci=.o)

# clean everything
.PHONY: clean
clean:
	rm -f $(TARGET)
	rm -f $(TARGET).*.csv
	rm -f $(OBJ)
	rm -f $(CGI)
	rm -f $(DEP)
	rm -f $(ASM)
	rm -f $(BUILDDIR)tests/*.toml.*
