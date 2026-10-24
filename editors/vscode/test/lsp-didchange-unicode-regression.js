/**
 * Regression: VS Code-style didChange with only "ç" / "ö" must not kill pscript_lsp.
 * Uses build/Debug/pscript_lsp.exe explicitly (not stale Release paths).
 */
const { spawn } = require("child_process");
const fs = require("fs");
const path = require("path");

const repoRoot = path.resolve(__dirname, "../../..");
const lspPath = process.env.PSCRIPT_LSP || path.join(repoRoot, "build", "Debug", "pscript_lsp.exe");

if (!fs.existsSync(lspPath)) {
  console.error(`FAIL  lsp-didchange-unicode-regression: missing ${lspPath}`);
  process.exit(1);
}

function frame(obj) {
  const body = JSON.stringify(obj);
  return `Content-Length: ${Buffer.byteLength(body, "utf8")}\r\n\r\n${body}`;
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function runSequence(label, documentText) {
  const uri = "file:///c:/temp/pscript_unicode_regression.phs";
  const proc = spawn(lspPath, [], { stdio: ["pipe", "pipe", "pipe"] });
  let exitCode = null;
  proc.on("exit", (code) => {
    exitCode = code;
  });
  proc.stderr.on("data", (d) => process.stderr.write(`[${label} stderr] ${d}`));

  proc.stdin.write(
    frame({
      jsonrpc: "2.0",
      id: 1,
      method: "initialize",
      params: {
        processId: null,
        rootUri: null,
        capabilities: {
          textDocument: {
            diagnostic: {},
            publishDiagnostics: {},
          },
        },
      },
    }),
  );
  await sleep(100);
  proc.stdin.write(frame({ jsonrpc: "2.0", method: "initialized", params: {} }));
  proc.stdin.write(
    frame({
      jsonrpc: "2.0",
      method: "textDocument/didOpen",
      params: {
        textDocument: { uri, languageId: "pscript", version: 1, text: "" },
      },
    }),
  );
  await sleep(50);
  proc.stdin.write(
    frame({
      jsonrpc: "2.0",
      method: "textDocument/didChange",
      params: {
        textDocument: { uri, version: 2 },
        contentChanges: [
          {
            range: {
              start: { line: 0, character: 0 },
              end: { line: 0, character: 0 },
            },
            rangeLength: 0,
            text: documentText,
          },
        ],
      },
    }),
  );
  await sleep(600);
  proc.stdin.write(
    frame({
      jsonrpc: "2.0",
      id: 2,
      method: "textDocument/completion",
      params: {
        textDocument: { uri },
        position: { line: 0, character: Math.max(0, [...documentText].length) },
      },
    }),
  );
  proc.stdin.write(
    frame({
      jsonrpc: "2.0",
      id: 3,
      method: "textDocument/semanticTokens/full",
      params: { textDocument: { uri } },
    }),
  );
  await sleep(600);
  if (exitCode !== null && exitCode !== 0) {
    throw new Error(`${label}: server exited with code ${exitCode}`);
  }
  proc.kill();
}

async function main() {
  await runSequence("ç", "ç");
  await runSequence("ö", "ö");
  await runSequence('print("ç")', 'print("ç")\n');
  await runSequence('print("ö")', 'print("ö")\n');
  await runSequence("print(", "print(");
  console.log(`PASS  lsp-didchange-unicode-regression (${lspPath})`);
}

main().catch((e) => {
  console.error(String(e));
  process.exit(1);
});
