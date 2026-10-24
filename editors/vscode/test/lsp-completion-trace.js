const assert = require("assert");
const { spawn } = require("child_process");
const path = require("path");

const { resolveLspPath } = require("./resolve-lsp-path");
const lspPath = resolveLspPath();

const uri = "file:///C:/tmp/lsp-trace.phs";

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
  let version = 1;

  function onData(chunk) {
    buffer = Buffer.concat([buffer, chunk]);
  }
  proc.stdout.on("data", onData);

  const waitResponse = (id) =>
    new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(`timeout ${id}`)), 3000);
      const tick = () => {
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
            return;
          }
        }
        setImmediate(tick);
      };
      tick();
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
  await sleep(40);

  async function complete(text, character) {
    version++;
    const id = nextId++;
    proc.stdin.write(
      frame({
        jsonrpc: "2.0",
        method: "textDocument/didChange",
        params: { textDocument: { uri, version }, contentChanges: [{ text }] },
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
    const printItem = (result.items || []).find((i) => i.label === "print");
    return {
      text,
      character,
      isIncomplete: result.isIncomplete,
      count: (result.items || []).length,
      printItem,
    };
  }

  const seq = [];
  for (const t of ["p", "pr", "pri", "pr", "p"]) {
    seq.push(await complete(t, t.length));
  }
  seq.push(await complete("x = pri + 10", "x = pri".length));
  seq.push(await complete("print(pri)", "print(pri".length));

  for (const step of seq) {
    assert.strictEqual(step.isIncomplete, false, `isIncomplete for ${step.text}`);
    assert.ok(step.printItem, `print item for ${step.text}`);
    assert.strictEqual(step.printItem.textEdit, undefined, `no textEdit for ${step.text}`);
    assert.strictEqual(step.printItem.filterText, "print", `filterText for ${step.text}`);
  }

  proc.kill();
  console.log("PASS  lsp-completion-trace");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
