const assert = require("assert");
const { spawn } = require("child_process");
const path = require("path");

const { resolveLspPath } = require("./resolve-lsp-path");
const lspPath = resolveLspPath();

const uri = "file:///C:/tmp/lsp-diag-completion.phs";

function frame(obj) {
  const body = JSON.stringify(obj);
  return `Content-Length: ${Buffer.byteLength(body, "utf8")}\r\n\r\n${body}`;
}

async function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function main() {
  const proc = spawn(lspPath, [], { stdio: ["pipe", "pipe", "pipe"] });
  let buffer = Buffer.alloc(0);
  let nextId = 10;

  const waitResponse = (id) =>
    new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(`timeout id=${id}`)), 2000);
      const handler = () => {
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
            clearTimeout(timer);
            resolve(msg.result);
          }
        }
      };
      const onData = (chunk) => {
        buffer = Buffer.concat([buffer, chunk]);
        handler();
      };
      proc.stdout.on("data", onData);
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
  await sleep(50);

  async function complete(text, character) {
    const id = nextId++;
    proc.stdin.write(
      frame({
        jsonrpc: "2.0",
        method: "textDocument/didChange",
        params: {
          textDocument: { uri, version: nextId },
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
    const result = await waitResponse(id);
    const items = result?.items || [];
    const labels = items.map((i) => i.label);
    const sample = items.find((i) =>
      ["print", "int", "catch", "while", "func"].includes(i.label),
    );
    return {
      text,
      character,
      count: items.length,
      isIncomplete: result?.isIncomplete,
      labels: labels.slice(0, 25),
      hasPrint: labels.includes("print"),
      hasInt: labels.includes("int"),
      hasCatch: labels.includes("catch"),
      hasWhile: labels.includes("while"),
      hasFunc: labels.includes("func"),
      sampleTextEdit: sample?.textEdit || null,
    };
  }

  const prefixes = ["p", "pr", "pri", "i", "in", "c", "ca", "w", "wh", "f", "fu"];
  const forward = [];
  for (const t of prefixes) {
    forward.push(await complete(t, t.length));
  }

  const backspaceSeq = [];
  for (const t of ["pri", "pr", "p"]) {
    backspaceSeq.push(await complete(t, t.length));
  }

  for (const row of forward) {
    assert.strictEqual(row.isIncomplete, false, `isIncomplete ${row.text}`);
    if (row.text === "p" || row.text === "pr" || row.text === "pri") {
      assert.ok(row.hasPrint, `print for ${row.text}`);
    }
    if (row.text === "i" || row.text === "in") {
      assert.ok(row.hasInt, `int for ${row.text}`);
    }
    if (row.text === "c" || row.text === "ca") {
      assert.ok(row.hasCatch, `catch for ${row.text}`);
    }
    if (row.text === "w" || row.text === "wh") {
      assert.ok(row.hasWhile, `while for ${row.text}`);
    }
    if (row.text === "f" || row.text === "fu") {
      assert.ok(row.hasFunc, `func for ${row.text}`);
    }
  }
  for (const row of backspaceSeq) {
    assert.strictEqual(row.isIncomplete, false);
    assert.ok(row.hasPrint, `print after backspace to ${row.text}`);
  }

  proc.kill();
  console.log("PASS  lsp-completion-diagnostic");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
