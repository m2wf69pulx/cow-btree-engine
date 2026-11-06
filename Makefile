CC ?= cc
CFLAGS ?= -std=c99 -O2 -g -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=

SRC := src/page.c src/tree.c src/main.c
OBJ := $(SRC:.c=.o)
BIN := build/cowbt

all: $(BIN)

$(BIN): $(OBJ) | build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

build:
	mkdir -p build

%.o: %.c include/cowbt.h | build
	$(CC) $(CFLAGS) -Iinclude -c -o $@ $<

check: $(BIN)
	rm -f build/selftest.db
	./$(BIN) selftest build/selftest.db
	rm -f build/crash.db
	./$(BIN) crash build/crash.db 6 4

clean:
	rm -rf build

.PHONY: all check clean
