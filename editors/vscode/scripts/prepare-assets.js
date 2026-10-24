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

const lspCandidates = [
  path.join(root, "build", "Release", "pscript_lsp.exe"),
  path.join(root, "build", "Release", "Release", "pscript_lsp.exe"),
  path.join(root, "build", "Release", "Release", "pscript_lsp"),
  path.join(root, "build", "Release", "pscript_lsp"),
];
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

const dapCandidates = [
  path.join(root, "build", "Release", "pscript_dap.exe"),
  path.join(root, "build", "Release", "Release", "pscript_dap.exe"),
  path.join(root, "build", "Release", "Release", "pscript_dap"),
  path.join(root, "build", "Release", "pscript_dap"),
];
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
