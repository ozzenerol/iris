CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter
SRC = $(wildcard src/*.c)
HDR = $(wildcard src/*.h)

iris: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC)

# AddressSanitizer and UBSan build, for finding memory bugs in the compiler
iris-debug: $(SRC) $(HDR)
	$(CC) -std=c11 -O0 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(SRC)

test: iris
	python3 tools/run_tests.py

test-debug: iris-debug
	python3 tools/run_tests.py --iris ./iris-debug

docs:
	python3 tools/build_docs.py

clean:
	rm -f iris iris-debug

.PHONY: test test-debug docs clean
