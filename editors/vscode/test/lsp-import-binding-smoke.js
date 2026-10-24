const { spawn } = require("child_process");

const { resolveLspPath } = require("./resolve-lsp-path");

const lspPath = resolveLspPath();
const uri = "file:///C:/tmp/lsp-import-binding.phs";

function frame(obj) {
  const body = JSON.stringify(obj);
  return `Content-Length: ${Buffer.byteLength(body, "utf8")}\r\n\r\n${body}`;
}

async function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function withLsp(fn) {
  const proc = spawn(lspPath, [], { stdio: ["pipe", "pipe", "pipe"] });
  proc.stdin.write(frame({ jsonrpc: "2.0", id: 1, method: "initialize", params: { capabilities: {} } }));
  await sleep(80);
  proc.stdin.write(frame({ jsonrpc: "2.0", method: "initialized", params: {} }));
  proc.stdin.write(
    frame({
      jsonrpc: "2.0",
      method: "textDocument/didOpen",
      params: {
        textDocument: { uri, languageId: "pscript", version: 1, text: "import ma\n" },
      },
    }),
  );
  await sleep(100);
  try {
    return await fn(proc);
  } finally {
    proc.kill();
  }
}

async function completionLabels(proc, text, line, character) {
  return new Promise((resolve, reject) => {
    let buffer = Buffer.alloc(0);
    const id = Math.floor(Math.random() * 1e6) + 5000;
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
          if (!msg.result) {
            reject(new Error(`completion null: ${JSON.stringify(msg.error || msg)}`));
            return;
          }
          resolve((msg.result.items || []).map((i) => ({ label: i.label, kind: i.kind })));
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
          method: "textDocument/completion",
          params: {
            textDocument: { uri },
            position: { line, character },
          },
        }),
      );
    }, 120);
    setTimeout(() => {
      proc.stdout.off("data", onData);
      reject(new Error("completion timeout"));
    }, 4000);
  });
}

async function main() {
  await withLsp(async (proc) => {
    const importMa = await completionLabels(proc, "import ma", 0, 9);
    if (!importMa.some((i) => i.label === "math")) {
      throw new Error(`import ma: expected math, got ${JSON.stringify(importMa.slice(0, 12))}`);
    }

    const mathM = await completionLabels(proc, "import math\nm", 1, 1);
    const mathItem = mathM.find((i) => i.label === "math");
    if (!mathItem) {
      throw new Error(`import math + m: expected math binding, got ${JSON.stringify(mathM.map((i) => i.label))}`);
    }
    if (mathItem.kind !== 6) {
      throw new Error(`import math + m: expected LSP Variable kind 6 for import binding, got ${mathItem.kind}`);
    }

    const blankM = await completionLabels(proc, "import math\n\nm", 2, 1);
    if (!blankM.some((i) => i.label === "math")) {
      throw new Error(`import math blank line + m: expected math, got ${JSON.stringify(blankM.map((i) => i.label))}`);
    }

    const ma = await completionLabels(proc, "import math\n\nma", 2, 2);
    if (!ma.some((i) => i.label === "math")) {
      throw new Error(`import math + ma: expected math, got ${JSON.stringify(ma.map((i) => i.label))}`);
    }

    const mat = await completionLabels(proc, "import math\n\nmat", 2, 3);
    if (!mat.some((i) => i.label === "math")) {
      throw new Error(`import math + mat: expected math, got ${JSON.stringify(mat.map((i) => i.label))}`);
    }

    const aliasM = await completionLabels(proc, "import math as mm\nm", 1, 1);
    if (!aliasM.some((i) => i.label === "mm")) {
      throw new Error(`import alias + m: expected mm, got ${JSON.stringify(aliasM.map((i) => i.label))}`);
    }

    const aliasDot = await completionLabels(proc, "import math as mm\nmm.", 1, 3);
    for (const name of ["sqrt", "pow", "pi", "e"]) {
      if (!aliasDot.some((i) => i.label === name)) {
        throw new Error(`mm. exports: missing ${name}, got ${JSON.stringify(aliasDot.slice(0, 20).map((i) => i.label))}`);
      }
    }

    const fromSq = await completionLabels(proc, "from math import sq", 0, 18);
    if (!fromSq.some((i) => i.label === "sqrt")) {
      throw new Error(`from math import sq: expected sqrt, got ${JSON.stringify(fromSq.map((i) => i.label))}`);
    }
  });

  console.log("PASS  lsp-import-binding-smoke.js");
}

main().catch((e) => {
  console.error("FAIL ", e.message || e);
  process.exit(1);
});
