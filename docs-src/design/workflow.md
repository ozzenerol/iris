# Working on Iris

How features are added, tested and released. This applies to people and to agents.

## Adding a feature

1. Write the rules. The feature's page in the Language section must say exactly how it behaves. If a rule is not settled, it goes to the maintainer as a question with a suggested answer.
2. Write the tests: programs that use the feature, and programs that should fail to compile.
3. Implement the feature.
4. Run all the tests on the three platforms.
5. Commit on a branch and merge it.

Language design is decided by the maintainer. How the compiler is built inside is decided by whoever writes the code, who then says what they chose.

## Tests

A test is an `.iris` file in `tests/`. A comment at the top says what should happen.

A program that runs and prints:

```iris
// expect: 7
void main() {
    print("{3 + 4}")
}
```

A program that must not compile:

```iris
// error: 3: x is not declared
void main() {
    print("{x}")
}
```

The test runner compiles each file, runs it, and compares the output or the error with the comment. Every example in the Language section is also run as a test.

## Platforms

Every change is tested on:

- Linux x86_64
- Linux arm64
- macOS arm64

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
