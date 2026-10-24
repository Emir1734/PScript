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
  const uri = "file:///C:/tmp/lsp-incomplete-string.phs";
  const text = 'print("hello';
  const proc = spawn(lspPath, [], { stdio: ["pipe", "pipe", "pipe"] });
  let crashed = false;
  proc.on("exit", (code) => {
    if (code !== 0 && code !== null) {
      crashed = true;
      console.error(`LSP exited with code ${code}`);
    }
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
      method: "textDocument/completion",
      params: {
        textDocument: { uri },
        position: { line: 0, character: text.length },
      },
    }),
  );
  await sleep(600);
  if (crashed) {
    process.exit(1);
  }
  proc.kill();
  console.log("PASS  lsp-incomplete-string-completion-smoke");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
