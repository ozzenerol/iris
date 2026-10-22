// Starts iris lsp for .iris files.
const vscode = require("vscode");
const { LanguageClient } = require("vscode-languageclient/node");

let client;

function start() {
    const cmd = vscode.workspace.getConfiguration("iris").get("path") || "iris";
    client = new LanguageClient(
        "iris",
        "Iris",
        { command: cmd, args: ["lsp"] },
        { documentSelector: [{ scheme: "file", language: "iris" }, { scheme: "untitled", language: "iris" }] },
    );
    return client.start().catch((e) => {
        vscode.window.showErrorMessage(`Iris: couldn't start "${cmd} lsp": ${e.message}. Set iris.path to the iris program.`);
    });
}

async function restart() {
    if (client) await client.stop().catch(() => {});
    await start();
}

function activate(context) {
    context.subscriptions.push(vscode.commands.registerCommand("iris.restartServer", restart));
    context.subscriptions.push(
        vscode.workspace.onDidChangeConfiguration((e) => {
            if (e.affectsConfiguration("iris.path")) restart();
        }),
    );
    return start();
}

function deactivate() {
    return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
