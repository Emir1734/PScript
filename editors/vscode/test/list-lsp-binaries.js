/**
 * Inventory all pscript_lsp.exe under the repo and print SHA256 (for VS Code path debugging).
 */
const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

const repoRoot = path.resolve(__dirname, "../../..");
const staleSha = "c767775326dc7be37175e9d192e34cde3ba3e3318fdddd0771fa3d0ed801513b";

function walk(dir, out) {
  let entries;
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true });
  } catch {
    return;
  }
  for (const ent of entries) {
    if (ent.name === "node_modules" || ent.name === ".git") {
      continue;
    }
    const full = path.join(dir, ent.name);
    if (ent.isDirectory()) {
      walk(full, out);
    } else if (ent.name === "pscript_lsp.exe" || ent.name === "pscript_lsp") {
      out.push(full);
    }
  }
}

const files = [];
walk(repoRoot, files);
files.sort();

console.log("pscript_lsp binaries in repo:\n");
for (const file of files) {
  const buf = fs.readFileSync(file);
  const sha = crypto.createHash("sha256").update(buf).digest("hex");
  const rel = path.relative(repoRoot, file);
  let tag = "";
  if (sha === staleSha) {
    tag = "  <-- STALE (VS Code c7677753…)";
  } else if (rel.replace(/\\/g, "/") === "build/Debug/pscript_lsp.exe") {
    tag = "  <-- CANONICAL (prefer this)";
  }
  console.log(`${sha}  ${buf.length}  ${rel}${tag}`);
}

console.log("\nExtension resolve (no env):");
const { resolveLspPath } = require("./resolve-lsp-path.js");
console.log(resolveLspPath());
