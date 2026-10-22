# Iris for VS Code

Highlights `.iris` files and the `iris` code blocks in Markdown. It starts `iris lsp`, which gives errors as you type, the type and comment of a name on hover, go to definition, the outline, completion and formatting. It also debugs Iris programs with gdb.

The `iris` program has to be on your PATH. Set `iris.path` to use another one.

## Install

```shell
make vscode
code --install-extension editors/vscode/iris.vsix
```

`make vscode` needs Node.js and npm.

## Debugging

Open a `.iris` file, set a breakpoint and press F5. The extension builds the file with `iris build` into a temporary folder and runs it under gdb's debug adapter (`gdb -i=dap`), so it needs gdb 14 or newer. Set `iris.gdbPath` to use another gdb.

To keep the settings, add a `launch.json` entry:

```json
{
  "type": "iris",
  "request": "launch",
  "name": "Debug Iris",
  "program": "${workspaceFolder}/main.iris",
  "args": [],
  "cwd": "${workspaceFolder}",
  "env": {},
  "mode": "debug"
}
```

`program` is a `.iris` file or a project folder. `mode` is `debug` (the default), `release-safe` or `release-fast`. If the build fails, its errors are in the Iris Build output panel.

The call stack shows a function by its module and name, such as `f.main.handle`.
