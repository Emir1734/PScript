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
  const uri = "file:///C:/tmp/lsp-semantic-lexer.phs";
  const text = 'print("hello\n';
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
        textDocument: { uri, languageId: "pscript", version: 1, text },
      },
    }),
  );
  await sleep(100);
  proc.stdin.write(
    frame({
      jsonrpc: "2.0",
      id: 2,
      method: "textDocument/semanticTokens/full",
      params: { textDocument: { uri } },
    }),
  );
  await sleep(400);
  if (exitCode !== null && exitCode !== 0) {
    console.error(`FAIL: LSP crashed with exit code ${exitCode}`);
    process.exit(1);
  }
  proc.kill();
  await sleep(100);
  if (exitCode !== null && exitCode !== 0) {
    console.error(`FAIL: LSP crashed with exit code ${exitCode}`);
    process.exit(1);
  }
  console.log("PASS  lsp-semantic-lexer-smoke");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
