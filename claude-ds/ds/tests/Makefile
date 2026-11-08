# Builds and runs the host tests (protocol, config and screen layouts) with
# the PC's own C compiler. No Nintendo DS toolchain needed.

CC      ?= cc
CFLAGS  ?= -std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
SRC     := ../source/proto.c ../source/ui.c ../source/config.c ../source/glyphs.c host_test.c

.PHONY: all clean

all: build/host_test
	./build/host_test build/screens.json > build/screens.txt
	@echo "screens written to tests/build/screens.txt"

build/host_test: $(SRC) $(wildcard ../source/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -I../source -o $@ $(SRC)

clean:
	rm -rf build
