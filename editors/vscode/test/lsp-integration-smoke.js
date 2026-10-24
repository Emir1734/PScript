const { spawn } = require("child_process");
const path = require("path");

const { resolveLspPath } = require("./resolve-lsp-path");
const lspPath = resolveLspPath();

const uri = "file:///C:/tmp/lsp-integration.phs";

function frame(obj) {
  const body = JSON.stringify(obj);
  return `Content-Length: ${Buffer.byteLength(body, "utf8")}\r\n\r\n${body}`;
}

function createClient() {
  const proc = spawn(lspPath, [], { stdio: ["pipe", "pipe", "pipe"] });
  let buffer = Buffer.alloc(0);
  const diagsByVersion = [];
  let initResult = null;
  const completionById = new Map();

  proc.stdout.on("data", (chunk) => {
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
      const msg = JSON.parse(body);
      if (msg.id === 1) initResult = msg.result;
      if (msg.id >= 100 && msg.result?.items) {
        completionById.set(msg.id, msg.result);
      }
      if (msg.method === "textDocument/publishDiagnostics") {
        diagsByVersion.push({
          version: msg.params?.version,
          count: (msg.params?.diagnostics || []).length,
          messages: (msg.params?.diagnostics || []).map((d) => d.message),
        });
      }
    }
  });

  return {
    proc,
    send: (obj) => proc.stdin.write(frame(obj)),
    getInit: () => initResult,
    getCompletion: (id) => completionById.get(id),
    getDiags: () => diagsByVersion,
    close: () => proc.kill(),
  };
}

async function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function main() {
  const client = createClient();
  client.send({ jsonrpc: "2.0", id: 1, method: "initialize", params: { capabilities: {} } });
  await sleep(100);
  client.send({ jsonrpc: "2.0", method: "initialized", params: {} });

  const init = client.getInit();
  const completionProvider = init?.capabilities?.completionProvider;
  if (!completionProvider) {
    console.error("FAIL: missing completionProvider in initialize");
    process.exit(1);
  }

  let version = 1;
  const openText = "w";
  client.send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: {
      textDocument: { uri, languageId: "pscript", version, text: openText },
    },
  });

  client.send({
    jsonrpc: "2.0",
    id: 100,
    method: "textDocument/completion",
    params: {
      textDocument: { uri },
      position: { line: 0, character: 1 },
    },
  });
  await sleep(300);

  const completion = client.getCompletion(100);
  const labels = (completion?.items || []).map((i) => i.label);
  const hasWhile = labels.includes("while");
  const whileItem = (completion?.items || []).find((i) => i.label === "while");

  client.send({
    jsonrpc: "2.0",
    method: "textDocument/didChange",
    params: {
      textDocument: { uri, version: ++version },
      contentChanges: [{ text: "p" }],
    },
  });
  await sleep(500);

  client.send({
    jsonrpc: "2.0",
    method: "textDocument/didChange",
    params: {
      textDocument: { uri, version: ++version },
      contentChanges: [{ text: "pr" }],
    },
  });
  await sleep(500);

  client.send({
    jsonrpc: "2.0",
    method: "textDocument/didChange",
    params: {
      textDocument: { uri, version: ++version },
      contentChanges: [{ text: 'print "merhaba"' }],
    },
  });
  await sleep(500);

  const diags = client.getDiags();
  const lastDiag = diags[diags.length - 1];
  const pDiag = diags.find((d) => d.count > 0 && d.messages.some((m) => m.includes("invalid statement")));

  client.close();

  const report = {
    lspPath,
    triggerCharacters: completionProvider.triggerCharacters,
    completionLabels: labels.slice(0, 10),
    hasWhile,
    whileUsesTextEdit: Boolean(whileItem?.textEdit),
    diagnosticEvents: diags,
    hadInvalidStatementOnP: Boolean(pDiag),
    finalDiagnosticCount: lastDiag?.count ?? -1,
  };
  console.log(JSON.stringify(report, null, 2));

  const ok = hasWhile && !whileItem?.textEdit && !pDiag && (lastDiag?.count ?? 0) === 0;
  process.exit(ok ? 0 : 1);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
