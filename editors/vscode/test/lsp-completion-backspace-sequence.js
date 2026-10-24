const assert = require("assert");
const { spawn } = require("child_process");
const path = require("path");

const { resolveLspPath } = require("./resolve-lsp-path");
const lspPath = resolveLspPath();

const uri = "file:///C:/tmp/lsp-backspace-seq.phs";

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
  let version = 1;
  let nextId = 10;

  proc.stdout.on("data", (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
  });

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

  async function complete(text, character, triggerKind = 1) {
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
          context: { triggerKind },
        },
      }),
    );
    return waitResponse(id);
  }

  const steps = [
    { text: "p", character: 1 },
    { text: "pr", character: 2 },
    { text: "pri", character: 3 },
    { text: "pr", character: 2, triggerKind: 3 },
    { text: "p", character: 1, triggerKind: 3 },
  ];

  for (const step of steps) {
    const result = await complete(step.text, step.character, step.triggerKind ?? 1);
    const labels = (result.items || []).map((i) => i.label);
    assert.strictEqual(result.isIncomplete, false, `isIncomplete ${step.text}`);
    assert.ok(labels.includes("print"), `${step.text} missing print: ${labels.slice(0, 8).join(",")}`);
    const printItem = result.items.find((i) => i.label === "print");
    assert.strictEqual(printItem.textEdit, undefined, `textEdit must be omitted for ${step.text}`);
    assert.strictEqual(printItem.filterText, "print", `filterText for ${step.text}`);
  }

  for (const [text, expect] of [
    ["i", "int"],
    ["c", "catch"],
    ["w", "while"],
    ["f", "func"],
    ["s", "string"],
    ["st", "str"],
  ]) {
    const result = await complete(text, text.length);
    const labels = (result.items || []).map((i) => i.label);
    assert.ok(labels.includes(expect), `${text} missing ${expect}`);
  }

  proc.kill();
  console.log("PASS  lsp-completion-backspace-sequence");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
