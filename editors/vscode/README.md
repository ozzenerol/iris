# Iris for VS Code

Highlights `.iris` files and the `iris` code blocks in Markdown. It starts `iris lsp`, which gives errors as you type, the type and comment of a name on hover, go to definition, the outline, completion and formatting.

The `iris` program has to be on your PATH. Set `iris.path` to use another one.

## Install

```shell
make vscode
code --install-extension editors/vscode/iris.vsix
```

`make vscode` needs Node.js and npm.
