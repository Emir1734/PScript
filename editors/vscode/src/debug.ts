import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";

const DAP_COMPILE_EXE = process.platform === "win32" ? "pscript_dap_compile.exe" : "pscript_dap_compile";
const DAP_RUNTIME_EXE = process.platform === "win32" ? "pscript_dap.exe" : "pscript_dap";

function firstExisting(candidates: string[]): string | undefined {
  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }
  return undefined;
}

function newestDapBinary(workspaceRoot: string, config: "Debug" | "Release"): string | undefined {
  const parent = path.join(workspaceRoot, "build", config, DAP_COMPILE_EXE);
  const nested = path.join(workspaceRoot, "build", config, "Debug", DAP_COMPILE_EXE);
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

function workspaceDapCandidates(workspaceRoot: string, preferDebug: boolean): string[] {
  const debugCompile = newestDapBinary(workspaceRoot, "Debug");
  const debugCanonical = path.join(workspaceRoot, "build", "Debug", DAP_RUNTIME_EXE);
  const releaseCompile = newestDapBinary(workspaceRoot, "Release");
  const releaseCanonical = path.join(workspaceRoot, "build", "Release", DAP_RUNTIME_EXE);
  const debug = [debugCompile, debugCanonical].filter((p): p is string => Boolean(p));
  const release = [releaseCompile, releaseCanonical].filter((p): p is string => Boolean(p));
  return preferDebug ? [...debug, ...release] : [...release, ...debug];
}

function expandWorkspaceFolder(configured: string): string {
  const folder = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
  if (!folder) {
    return configured;
  }
  return configured.replace(/\$\{workspaceFolder\}/g, folder);
}

export function resolveDebugAdapterPath(): string {
  const fromEnv = process.env.PSCRIPT_DAP?.trim();
  if (fromEnv && fs.existsSync(fromEnv)) {
    return fromEnv;
  }
  const configuredRaw = vscode.workspace.getConfiguration("pscript").get<string>("debugAdapter.path", "");
  const configured = configuredRaw ? expandWorkspaceFolder(configuredRaw.trim()) : "";
  if (configured && fs.existsSync(configured)) {
    return configured;
  }
  const preferDebug = vscode.workspace
    .getConfiguration("pscript")
    .get<boolean>("languageServer.preferWorkspaceDebug", true);
  const folders = vscode.workspace.workspaceFolders;
  if (folders && folders.length > 0) {
    const found = firstExisting(workspaceDapCandidates(folders[0].uri.fsPath, preferDebug));
    if (found) {
      return found;
    }
  }
  const bundled = path.join(__dirname, "..", "server", DAP_RUNTIME_EXE);
  if (fs.existsSync(bundled)) {
    return bundled;
  }
  throw new Error(
    "pscript_dap not found. Build the C++ target pscript_dap or set pscript.debugAdapter.path.",
  );
}

export function registerDebugAdapterFactory(context: vscode.ExtensionContext): void {
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory("pscript", {
      createDebugAdapterDescriptor(_session: vscode.DebugSession) {
        const adapterPath = resolveDebugAdapterPath();
        return new vscode.DebugAdapterExecutable(adapterPath);
      },
    }),
  );
}
