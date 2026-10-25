const fs = require("fs");
const path = require("path");

const root = path.join(__dirname, "..", "..", "..");
const sourceIcon = path.join(root, "assets", "icon.png");
const targetDir = path.join(__dirname, "..", "assets");
const targetIcon = path.join(targetDir, "icon.png");

if (!fs.existsSync(targetDir)) {
  fs.mkdirSync(targetDir, { recursive: true });
}

if (fs.existsSync(sourceIcon)) {
  fs.copyFileSync(sourceIcon, targetIcon);
  console.log("Copied assets/icon.png into extension package.");
} else {
  console.warn("Warning: repository assets/icon.png not found; VSIX icon may be missing.");
}

const PREFERRED_BUILD_SUBDIRS = ["main", "release", "debug"];
function releaseLspCandidates(repoRoot) {
  const out = [];
  const buildParent = path.join(repoRoot, "build");
  if (!fs.existsSync(buildParent)) {
    return out;
  }
  const subdirs = fs
    .readdirSync(buildParent)
    .filter((name) => {
      try {
        return fs.statSync(path.join(buildParent, name)).isDirectory();
      } catch {
        return false;
      }
    })
    .sort((a, b) => {
      const ia = PREFERRED_BUILD_SUBDIRS.indexOf(a);
      const ib = PREFERRED_BUILD_SUBDIRS.indexOf(b);
      if (ia >= 0 && ib >= 0) return ia - ib;
      if (ia >= 0) return -1;
      if (ib >= 0) return 1;
      return a.localeCompare(b);
    });
  for (const sub of subdirs) {
    const base = path.join(buildParent, sub, "Release");
    out.push(
      path.join(base, "pscript_lsp.exe"),
      path.join(base, "Release", "pscript_lsp.exe"),
      path.join(base, "Release", "pscript_lsp"),
      path.join(base, "pscript_lsp"),
      path.join(buildParent, sub, "bin", "RELEASE", "pscript_lsp.exe"),
      path.join(buildParent, sub, "bin", "Release", "pscript_lsp.exe"),
    );
  }
  return out;
}
const lspCandidates = releaseLspCandidates(root);
const serverDir = path.join(__dirname, "..", "server");
const bundledName = process.platform === "win32" ? "pscript_lsp.exe" : "pscript_lsp";
const bundledPath = path.join(serverDir, bundledName);
let copiedLsp = false;
for (const candidate of lspCandidates) {
  if (fs.existsSync(candidate)) {
    if (!fs.existsSync(serverDir)) {
      fs.mkdirSync(serverDir, { recursive: true });
    }
    fs.copyFileSync(candidate, bundledPath);
    console.log(`Bundled language server: ${candidate} -> ${bundledPath}`);
    copiedLsp = true;
    break;
  }
}
if (!copiedLsp) {
  console.warn(
    "Warning: Release pscript_lsp not found; build pscript_lsp before packaging VSIX.",
  );
}

function releaseDapCandidates(repoRoot) {
  const out = [];
  const buildParent = path.join(repoRoot, "build");
  if (!fs.existsSync(buildParent)) {
    return out;
  }
  for (const sub of fs.readdirSync(buildParent)) {
    const base = path.join(buildParent, sub, "Release");
    if (!fs.existsSync(base)) {
      continue;
    }
    out.push(
      path.join(base, "pscript_dap.exe"),
      path.join(base, "Release", "pscript_dap.exe"),
      path.join(base, "Release", "pscript_dap"),
      path.join(base, "pscript_dap"),
    );
  }
  return out;
}
const dapCandidates = releaseDapCandidates(root);
const dapName = process.platform === "win32" ? "pscript_dap.exe" : "pscript_dap";
const bundledDapPath = path.join(serverDir, dapName);
let copiedDap = false;
for (const candidate of dapCandidates) {
  if (fs.existsSync(candidate)) {
    if (!fs.existsSync(serverDir)) {
      fs.mkdirSync(serverDir, { recursive: true });
    }
    fs.copyFileSync(candidate, bundledDapPath);
    console.log(`Bundled debug adapter: ${candidate} -> ${bundledDapPath}`);
    copiedDap = true;
    break;
  }
}
if (!copiedDap) {
  console.warn("Warning: Release pscript_dap not found; build pscript_dap before packaging VSIX.");
}
