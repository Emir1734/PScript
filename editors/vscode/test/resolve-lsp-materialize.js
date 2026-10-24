const fs = require("fs");
const os = require("os");
const path = require("path");

const { resolveBuildLspPath, materializeLspRuntime } = require("./resolve-lsp-path");

const source = resolveBuildLspPath(true);
if (!fs.existsSync(source)) {
  console.log("SKIP  resolve-lsp-materialize (build LSP first)");
  process.exit(0);
}

const destDir = path.join(os.tmpdir(), "pscript-lsp-materialize-test");
const dest = path.join(destDir, "pscript_lsp.exe");
fs.rmSync(destDir, { recursive: true, force: true });

const first = materializeLspRuntime(source, dest);
if (first !== dest || !fs.existsSync(dest)) {
  throw new Error("materializeLspRuntime failed");
}
const hash1 = fs.readFileSync(dest);

const second = materializeLspRuntime(source, dest);
if (second !== dest) {
  throw new Error("materializeLspRuntime should return dest path");
}
const hash2 = fs.readFileSync(dest);
if (!hash1.equals(hash2)) {
  throw new Error("materialize should not corrupt binary");
}

console.log("PASS  resolve-lsp-materialize");
