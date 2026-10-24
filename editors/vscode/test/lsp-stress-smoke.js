const { spawn } = require("child_process");
const path = require("path");

const { resolveLspPath } = require("./resolve-lsp-path");
const lspPath = resolveLspPath();

const uri = "file:///C:/tmp/lsp-stress.phs";

function frame(obj) {
  const body = JSON.stringify(obj);
  return `Content-Length: ${Buffer.byteLength(body, "utf8")}\r\n\r\n${body}`;
}

async function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function main() {
  const proc = spawn(lspPath, [], { stdio: ["pipe", "pipe", "pipe"] });
  let exited = false;
  let exitCode = null;
  proc.on("exit", (code) => {
    exited = true;
    exitCode = code;
  });
  proc.stderr.on("data", (d) => process.stderr.write(d));

  proc.stdin.on("error", () => {
    exited = true;
  });

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

  const sequences = ["w", "wh", "while", "", "p", "pr", "print", 'print "merhaba"', "while", "if", "print"];
  let version = 1;
  let completionId = 100;

  for (let round = 0; round < 30 && !exited; round++) {
    for (const text of sequences) {
      if (exited) break;
      version++;
      if (exited) break;
      try {
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
      } catch {
        exited = true;
        break;
      }
      completionId++;
      if (exited) break;
      try {
        proc.stdin.write(
        frame({
          jsonrpc: "2.0",
          id: completionId,
          method: "textDocument/completion",
          params: {
            textDocument: { uri },
            position: { line: 0, character: Math.max(1, text.length) },
          },
        }),
      );
      } catch {
        exited = true;
        break;
      }
      await sleep(15);
    }
    await sleep(450);
  }

  await sleep(800);
  if (!exited) {
    proc.kill();
  }
  await sleep(200);

  if (exited && exitCode !== 0 && exitCode !== null) {
    console.error(`FAIL: LSP exited with code ${exitCode} (0x${(exitCode >>> 0).toString(16)})`);
    process.exit(1);
  }
  console.log("PASS  lsp-stress-smoke (process survived stress loop)");
  process.exit(0);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
