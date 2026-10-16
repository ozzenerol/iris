CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter
SRC = $(wildcard src/*.c)
HDR = $(wildcard src/*.h)
STD = $(sort $(wildcard std/*/*.iris))

iris: $(SRC) $(HDR) build/runtime.inc build/std.inc
	$(CC) $(CFLAGS) -Ibuild -o $@ $(SRC) -lm

# AddressSanitizer and UBSan build, for finding memory bugs in the compiler
iris-debug: $(SRC) $(HDR) build/runtime.inc build/std.inc
	$(CC) -std=c11 -O0 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer -Ibuild -o $@ $(SRC) -lm

# the standard library's files, as bytes the compiler carries with it
build/std.inc: $(STD)
	mkdir -p build
	{ i=0; for f in $(STD); do \
	    echo "static const char std_$$i[] = {"; \
	    od -An -v -tx1 $$f | sed -e 's/\([0-9a-f][0-9a-f]\)/0x\1,/g'; \
	    echo "0};"; i=$$((i+1)); done; \
	  echo "static const struct { const char *path; const char *data; } stdFiles[] = {"; \
	  i=0; for f in $(STD); do echo "    {\"$${f#std/}\", std_$$i},"; i=$$((i+1)); done; \
	  echo "};"; } > $@

# the runtime's source, as bytes the compiler carries with it
build/runtime.inc: runtime/rt.c
	mkdir -p build
	od -An -v -tx1 runtime/rt.c | sed -e 's/\([0-9a-f][0-9a-f]\)/0x\1,/g' > $@

test: iris
	python3 tools/run_tests.py

test-debug: iris-debug
	python3 tools/run_tests.py --iris ./iris-debug

docs:
	python3 tools/build_docs.py

clean:
	rm -rf iris iris-debug build

.PHONY: test test-debug docs clean
