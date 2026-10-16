CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter
SRC = $(wildcard src/*.c)
HDR = $(wildcard src/*.h)

iris: $(SRC) $(HDR) build/runtime.inc
	$(CC) $(CFLAGS) -Ibuild -o $@ $(SRC)

# AddressSanitizer and UBSan build, for finding memory bugs in the compiler
iris-debug: $(SRC) $(HDR) build/runtime.inc
	$(CC) -std=c11 -O0 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer -Ibuild -o $@ $(SRC)

# the runtime's source, as a C string the compiler carries with it
build/runtime.inc: runtime/rt.c
	mkdir -p build
	sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' runtime/rt.c > $@

test: iris
	python3 tools/run_tests.py

test-debug: iris-debug
	python3 tools/run_tests.py --iris ./iris-debug

docs:
	python3 tools/build_docs.py

clean:
	rm -rf iris iris-debug build

.PHONY: test test-debug docs clean
