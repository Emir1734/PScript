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

async function main() {
  const uri = "file:///C:/tmp/lsp-invalid-char.phs";
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

  const edits = ["ç", "pr", "print", "async f", "await "];
  let version = 1;
  let id = 10;
  for (const text of edits) {
    version++;
    proc.stdin.write(
      frame({
        jsonrpc: "2.0",
        method: "textDocument/didChange",
        params: {
          textDocument: { uri, version },
          contentChanges: [{ text }],
        },
      }),
    );
    id++;
    proc.stdin.write(
      frame({
        jsonrpc: "2.0",
        id,
        method: "textDocument/completion",
        params: {
          textDocument: { uri },
          position: { line: 0, character: Math.max(0, text.length) },
        },
      }),
    );
    await sleep(30);
  }
  await sleep(600);
  if (exitCode !== null && exitCode !== 0) {
    console.error(`FAIL: LSP exited with code ${exitCode}`);
    process.exit(1);
  }
  proc.kill();
  console.log("PASS  lsp-invalid-char-smoke");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
