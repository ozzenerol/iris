// Starts iris lsp for .iris files, and debugs Iris programs with gdb's debug adapter (gdb -i=dap).
const vscode = require("vscode");
const cp = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");
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

let buildOutput;

// Builds config.program (a .iris file or a project folder) with iris build and returns the binary's path.
function build(config) {
    const iris = vscode.workspace.getConfiguration("iris").get("path") || "iris";
    const src = config.program;
    const dir = path.join(os.tmpdir(), "iris-debug");
    fs.mkdirSync(dir, { recursive: true });
    const out = path.join(dir, path.basename(src, ".iris") || "main");
    const args = ["build", `--mode=${config.mode || "debug"}`, src, "-o", out];
    const cwd = fs.existsSync(src) && fs.statSync(src).isDirectory() ? src : path.dirname(src);
    buildOutput.clear();
    buildOutput.appendLine(`${iris} ${args.join(" ")}`);
    return new Promise((resolve) => {
        cp.execFile(iris, args, { cwd }, (err, stdout, stderr) => {
            if (stdout) buildOutput.append(stdout);
            if (stderr) buildOutput.append(stderr);
            if (err) {
                buildOutput.show(true);
                vscode.window.showErrorMessage(`Iris: the build of ${path.basename(src)} failed.`);
                resolve(undefined);
            } else {
                resolve(out);
            }
        });
    });
}

const debugConfigs = {
    // F5 with no launch.json debugs the open .iris file.
    resolveDebugConfiguration(folder, config) {
        if (!config.type && !config.request && !config.name) {
            const editor = vscode.window.activeTextEditor;
            if (!editor || editor.document.languageId !== "iris") return config;
            config.type = "iris";
            config.request = "launch";
            config.name = "Debug Iris";
            config.program = "${file}";
            config.cwd = folder ? folder.uri.fsPath : path.dirname(editor.document.fileName);
        }
        if (!config.program) {
            vscode.window.showErrorMessage("Iris: set program in launch.json to the .iris file or project folder to debug.");
            return undefined;
        }
        return config;
    },
    async resolveDebugConfigurationWithSubstitutedVariables(folder, config) {
        const bin = await build(config);
        if (!bin) return undefined;
        config.source = config.program;
        config.program = bin;
        if (!config.cwd) config.cwd = path.dirname(config.source);
        return config;
    },
};

const debugAdapters = {
    createDebugAdapterDescriptor() {
        const gdb = vscode.workspace.getConfiguration("iris").get("gdbPath") || "gdb";
        return new vscode.DebugAdapterExecutable(gdb, ["-q", "-i=dap"]);
    },
};

function activate(context) {
    buildOutput = vscode.window.createOutputChannel("Iris Build");
    context.subscriptions.push(buildOutput);
    context.subscriptions.push(vscode.debug.registerDebugConfigurationProvider("iris", debugConfigs));
    context.subscriptions.push(vscode.debug.registerDebugAdapterDescriptorFactory("iris", debugAdapters));
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
