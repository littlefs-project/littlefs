TARGET = lfs.a
ifneq ($(wildcard test.c main.c),)
override TARGET = lfs
endif

CC ?= gcc
AR ?= ar
SIZE ?= size
NM ?= nm
GCOV ?= gcov
LCOV ?= lcov

SRC += $(wildcard *.c bd/*.c)
OBJ := $(SRC:.c=.o)
DEP := $(SRC:.c=.d)
ASM := $(SRC:.c=.s)

ifdef DEBUG
override CFLAGS += -O0 -g3
else
override CFLAGS += -Os
endif
ifdef WORD
override CFLAGS += -m$(WORD)
endif
ifdef TRACE
override CFLAGS += -DLFS_YES_TRACE
endif
override CFLAGS += -I.
override CFLAGS += -std=c99 -Wall -pedantic
override CFLAGS += -Wextra -Wshadow -Wjump-misses-init -Wundef

ifdef VERBOSE
override SCRIPTFLAGS += -v
endif
ifdef EXEC
override TESTFLAGS += $(patsubst %,--exec=%,$(EXEC))
endif


.PHONY: all build
all build: $(TARGET)

.PHONY: asm
asm: $(ASM)

.PHONY: size
size: $(OBJ)
	$(SIZE) -t $^

.PHONY: code
code:
	./scripts/code.py $(SCRIPTFLAGS)

.PHONY: coverage
coverage:
	./scripts/coverage.py $(SCRIPTFLAGS)

.PHONY: test
test:
	./scripts/test.py $(TESTFLAGS) $(SCRIPTFLAGS)
.SECONDEXPANSION:
test%: tests/test$$(firstword $$(subst \#, ,%)).toml
	./scripts/test.py $@ $(TESTFLAGS) $(SCRIPTFLAGS)

-include $(DEP)

lfs: $(OBJ)
	$(CC) $(CFLAGS) $^ $(LFLAGS) -o $@

%.a: $(OBJ)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) -c -MMD $(CFLAGS) $< -o $@

%.s: %.c
	$(CC) -S $(CFLAGS) $< -o $@

%.gcda.gcov: %.gcda
	( cd $(dir $@) ; $(GCOV) -ri $(notdir $<) )

.PHONY: clean
clean:
	rm -f $(TARGET)
	rm -f $(OBJ)
	rm -f $(DEP)
	rm -f $(ASM)
	rm -f tests/*.toml.*
	rm -f sizes/*
	rm -f results/*
