const { spawn } = require("child_process");
const path = require("path");

const { resolveLspPath } = require("./resolve-lsp-path");
const lspPath = resolveLspPath();

function frame(obj) {
  const body = JSON.stringify(obj);
  return `Content-Length: ${Buffer.byteLength(body, "utf8")}\r\n\r\n${body}`;
}

async function readMessages(stream, onMessage, max = 5) {
  let buffer = Buffer.alloc(0);
  let count = 0;
  return new Promise((resolve) => {
    const finish = () => resolve(buffer.toString("utf8"));
    stream.on("data", (chunk) => {
      buffer = Buffer.concat([buffer, chunk]);
      while (true) {
        const headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) break;
        const header = buffer.slice(0, headerEnd).toString("utf8");
        const match = /Content-Length:\s*(\d+)/i.exec(header);
        if (!match) break;
        const len = Number(match[1]);
        const start = headerEnd + 4;
        if (buffer.length < start + len) break;
        const body = buffer.slice(start, start + len).toString("utf8");
        buffer = buffer.slice(start + len);
        onMessage(JSON.parse(body));
        count++;
        if (count >= max) return finish();
      }
    });
    stream.on("end", finish);
  });
}

async function main() {
  const proc = spawn(lspPath, [], { stdio: ["pipe", "pipe", "pipe"] });
  let completionResult = null;
  let initResult = null;

  proc.stdout.on("data", () => {});
  readMessages(proc.stdout, (msg) => {
    if (msg.id === 1) initResult = msg.result;
    if (msg.id === 2) completionResult = msg.result;
  }, 3);

  proc.stdin.write(
    frame({ jsonrpc: "2.0", id: 1, method: "initialize", params: { capabilities: {} } }),
  );
  await new Promise((r) => setTimeout(r, 100));
  proc.stdin.write(frame({ jsonrpc: "2.0", method: "initialized", params: {} }));
  proc.stdin.write(
    frame({
      jsonrpc: "2.0",
      method: "textDocument/didOpen",
      params: {
        textDocument: {
          uri: "file:///C:/tmp/t.phs",
          languageId: "pscript",
          version: 1,
          text: "w",
        },
      },
    }),
  );
  proc.stdin.write(
    frame({
      jsonrpc: "2.0",
      id: 2,
      method: "textDocument/completion",
      params: {
        textDocument: { uri: "file:///C:/tmp/t.phs" },
        position: { line: 0, character: 1 },
      },
    }),
  );
  await new Promise((r) => setTimeout(r, 800));
  proc.kill();

  const hasProvider = !!initResult?.capabilities?.completionProvider;
  const labels = (completionResult?.items || []).map((i) => i.label);
  const hasWhile = labels.includes("while");
  console.log(
    JSON.stringify(
      {
        lspPath,
        completionProvider: hasProvider,
        triggerCharacters: initResult?.capabilities?.completionProvider?.triggerCharacters,
        itemCount: labels.length,
        hasWhile,
        sampleLabels: labels.slice(0, 15),
        firstItem: completionResult?.items?.[0] || null,
      },
      null,
      2,
    ),
  );
  process.exit(hasProvider && hasWhile ? 0 : 1);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
