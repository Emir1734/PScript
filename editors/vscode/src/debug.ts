import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";
import {
  canonicalBinaryUnderRoots,
  cmakeBuildRoots,
  newestBinaryUnderRoots,
} from "./buildPaths";

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

function workspaceDapCandidates(workspaceRoot: string, preferDebug: boolean): string[] {
  const roots = cmakeBuildRoots(workspaceRoot);
  const debugCompile = newestBinaryUnderRoots(roots, "Debug", DAP_COMPILE_EXE);
  const debugCanonical = canonicalBinaryUnderRoots(roots, "Debug", DAP_RUNTIME_EXE);
  const releaseCompile = newestBinaryUnderRoots(roots, "Release", DAP_COMPILE_EXE);
  const releaseCanonical = canonicalBinaryUnderRoots(roots, "Release", DAP_RUNTIME_EXE);
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
