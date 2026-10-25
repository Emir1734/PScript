"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");

const root = path.join(__dirname, "..", "..", "..");
const PREFERRED = ["main", "release", "debug"];

function cmakeBuildRoots() {
  const parent = path.join(root, "build");
  if (!fs.existsSync(parent)) {
    return [];
  }
  return fs
    .readdirSync(parent)
    .filter((name) => {
      try {
        return fs.statSync(path.join(parent, name)).isDirectory();
      } catch {
        return false;
      }
    })
    .sort((a, b) => {
      const ia = PREFERRED.indexOf(a);
      const ib = PREFERRED.indexOf(b);
      if (ia >= 0 && ib >= 0) return ia - ib;
      if (ia >= 0) return -1;
      if (ib >= 0) return 1;
      return a.localeCompare(b);
    })
    .map((name) => path.join(parent, name));
}

const candidates = [];
for (const tree of cmakeBuildRoots()) {
  candidates.push(
    path.join(tree, "Release", "pscript_dap.exe"),
    path.join(tree, "Release", "Release", "pscript_dap.exe"),
    path.join(tree, "Debug", "pscript_dap.exe"),
  );
}

const found = candidates.find((p) => fs.existsSync(p));
assert.ok(found, `pscript_dap not found; build pscript_dap first. Tried: ${candidates.slice(0, 6).join(", ")}…`);
console.log("PASS  dap binary resolve smoke:", found);
