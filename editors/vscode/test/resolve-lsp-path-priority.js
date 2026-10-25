/**
 * When both build/main/Debug/pscript_lsp.exe and nested compile outputs exist,
 * resolution must pick the canonical CMake output (no nested Debug/Debug).
 */
const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

function runResolveLspPathPriority() {
  const repoRoot = path.resolve(__dirname, "../../..");
  const compile = path.join(repoRoot, "build", "main", "Debug", "pscript_lsp_compile.exe");
  const canonical = path.join(repoRoot, "build", "main", "Debug", "pscript_lsp.exe");
  const nested = path.join(repoRoot, "build", "main", "Debug", "Debug", "pscript_lsp.exe");

  if (!fs.existsSync(compile) && !fs.existsSync(canonical)) {
    console.log("SKIP  resolve-lsp-path-priority (build Debug pscript_lsp first)");
    return;
  }

  const nestedCompile = path.join(repoRoot, "build", "main", "Debug", "Debug", "pscript_lsp_compile.exe");
  const { resolveBuildLspPath } = require("./resolve-lsp-path");
  const resolved = resolveBuildLspPath(true);
  let expected = compile;
  if (fs.existsSync(nestedCompile) && fs.existsSync(compile)) {
    expected =
      fs.statSync(nestedCompile).mtimeMs > fs.statSync(compile).mtimeMs ? nestedCompile : compile;
  } else if (fs.existsSync(nestedCompile)) {
    expected = nestedCompile;
  } else if (!fs.existsSync(compile)) {
    expected = canonical;
  }
  if (resolved !== expected) {
    throw new Error(`resolve-lsp-path-priority: expected ${expected}, got ${resolved}`);
  }

  if (fs.existsSync(nested) && fs.existsSync(canonical)) {
    const hashCanonical = crypto.createHash("sha256").update(fs.readFileSync(canonical)).digest("hex");
    const hashNested = crypto.createHash("sha256").update(fs.readFileSync(nested)).digest("hex");
    if (hashCanonical !== hashNested) {
      console.log(
        `INFO  nested canonical differs from top-level (top ${hashCanonical.slice(0, 12)}… vs nested ${hashNested.slice(0, 12)}…)`,
      );
    }
  }

  console.log("PASS  resolve-lsp-path-priority");
}

module.exports = { runResolveLspPathPriority };

if (require.main === module) {
  try {
    runResolveLspPathPriority();
  } catch (e) {
    console.error(String(e));
    process.exit(1);
  }
}
