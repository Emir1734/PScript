const { spawn } = require("child_process");

const { resolveLspPath } = require("./resolve-lsp-path");

const lspPath = resolveLspPath();
const uri = "file:///C:/tmp/lsp-semantic-variable.phs";
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
    const id = 77;
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

  const vars =
    "x = 10\nmy_variable = 20\nprint(x)\nprint(my_variable)\n";
  const tokens = await semanticTokens(proc, vars);
  for (const [line, char, len, expected] of [
    [0, 0, 1, "variable"],
    [1, 0, 11, "variable"],
    [2, 6, 1, "variable"],
    [3, 6, 11, "variable"],
  ]) {
    const t = tokenAt(tokens, line, char, len);
    if (!t || t.type !== expected) {
      proc.kill();
      throw new Error(`expected ${expected} at ${line}:${char}, got ${t ? t.type : "missing"}`);
    }
  }

  const shadow =
    "class Player:\n    public string name\n\nfunc test():\n    Player = 123\n    print(Player)\n";
  const tokens2 = await semanticTokens(proc, shadow);
  const playerAssign = tokenAt(tokens2, 4, 4, 6);
  if (!playerAssign || playerAssign.type !== "variable") {
    proc.kill();
    throw new Error(
      `shadowed Player assign should be variable, got ${playerAssign ? playerAssign.type : "missing"}`,
    );
  }
  const playerUse = tokenAt(tokens2, 5, 10, 6);
  if (!playerUse || playerUse.type !== "variable") {
    proc.kill();
    throw new Error(`shadowed Player use should be variable, got ${playerUse ? playerUse.type : "missing"}`);
  }
  const playerClass = tokenAt(tokens2, 0, 6, 6);
  if (!playerClass || playerClass.type !== "class") {
    proc.kill();
    throw new Error(`Player declaration should be class, got ${playerClass ? playerClass.type : "missing"}`);
  }

  const primitiveDecls =
    "int playerCount = 10\nfloat speed = 3.14\nstr name = \"w\"\nstring title = \"H\"\n";
  const tokens3 = await semanticTokens(proc, primitiveDecls);
  for (const [line, char, len, expected] of [
    [0, 0, 3, "type"],
    [0, 4, 11, "variable"],
    [1, 0, 5, "type"],
    [1, 6, 5, "variable"],
    [2, 0, 3, "type"],
    [2, 4, 4, "variable"],
    [3, 0, 6, "type"],
    [3, 7, 5, "variable"],
  ]) {
    const t = tokenAt(tokens3, line, char, len);
    if (!t || t.type !== expected) {
      proc.kill();
      throw new Error(`primitive decl at ${line}:${char} expected ${expected}, got ${t ? t.type : "missing"}`);
    }
  }

  proc.kill();
  console.log("PASS  lsp-semantic-variable-smoke.js");
}

main().catch((e) => {
  console.error("FAIL ", e.message || e);
  process.exit(1);
});
