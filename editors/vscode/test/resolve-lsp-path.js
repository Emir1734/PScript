const fs = require("fs");
const path = require("path");

const COMPILE_EXE = "pscript_lsp_compile.exe";
const CANONICAL_EXE = "pscript_lsp.exe";
const PREFERRED_BUILD_SUBDIRS = ["main", "release", "debug"];

function firstExisting(candidates) {
  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }
  return undefined;
}

function repoRoot() {
  return path.resolve(__dirname, "../../..");
}

function cmakeBuildRoots(workspaceRoot) {
  const parent = path.join(workspaceRoot, "build");
  if (!fs.existsSync(parent)) {
    return [];
  }
  const names = fs.readdirSync(parent).filter((name) => {
    const full = path.join(parent, name);
    try {
      return fs.statSync(full).isDirectory();
    } catch {
      return false;
    }
  });
  names.sort((a, b) => {
    const ia = PREFERRED_BUILD_SUBDIRS.indexOf(a);
    const ib = PREFERRED_BUILD_SUBDIRS.indexOf(b);
    if (ia >= 0 && ib >= 0) {
      return ia - ib;
    }
    if (ia >= 0) {
      return -1;
    }
    if (ib >= 0) {
      return 1;
    }
    return a.localeCompare(b);
  });
  return names.map((name) => path.join(parent, name));
}

function newestBinaryUnderRoots(roots, config, fileName) {
  let best;
  let bestMtime = -1;
  for (const root of roots) {
    for (const candidate of [
      path.join(root, config, fileName),
      path.join(root, config, "Debug", fileName),
    ]) {
      if (!fs.existsSync(candidate)) {
        continue;
      }
      const mtime = fs.statSync(candidate).mtimeMs;
      if (mtime > bestMtime) {
        bestMtime = mtime;
        best = candidate;
      }
    }
  }
  return best;
}

function canonicalBinaryUnderRoots(roots, config, fileName) {
  for (const root of roots) {
    const candidate = path.join(root, config, fileName);
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }
  return undefined;
}

function resolveBuildLspCompilePath(config = "Debug") {
  const roots = cmakeBuildRoots(repoRoot());
  const found = newestBinaryUnderRoots(roots, config, COMPILE_EXE);
  return found || path.join(repoRoot(), "build", "main", config, COMPILE_EXE);
}

function resolveBuildLspCanonicalPath(config = "Debug") {
  const roots = cmakeBuildRoots(repoRoot());
  const found = canonicalBinaryUnderRoots(roots, config, CANONICAL_EXE);
  return found || path.join(repoRoot(), "build", "main", config, CANONICAL_EXE);
}

function resolveBuildLspPath(preferDebug = true) {
  const roots = cmakeBuildRoots(repoRoot());
  const debugCompile = newestBinaryUnderRoots(roots, "Debug", COMPILE_EXE);
  const debugCanonical = canonicalBinaryUnderRoots(roots, "Debug", CANONICAL_EXE);
  const releaseCompile = newestBinaryUnderRoots(roots, "Release", COMPILE_EXE);
  const releaseCanonical = canonicalBinaryUnderRoots(roots, "Release", CANONICAL_EXE);
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
