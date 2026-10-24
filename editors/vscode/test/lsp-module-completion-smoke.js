const { spawn } = require("child_process");
const path = require("path");

const { resolveLspPath } = require("./resolve-lsp-path");

const lspPath = resolveLspPath();

const uri = "file:///C:/tmp/lsp-module-completion.phs";

function frame(obj) {
  const body = JSON.stringify(obj);
  return `Content-Length: ${Buffer.byteLength(body, "utf8")}\r\n\r\n${body}`;
}

async function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function completionLabels(proc, text, line, character) {
  return new Promise((resolve, reject) => {
    let buffer = Buffer.alloc(0);
    const id = Math.floor(Math.random() * 1e6) + 4000;
    const onData = (chunk) => {
      buffer = Buffer.concat([buffer, chunk]);
      while (true) {
        const headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) break;
        const match = /Content-Length:\s*(\d+)/i.exec(buffer.slice(0, headerEnd).toString());
        if (!match) break;
        const len = Number(match[1]);
        const start = headerEnd + 4;
        if (buffer.length < start + len) break;
        const msg = JSON.parse(buffer.slice(start, start + len).toString());
        buffer = buffer.slice(start + len);
        if (msg.id === id) {
          proc.stdout.off("data", onData);
          if (!msg.result) {
            reject(new Error(`completion null result: ${JSON.stringify(msg.error || msg)}`));
            return;
          }
          resolve((msg.result.items || []).map((i) => i.label));
        }
      }
    };
    proc.stdout.on("data", onData);
    proc.stdin.write(
      frame({
        jsonrpc: "2.0",
        method: "textDocument/didChange",
        params: {
          textDocument: { uri, version: Date.now() },
          contentChanges: [{ text }],
        },
      }),
    );
    setTimeout(() => {
    proc.stdin.write(
      frame({
        jsonrpc: "2.0",
        id,
        method: "textDocument/completion",
        params: {
          textDocument: { uri },
          position: { line, character },
        },
      }),
    );
    }, 150);
    setTimeout(() => {
      proc.stdout.off("data", onData);
      reject(new Error("completion timeout"));
    }, 2000);
  });
}

async function main() {
  const proc = spawn(lspPath, [], { stdio: ["pipe", "pipe", "pipe"] });
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
        textDocument: { uri, languageId: "pscript", version: 1, text: "import ma\n" },
      },
    }),
  );
  await sleep(120);

  const importLabels = await completionLabels(proc, "import ma", 0, 9);
  if (!importLabels.includes("math")) {
    console.error("FAIL  import ma: expected math in", importLabels.slice(0, 12));
    proc.kill();
    process.exit(1);
  }

  const mathDotSource = "import math\nmath.\n";
  const memberLabels = await completionLabels(proc, mathDotSource, 1, 5);
  const expected = ["sqrt", "pow", "floor"];
  const missing = expected.filter((name) => !memberLabels.includes(name));
  if (missing.length > 0) {
    console.error("FAIL  math.: missing exports", missing, "got", memberLabels.slice(0, 20));
    proc.kill();
    process.exit(1);
  }
  if (memberLabels.includes("match")) {
    console.error("FAIL  math.: should not include match keyword");
    proc.kill();
    process.exit(1);
  }

  proc.kill();
  console.log("PASS  lsp-module-completion-smoke.js");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
