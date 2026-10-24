const { spawn } = require("child_process");
const path = require("path");

const { resolveLspPath } = require("./resolve-lsp-path");
const lspPath = resolveLspPath();

function frame(obj) {
  const body = JSON.stringify(obj);
  return `Content-Length: ${Buffer.byteLength(body, "utf8")}\r\n\r\n${body}`;
}

async function main() {
  const proc = spawn(lspPath, [], { stdio: ["pipe", "pipe", "pipe"] });
  let buffer = Buffer.alloc(0);
  let diags = null;

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
      if (msg.method === "textDocument/publishDiagnostics") {
        diags = msg.params;
      }
    }
  });

  proc.stdin.write(
    frame({ jsonrpc: "2.0", id: 1, method: "initialize", params: { capabilities: {} } }),
  );
  await new Promise((r) => setTimeout(r, 80));
  proc.stdin.write(frame({ jsonrpc: "2.0", method: "initialized", params: {} }));
  proc.stdin.write(
    frame({
      jsonrpc: "2.0",
      method: "textDocument/didOpen",
      params: {
        textDocument: {
          uri: "file:///C:/tmp/m.phs",
          languageId: "pscript",
          version: 1,
          text: process.env.PSCRIPT_TEXT || 'print "merhaba"\n',
        },
      },
    }),
  );
  await new Promise((r) => setTimeout(r, 600));
  proc.kill();
  const count = diags?.diagnostics?.length ?? -1;
  console.log(JSON.stringify({ diagnosticCount: count, diags }, null, 2));
  process.exit(count === 0 ? 0 : count > 0 ? 2 : 1);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
