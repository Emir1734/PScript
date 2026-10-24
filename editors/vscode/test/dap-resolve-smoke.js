"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");

const root = path.join(__dirname, "..", "..", "..");
const candidates = [
  path.join(root, "build", "Release", "pscript_dap.exe"),
  path.join(root, "build", "Release", "Release", "pscript_dap.exe"),
  path.join(root, "build", "Debug", "pscript_dap.exe"),
];

const found = candidates.find((p) => fs.existsSync(p));
assert.ok(found, `pscript_dap not found; build pscript_dap first. Tried: ${candidates.join(", ")}`);
console.log("PASS  dap binary resolve smoke:", found);
