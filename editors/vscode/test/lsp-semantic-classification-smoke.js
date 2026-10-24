const { spawn } = require("child_process");

const { resolveLspPath } = require("./resolve-lsp-path");

const lspPath = resolveLspPath();
const uri = "file:///C:/tmp/lsp-semantic-classification.phs";
const LEGEND = [
  "namespace",
  "type",
  "class",
  "enum",
  "interface",
  "struct",
  "typeParameter",
  "parameter",
  "variable",
  "property",
  "enumMember",
  "function",
  "method",
  "keyword",
  "modifier",
  "comment",
  "string",
  "number",
  "operator",
];

function frame(obj) {
  const body = JSON.stringify(obj);
  return `Content-Length: ${Buffer.byteLength(body, "utf8")}\r\n\r\n${body}`;
}

async function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

function decodeSemantic(data) {
  const out = [];
  let line = 0;
  let char = 0;
  for (let i = 0; i < data.length; i += 5) {
    line += data[i];
    if (data[i] > 0) char = 0;
    char += data[i + 1];
    const len = data[i + 2];
    const type = data[i + 3];
    out.push({ line, char, len, type: LEGEND[type] || String(type) });
  }
  return out;
}

function tokenAt(tokens, line, char, len) {
  return tokens.find((t) => t.line === line && t.char === char && t.len === len);
}

async function semanticTokens(proc, text) {
  return new Promise((resolve, reject) => {
    let buffer = Buffer.alloc(0);
    const id = 88;
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
          resolve(decodeSemantic(msg.result?.data || []));
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
          method: "textDocument/semanticTokens/full",
          params: { textDocument: { uri } },
        }),
      );
    }, 120);
    setTimeout(() => {
      proc.stdout.off("data", onData);
      reject(new Error("semantic timeout"));
    }, 4000);
  });
}

function expectToken(tokens, line, char, len, expected, label) {
  const t = tokenAt(tokens, line, char, len);
  if (!t || t.type !== expected) {
    throw new Error(`${label}: expected ${expected} at ${line}:${char}, got ${t ? t.type : "missing"}`);
  }
}

async function main() {
  const proc = spawn(lspPath, [], { stdio: ["pipe", "pipe", "pipe"] });
  proc.stdin.write(frame({ jsonrpc: "2.0", id: 1, method: "initialize", params: { capabilities: {} } }));
  await sleep(80);
  proc.stdin.write(frame({ jsonrpc: "2.0", method: "initialized", params: {} }));
  proc.stdin.write(
    frame({
      jsonrpc: "2.0",
      method: "textDocument/didOpen",
      params: {
        textDocument: { uri, languageId: "pscript", version: 1, text: "x = 1\n" },
      },
    }),
  );
  await sleep(100);

  const lone = "int\nfloat\nstr\nstring\n";
  const loneTokens = await semanticTokens(proc, lone);
  for (const [line, word] of [[0, "int"], [1, "float"], [2, "str"], [3, "string"]]) {
    expectToken(loneTokens, line, 0, word.length, "type", `lone ${word}`);
  }

  const decls =
    "int playerCount = 10\nfloat speed = 3.14\nstr name = \"test\"\nstring title = \"hello\"\n";
  const declTokens = await semanticTokens(proc, decls);
  expectToken(declTokens, 0, 0, 3, "type", "decl int");
  expectToken(declTokens, 0, 4, 11, "variable", "playerCount");
  expectToken(declTokens, 1, 0, 5, "type", "decl float");
  expectToken(declTokens, 1, 6, 5, "variable", "speed");
  expectToken(declTokens, 2, 0, 3, "type", "decl str");
  expectToken(declTokens, 2, 4, 4, "variable", "name");
  expectToken(declTokens, 3, 0, 6, "type", "decl string");
  expectToken(declTokens, 3, 7, 5, "variable", "title");

  const playerClass = "class Player:\n    public string name\n\nplayer = Player()\nplayer.name\n";
  const classTokens = await semanticTokens(proc, playerClass);
  expectToken(classTokens, 0, 6, 6, "class", "Player");
  expectToken(classTokens, 3, 0, 6, "variable", "player instance");
  expectToken(classTokens, 4, 7, 4, "property", "player.name");

  const mathUse = "import math\n\nmath.sqrt(16)\nmath.pi\n";
  const mathTokens = await semanticTokens(proc, mathUse);
  expectToken(mathTokens, 2, 0, 4, "namespace", "math");
  expectToken(mathTokens, 2, 5, 4, "function", "sqrt");
  expectToken(mathTokens, 3, 5, 2, "variable", "pi");

  proc.kill();
  console.log("PASS  lsp-semantic-classification-smoke.js");
}

main().catch((e) => {
  console.error("FAIL ", e.message || e);
  process.exit(1);
});
