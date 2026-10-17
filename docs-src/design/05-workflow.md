# Working on Iris

How features are added, tested and released. This applies to people and to agents.

## Adding a feature

1. Write the rules. The feature's page in the Language section must say exactly how it behaves. If a rule is not settled, it goes to the maintainer as a question with a suggested answer.
2. Write the tests: programs that use the feature, and programs that should fail to compile.
3. Implement the feature.
4. Run all the tests on the three platforms.
5. Commit on a branch and merge it.

Language design is decided by the maintainer. How the compiler is built inside is decided by whoever writes the code, who then says what they chose.

## Building

```shell
make              # builds ./iris
make iris-debug   # builds ./iris-debug with AddressSanitizer
make test         # runs the tests
make test-debug   # runs the tests with iris-debug
```

## Tests

Tests are `.iris` files in `tests/`. `python3 tools/run_tests.py` runs them all. Each folder holds one kind:

| Folder | What the test checks |
|---|---|
| `tests/parse/` | `iris parse` succeeds and prints the tree in the `.tree` file next to it. |
| `tests/errors/` | `iris parse` fails with the errors listed at the top of the file. |
| `tests/run/` | The program runs and prints what the comment at the top says. Starts in 0.2. |

An error test lists each expected error as a comment with its line number:

```iris error
// error: 4: this string has no closing quote

void main() {
    print("hello)
}
```

A run test says what the program prints:

```iris
// expect: 7
void main() {
    print("{3 + 4}")
}
```

Every `iris` code block in `docs-src/` must also parse. A block marked `iris error` is skipped.

After changing the parser on purpose, `python3 tools/run_tests.py --update` rewrites the `.tree` files. Check the diff before committing.

## Platforms

Every change is tested on:

- Linux x86_64
- Linux arm64
- macOS arm64

`tools/test-platforms.sh` builds Iris and runs the tests on all three at once, with the normal and the AddressSanitizer build.

## Git

- `main` always builds and passes the tests.
- One branch per feature or bug: `feat/...` or `fix/...`.
- Commit messages are one line, lowercase, starting with `feat:`, `fix:`, `docs:` or `test:`, and say what changed.
- Changes to the docs only go straight to `main`.
- The repository is private for now.

## Releases

A version from the [roadmap](../roadmap.html) is released when everything in it works and the tests pass on all three platforms.

## Writing the docs

The docs are Markdown files in `docs-src/`. `python3 tools/build_docs.py` builds the HTML into `docs/`. Commit both.

Write plainly. Say what a thing does, in short sentences. Don't sell it, don't compare it with other languages, and give every feature an example.
