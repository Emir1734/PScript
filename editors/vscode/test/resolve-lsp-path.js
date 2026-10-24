const fs = require("fs");
const path = require("path");

const COMPILE_EXE = "pscript_lsp_compile.exe";
const CANONICAL_EXE = "pscript_lsp.exe";

function firstExisting(candidates) {
  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }
  return undefined;
}

/** MSVC multi-config can emit pscript_lsp_compile.exe under build/Debug/Debug; pick the newest compile binary. */
function newestCompileBinary(config) {
  const parent = path.join(repoRoot(), "build", config, COMPILE_EXE);
  const nested = path.join(repoRoot(), "build", config, "Debug", COMPILE_EXE);
  let best;
  let bestMtime = -1;
  for (const candidate of [parent, nested]) {
    if (!fs.existsSync(candidate)) {
      continue;
    }
    const mtime = fs.statSync(candidate).mtimeMs;
    if (mtime > bestMtime) {
      bestMtime = mtime;
      best = candidate;
    }
  }
  return best;
}

function repoRoot() {
  return path.resolve(__dirname, "../../..");
}

function resolveBuildLspCompilePath(config = "Debug") {
  return path.join(repoRoot(), "build", config, COMPILE_EXE);
}

function resolveBuildLspCanonicalPath(config = "Debug") {
  return path.join(repoRoot(), "build", config, CANONICAL_EXE);
}

/** CMake link output (safe to rebuild while IDE runs a copied runtime). */
function resolveBuildLspPath(preferDebug = true) {
  const debugCompile = newestCompileBinary("Debug");
  const debugCanonical = resolveBuildLspCanonicalPath("Debug");
  const releaseCompile = newestCompileBinary("Release");
  const releaseCanonical = resolveBuildLspCanonicalPath("Release");
  if (preferDebug) {
    return (
      firstExisting([debugCompile, debugCanonical, releaseCompile, releaseCanonical].filter(Boolean)) ||
      debugCompile ||
      debugCanonical
    );
  }
  return (
    firstExisting([releaseCompile, releaseCanonical, debugCompile, debugCanonical].filter(Boolean)) ||
    releaseCompile ||
    releaseCanonical
  );
}

/** Used by LSP smoke tests: prefer compile-stage binary, then canonical. */
function resolveLspPath() {
  if (process.env.PSCRIPT_LSP && fs.existsSync(process.env.PSCRIPT_LSP)) {
    return process.env.PSCRIPT_LSP;
  }
  const preferDebug = process.env.PSCRIPT_LSP_PREFER_DEBUG !== "0";
  return resolveBuildLspPath(preferDebug);
}

function materializeLspRuntime(sourcePath, destPath) {
  fs.mkdirSync(path.dirname(destPath), { recursive: true });
  const srcStat = fs.statSync(sourcePath);
  try {
    let needsCopy = true;
    if (fs.existsSync(destPath)) {
      const destStat = fs.statSync(destPath);
      needsCopy = destStat.mtimeMs < srcStat.mtimeMs || destStat.size !== srcStat.size;
    }
    if (needsCopy) {
      fs.copyFileSync(sourcePath, destPath);
    }
  } catch {
    return sourcePath;
  }
  if (fs.existsSync(destPath)) {
    const destStat = fs.statSync(destPath);
    if (destStat.mtimeMs < srcStat.mtimeMs || destStat.size !== srcStat.size) {
      return sourcePath;
    }
  }
  return destPath;
}

module.exports = {
  resolveLspPath,
  resolveBuildLspPath,
  resolveBuildLspCompilePath,
  resolveBuildLspCanonicalPath,
  materializeLspRuntime,
  COMPILE_EXE,
  CANONICAL_EXE,
};
