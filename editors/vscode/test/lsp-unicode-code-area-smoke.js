const { spawn } = require("child_process");
const { resolveLspPath } = require("./resolve-lsp-path");

const lspPath = resolveLspPath();

function frame(obj) {
  const body = JSON.stringify(obj);
  return `Content-Length: ${Buffer.byteLength(body, "utf8")}\r\n\r\n${body}`;
}

async function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function exerciseChar(ch) {
  const uri = `file:///C:/tmp/lsp-unicode-${ch.codePointAt(0)}.phs`;
  const proc = spawn(lspPath, [], { stdio: ["pipe", "pipe", "pipe"] });
  let exitCode = null;
  proc.on("exit", (code) => {
    exitCode = code;
  });
  proc.stderr.on("data", (d) => process.stderr.write(d));

  proc.stdin.write(
    frame({ jsonrpc: "2.0", id: 1, method: "initialize", params: { capabilities: {} } }),
  );
  await sleep(80);
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

  proc.stdin.write(
    frame({
      jsonrpc: "2.0",
      method: "textDocument/didChange",
      params: {
        textDocument: { uri, version: 2 },
        contentChanges: [{ text: ch }],
      },
    }),
  );
  await sleep(500);
  proc.stdin.write(
    frame({
      jsonrpc: "2.0",
      id: 2,
      method: "textDocument/completion",
      params: {
        textDocument: { uri },
        position: { line: 0, character: 1 },
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
  await sleep(500);
  if (exitCode !== null && exitCode !== 0) {
    throw new Error(`LSP exited with code ${exitCode} for character ${ch}`);
  }
  proc.kill();
}

async function main() {
  await exerciseChar("ç");
  await exerciseChar("ö");
  await exerciseChar('print("ç")\n');
  await exerciseChar('print("ö")\n');
  await exerciseChar('print("hello\n');
  console.log("PASS  lsp-unicode-code-area-smoke");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
