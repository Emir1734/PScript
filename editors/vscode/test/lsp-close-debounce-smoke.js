const assert = require("assert");
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
  const uri = "file:///C:/tmp/lsp-close-debounce.phs";
  const proc = spawn(lspPath, [], { stdio: ["pipe", "pipe", "pipe"] });
  let exited = false;
  let exitCode = null;
  proc.on("exit", (code) => {
    exited = true;
    exitCode = code;
  });
  proc.stderr.on("data", (d) => process.stderr.write(d));

  proc.stdin.write(
    frame({ jsonrpc: "2.0", id: 1, method: "initialize", params: { capabilities: {} } }),
  );
  await sleep(80);
  proc.stdin.write(frame({ jsonrpc: "2.0", method: "initialized", params: {} }));

  let id = 10;
  for (let round = 0; round < 40 && !exited; round++) {
    const text =
      round % 3 === 0
        ? "async func main():\n    await delay(500ms)\n"
        : round % 3 === 1
          ? "func f(x: str):\n    print(x)\n"
          : "var n: int = 1\nprint(n)\n";
    proc.stdin.write(
      frame({
        jsonrpc: "2.0",
        method: "textDocument/didOpen",
        params: {
          textDocument: { uri, languageId: "pscript", version: 1, text },
        },
      }),
    );
    for (let v = 2; v <= 8 && !exited; v++) {
      proc.stdin.write(
        frame({
          jsonrpc: "2.0",
          method: "textDocument/didChange",
          params: {
            textDocument: { uri, version: v },
            contentChanges: [{ text: text + "# edit\n" }],
          },
        }),
      );
      const cid = id++;
      proc.stdin.write(
        frame({
          jsonrpc: "2.0",
          id: cid,
          method: "textDocument/completion",
          params: {
            textDocument: { uri },
            position: { line: 0, character: 2 },
          },
        }),
      );
      await sleep(5);
    }
    proc.stdin.write(
      frame({
        jsonrpc: "2.0",
        method: "textDocument/didClose",
        params: { textDocument: { uri } },
      }),
    );
    await sleep(500);
  }

  await sleep(600);
  if (!exited) {
    proc.stdin.write(frame({ jsonrpc: "2.0", id: 99, method: "shutdown", params: {} }));
    await sleep(50);
    proc.stdin.write(frame({ jsonrpc: "2.0", method: "exit", params: {} }));
    await sleep(200);
  }

  if (exited && exitCode !== 0 && exitCode !== null) {
    console.error(`FAIL: LSP exited with code ${exitCode}`);
    process.exit(1);
  }
  console.log("PASS  lsp-close-debounce-smoke");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
