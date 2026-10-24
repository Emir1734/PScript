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
  const proc = spawn(lspPath, [], { stdio: ["pipe", "pipe", "pipe"] });
  let buffer = Buffer.alloc(0);
  const published = [];
  let nextId = 10;

  function drain(id, onNotification) {
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
        return msg.result;
      }
      if (msg.method && onNotification) {
        onNotification(msg);
      }
    }
    return undefined;
  }

  proc.stdout.on("data", (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
  });

  const waitResponse = (id, onNotification) =>
    new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(`timeout ${id}`)), 3000);
      const tick = () => {
        const result = drain(id, onNotification);
        if (result !== undefined) {
          clearTimeout(timer);
          resolve(result);
          return;
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
        textDocument: { uri: "/", languageId: "pscript", version: 1, text: "foo/" },
      },
    }),
  );
  await sleep(500);

  const phsUri = "file:///C:/tmp/sample.phs";
  proc.stdin.write(
    frame({
      jsonrpc: "2.0",
      method: "textDocument/didOpen",
      params: {
        textDocument: { uri: phsUri, languageId: "pscript", version: 1, text: "print 1\n" },
      },
    }),
  );
  await sleep(500);

  const onNotification = (msg) => {
    if (msg.method === "textDocument/publishDiagnostics") {
      published.push(msg.params);
    }
  };

  const completionId = nextId++;
  proc.stdin.write(
    frame({
      jsonrpc: "2.0",
      id: completionId,
      method: "textDocument/completion",
      params: {
        textDocument: { uri: phsUri },
        position: { line: 0, character: 1 },
      },
    }),
  );
  const completion = await waitResponse(completionId, onNotification);

  proc.kill();
  for (const params of published) {
    assert.notStrictEqual(params?.uri, "/", "must not publish diagnostics for uri /");
    assert.ok(
      typeof params?.uri === "string" && params.uri.toLowerCase().includes(".phs"),
      `publishDiagnostics uri must be a .phs file, got ${params?.uri}`,
    );
  }
  assert.ok((completion?.items || []).some((i) => i.label === "print"), "phs document tracked for LSP");
  console.log("PASS  lsp-diag-uri-smoke");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
