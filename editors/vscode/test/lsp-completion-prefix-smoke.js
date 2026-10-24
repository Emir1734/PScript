const { spawn } = require("child_process");
const path = require("path");

const { resolveLspPath } = require("./resolve-lsp-path");
const lspPath = resolveLspPath();

const uri = "file:///C:/tmp/lsp-completion-prefix.phs";

function frame(obj) {
  const body = JSON.stringify(obj);
  return `Content-Length: ${Buffer.byteLength(body, "utf8")}\r\n\r\n${body}`;
}

async function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function completionLabels(proc, text, character) {
  return new Promise((resolve, reject) => {
    let buffer = Buffer.alloc(0);
    const id = Math.floor(Math.random() * 1e6) + 2000;
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
          resolve((msg.result?.items || []).map((i) => i.label));
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
    proc.stdin.write(
      frame({
        jsonrpc: "2.0",
        id,
        method: "textDocument/completion",
        params: {
          textDocument: { uri },
          position: { line: 0, character },
        },
      }),
    );
    setTimeout(() => {
      proc.stdout.off("data", onData);
      reject(new Error(`completion timeout for ${JSON.stringify(text)}`));
    }, 1500);
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
        textDocument: { uri, languageId: "pscript", version: 1, text: "" },
      },
    }),
  );
  await sleep(50);

  const cases = [
    { text: "p", character: 1, expect: "print" },
    { text: "pr", character: 2, expect: "print" },
    { text: "pri", character: 3, expect: "print" },
    { text: "i", character: 1, expect: "int" },
    { text: "in", character: 2, expect: "int" },
    { text: "int", character: 3, expect: "int" },
  ];

  for (const testCase of cases) {
    const labels = await completionLabels(proc, testCase.text, testCase.character);
    if (!labels.includes(testCase.expect)) {
      console.error(`FAIL ${testCase.text} -> expected ${testCase.expect}, got ${labels.slice(0, 12).join(",")}`);
      proc.kill();
      process.exit(1);
    }
  }

  proc.kill();
  console.log("PASS  lsp-completion-prefix-smoke");
  process.exit(0);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
