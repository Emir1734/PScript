import * as crypto from "crypto";
import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  Trace,
  TransportKind,
} from "vscode-languageclient/node";
import { registerDebugAdapterFactory } from "./debug";

const COMPILE_EXE = process.platform === "win32" ? "pscript_lsp_compile.exe" : "pscript_lsp_compile";
const RUNTIME_EXE = process.platform === "win32" ? "pscript_lsp.exe" : "pscript_lsp";

let client: LanguageClient | undefined;
let outputChannel: vscode.OutputChannel | undefined;
let extensionContext: vscode.ExtensionContext | undefined;

function firstExisting(candidates: string[]): string | undefined {
  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }
  return undefined;
}

function newestCompileBinary(workspaceRoot: string, config: "Debug" | "Release"): string | undefined {
  const parent = path.join(workspaceRoot, "build", config, COMPILE_EXE);
  const nested = path.join(workspaceRoot, "build", config, "Debug", COMPILE_EXE);
  let best: string | undefined;
  let bestMtime = -1;
  for (const candidate of [parent, nested]) {
    if (!fs.existsSync(candidate)) {
      continue;
    }
    const mtime = fs.statSync(candidate).mtimeMs;
    if (mtime > bestMtime) {
      bestMtime = mtime;
      best = candidate;
    }
  }
  return best;
}

function workspaceLspBuildCandidates(workspaceRoot: string, preferDebug: boolean): string[] {
  const debugCompile = newestCompileBinary(workspaceRoot, "Debug");
  const debugCanonical = path.join(workspaceRoot, "build", "Debug", RUNTIME_EXE);
  const releaseCompile = newestCompileBinary(workspaceRoot, "Release");
  const releaseCanonical = path.join(workspaceRoot, "build", "Release", RUNTIME_EXE);
  const debug = [debugCompile, debugCanonical].filter((p): p is string => Boolean(p));
  const release = [releaseCompile, releaseCanonical].filter((p): p is string => Boolean(p));
  return preferDebug ? [...debug, ...release] : [...release, ...debug];
}

function findServerInWorkspace(workspaceRoot: string, preferDebug: boolean): string | undefined {
  return firstExisting(workspaceLspBuildCandidates(workspaceRoot, preferDebug));
}

function expandWorkspaceFolder(configured: string): string {
  const folder = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
  if (!folder) {
    return configured;
  }
  return configured.replace(/\$\{workspaceFolder\}/g, folder);
}

export function resolveBuildSourcePath(): string {
  const fromEnv = process.env.PSCRIPT_LSP?.trim();
  if (fromEnv && fs.existsSync(fromEnv)) {
    return fromEnv;
  }
  const configuredRaw = vscode.workspace
    .getConfiguration("pscript")
    .get<string>("languageServer.path", "");
  const configured = configuredRaw ? expandWorkspaceFolder(configuredRaw.trim()) : "";
  if (configured && fs.existsSync(configured)) {
    return configured;
  }
  const preferDebug = vscode.workspace
    .getConfiguration("pscript")
    .get<boolean>("languageServer.preferWorkspaceDebug", true);
  const folders = vscode.workspace.workspaceFolders;
  if (folders && folders.length > 0) {
    const found = findServerInWorkspace(folders[0].uri.fsPath, preferDebug);
    if (found) {
      return found;
    }
  }
  const bundled = path.join(__dirname, "..", "server", RUNTIME_EXE);
  if (fs.existsSync(bundled)) {
    return bundled;
  }
  throw new Error(
    "pscript_lsp not found. Build the C++ target pscript_lsp or set pscript.languageServer.path.",
  );
}

/** Pick runtime executable; prefers fresh build binary when globalStorage copy is stale or locked. */
export function chooseLanguageServerExecutable(
  context: vscode.ExtensionContext,
  buildSourcePath: string,
  log: vscode.OutputChannel,
): string {
  const useRuntimeCopy = vscode.workspace
    .getConfiguration("pscript")
    .get<boolean>("languageServer.useRuntimeCopy", true);
  if (!useRuntimeCopy) {
    log.appendLine(`Using build binary directly (pscript.languageServer.useRuntimeCopy=false).`);
    return buildSourcePath;
  }

  const runtimeDir = path.join(context.globalStorageUri.fsPath, "lsp-runtime");
  const destPath = path.join(runtimeDir, RUNTIME_EXE);
  fs.mkdirSync(runtimeDir, { recursive: true });
  const srcStat = fs.statSync(buildSourcePath);

  try {
    let needsCopy = true;
    if (fs.existsSync(destPath)) {
      const destStat = fs.statSync(destPath);
      needsCopy = destStat.mtimeMs < srcStat.mtimeMs || destStat.size !== srcStat.size;
    }
    if (needsCopy) {
      fs.copyFileSync(buildSourcePath, destPath);
      log.appendLine(`Updated LSP runtime copy from ${buildSourcePath}`);
    }
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    log.appendLine(`LSP runtime copy failed (${message}); using build binary for this session.`);
    return buildSourcePath;
  }

  if (fs.existsSync(destPath)) {
    const destStat = fs.statSync(destPath);
    if (destStat.mtimeMs < srcStat.mtimeMs || destStat.size !== srcStat.size) {
      log.appendLine(
        "LSP runtime copy is stale (build is newer). Using build binary. " +
          'Run "PScript: Restart Language Server" after rebuilding.',
      );
      return buildSourcePath;
    }
  }
  return destPath;
}

function appendServerFingerprint(log: vscode.OutputChannel, serverPath: string, buildSourcePath: string): void {
  log.appendLine(`Using language server: ${serverPath}`);
  if (buildSourcePath !== serverPath) {
    log.appendLine(`LSP build source: ${buildSourcePath}`);
  }
  try {
    const hash = crypto.createHash("sha256").update(fs.readFileSync(serverPath)).digest("hex");
    log.appendLine(`Language server SHA256: ${hash}`);
    if (buildSourcePath !== serverPath && fs.existsSync(buildSourcePath)) {
      const srcHash = crypto.createHash("sha256").update(fs.readFileSync(buildSourcePath)).digest("hex");
      if (hash !== srcHash) {
        log.appendLine(
          "WARNING: Running LSP binary differs from build source. Restart language server or reload window.",
        );
      }
    }
  } catch {
    log.appendLine("Language server SHA256: (unavailable)");
  }
}

async function activateIconTheme(): Promise<void> {
  const auto = vscode.workspace.getConfiguration("pscript").get<boolean>("autoSelectIconTheme", false);
  if (!auto) {
    return;
  }
  const workbench = vscode.workspace.getConfiguration("workbench");
  const current = workbench.get<string | null>("iconTheme");
  if (current && current.length > 0) {
    return;
  }
  await workbench.update("iconTheme", "pscript-icons", vscode.ConfigurationTarget.Global);
}

async function warnIfLanguageNotRegistered(context: vscode.ExtensionContext): Promise<void> {
  const languages = await vscode.languages.getLanguages();
  if (languages.includes("pscript")) {
    return;
  }
  if (context.globalState.get<boolean>("pscript.glassLanguageWarned")) {
    return;
  }
  await context.globalState.update("pscript.glassLanguageWarned", true);
  void vscode.window.showWarningMessage(
    "PScript language mode is not registered in this Cursor session (Glass UI). " +
      "Restart Cursor with --classic (or use VS Code) for .phs syntax and Change Language Mode.",
  );
}

async function startLanguageServer(context: vscode.ExtensionContext): Promise<void> {
  if (client) {
    await client.stop();
    client = undefined;
  }

  const log = outputChannel ?? vscode.window.createOutputChannel("PScript Language Server");
  outputChannel = log;

  let buildSourcePath: string;
  let serverPath: string;
  try {
    buildSourcePath = resolveBuildSourcePath();
    serverPath = chooseLanguageServerExecutable(context, buildSourcePath, log);
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    void vscode.window.showWarningMessage(
      `PScript LSP not started: ${message}. Syntax highlighting still works; build pscript_lsp or set pscript.languageServer.path.`,
    );
    return;
  }

  appendServerFingerprint(log, serverPath, buildSourcePath);

  const trace = vscode.workspace.getConfiguration("pscript").get<string>("languageServer.trace", "off");
  const logCompletion = vscode.workspace
    .getConfiguration("pscript")
    .get<boolean>("languageServer.logCompletion", false);
  const serverEnv: NodeJS.ProcessEnv = {
    ...process.env,
    PSCRIPT_LSP_LOG_COMPLETION: logCompletion ? "1" : "0",
  };

  const serverOptions: ServerOptions = {
    run: { command: serverPath, transport: TransportKind.stdio, options: { env: serverEnv } },
    debug: { command: serverPath, transport: TransportKind.stdio, options: { env: serverEnv } },
  };
  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: "file", pattern: "**/*.phs" }],
    outputChannel: log,
    traceOutputChannel: log,
    middleware: {
      handleDiagnostics: (uri, diagnostics, next) => {
        if (uri.scheme !== "file" || !uri.fsPath.toLowerCase().endsWith(".phs")) {
          return;
        }
        next(uri, diagnostics);
      },
      provideCompletionItem: async (document, position, ctx, token, next) => {
        const result = await next(document, position, ctx, token);
        const line = document.lineAt(position.line).text;
        const items = result
          ? Array.isArray(result)
            ? result
            : result.items
          : [];
        if (trace === "verbose" || logCompletion) {
          const summary = items
            .slice(0, 40)
            .map((item) => `${item.label}(kind=${item.kind})`)
            .join(", ");
          log.appendLine(
            `[completion] uri=${document.uri.toString()} v=${document.version} ` +
              `pos=${position.line}:${position.character} line=${JSON.stringify(line)} ` +
              `count=${items.length} hasMath=${items.some((i) => i.label === "math")} ` +
              `items=[${summary}${items.length > 40 ? ", …" : ""}]`,
          );
        }
        return result;
      },
    },
  };

  client = new LanguageClient("pscriptLanguageServer", "PScript Language Server", serverOptions, clientOptions);
  if (trace === "messages") {
    client.setTrace(Trace.Messages);
  } else if (trace === "verbose") {
    client.setTrace(Trace.Verbose);
  }
  await client.start();
  log.appendLine("Language server started.");
}

export function activate(context: vscode.ExtensionContext): void {
  extensionContext = context;
  void warnIfLanguageNotRegistered(context);
  void activateIconTheme();

  outputChannel = vscode.window.createOutputChannel("PScript Language Server");
  context.subscriptions.push(outputChannel);

  context.subscriptions.push(
    vscode.commands.registerCommand("pscript.restartLanguageServer", async () => {
      if (!extensionContext) {
        return;
      }
      await startLanguageServer(extensionContext);
      void vscode.window.showInformationMessage("PScript language server restarted.");
    }),
  );

  registerDebugAdapterFactory(context);
  void startLanguageServer(context);
  context.subscriptions.push({ dispose: () => client?.stop() });
}

export function deactivate(): Promise<void> | undefined {
  return client?.stop();
}
