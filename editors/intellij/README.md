# Iris for IntelliJ

A plugin for IntelliJ IDEA 2026.2 and other JetBrains IDEs on the same platform (GoLand, CLion...).

- Highlights `.iris` files, indents with 4 spaces, comments lines with Ctrl+/, matches braces and quotes.
- Runs `iris lsp` for errors, hover, go to definition, completion and formatting.
- Adds an Iris run configuration and a run button beside `main`, every `test` and every `bench`.
- Turns `file.iris:LINE:COL` in compiler errors and panics into links.

## Build and install

```
make intellij
```

This compiles the plugin against the IDE in `~/.local/share/JetBrains/Toolbox/apps/intellij-idea`
(set `IDE=/path/to/ide` for another one) and writes `editors/intellij/iris-intellij.jar`. Install
it with Settings, Plugins, the gear, Install Plugin from Disk, then restart the IDE.

## Running programs

Click the run button beside `void main()` (or `!void main()`), a `test "name"` or a `bench "name"`.
The IDE makes a run configuration for it and runs it, and Shift+F10 runs it again. Right-clicking a
`.iris` file runs it too: `iris run` when it has `main`, `iris test` when it does not.

An Iris run configuration (Run, Edit Configurations, +, Iris) has:

- Command: run, test, bench, build or check.
- File or project: a `.iris` file or a folder with `iris.mod`.
- Test or bench name: only the blocks whose name has this.
- Mode: debug, release-safe or release-fast.
- iris options: more options before the file, such as `-o name`, `--cpu=native` or `--time=2s`.
- Program arguments: given to the program by `iris run`.
- Working directory: the folder of the file when empty.
- Environment variables.

## Settings

Settings, Languages & Frameworks, Iris sets the iris program. When it is empty the plugin uses the
`iris` on PATH, then `/usr/local/bin/iris`.
