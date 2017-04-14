TARGET = lfs

CC = gcc
AR = ar
SIZE = size

SRC += $(wildcard *.c emubd/*.c)
OBJ := $(SRC:.c=.o)
DEP := $(SRC:.c=.d)
ASM := $(SRC:.c=.s)

TEST := $(patsubst tests/%.sh,%,$(wildcard tests/test_*))

ifdef DEBUG
CFLAGS += -O0 -g3
else
CFLAGS += -Os
endif
ifdef WORD
CFLAGS += -m$(WORD)
endif
CFLAGS += -I.
CFLAGS += -std=c99 -Wall -pedantic


all: $(TARGET)

asm: $(ASM)

size: $(OBJ)
	$(SIZE) -t $^

.SUFFIXES:
test: test_format test_dirs test_files test_alloc test_orphan test_paths
test_%: tests/test_%.sh
	./$<

-include $(DEP)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $^ $(LFLAGS) -o $@

%.a: $(OBJ)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) -c -MMD $(CFLAGS) $< -o $@

%.s: %.c
	$(CC) -S $(CFLAGS) $< -o $@

clean:
	rm -f $(TARGET)
	rm -f $(OBJ)
	rm -f $(DEP)
	rm -f $(ASM)
