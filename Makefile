TARGET_EXE = lfs
TARGET_LIB = lfs.a

TARGET := $(TARGET_LIB)
ifneq (,$(wildcard test.c))
	TARGET := $(TARGET_EXE)
endif

CC = gcc

SRC += $(wildcard *.c emubd/*.c)
OBJ := $(SRC:.c=.o)
DEP := $(SRC:.c=.d)

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

LFLAGS = -lm

-include $(DEP)

all: $(TARGET)

$(TARGET_EXE):  $(TARGET_LIB)
	$(CC) $(CFLAGS) $^ $(LFLAGS) -o $@

%.o: %.c
	$(CC) -c -MMD $(CFLAGS) $< -o $@

$(TARGET_LIB): $(OBJ)
	ar rcs $(TARGET_LIB) $^

.PHONY: test
test:
	cd tests && $(MAKE)

.PHONY: ar
ar : $(OBJ)
	ar rcs $(TARGET_LIB) $^

.PHONY: asm
ASM := $(SRC:.c=.s)
asm: $(ASM)
%.s: %.c
	$(CC) -S $(CFLAGS) $< -o $@

.PHONY: size
size: $(OBJ)
	size -t $^

.PHONY: clean
clean:
	rm -f $(TARGET_EXE) $(TARGET_LIB)
	rm -f $(OBJ)
	rm -f $(DEP)
	rm -f $(ASM)
	rm -f test.*
