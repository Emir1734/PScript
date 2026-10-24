const assert = require("assert");
const fs = require("fs");
const path = require("path");

const extensionRoot = path.join(__dirname, "..");

function readGrammar() {
  return JSON.parse(fs.readFileSync(path.join(extensionRoot, "syntaxes", "pscript.tmLanguage.json"), "utf8"));
}

async function main() {
  const grammar = readGrammar();
  assert.strictEqual(grammar.scopeName, "source.pscript");

  const unterminatedRules = grammar.repository.strings.patterns.filter((p) => p.begin && p.end === "$");
  assert.ok(unterminatedRules.length >= 2, "expected begin/end=$ rules for unterminated string lines");
  const completeRules = grammar.repository.strings.patterns.filter((p) => p.match && p.match.includes("\\n"));
  assert.ok(completeRules.length >= 2, "expected line-bounded complete string match rules");

  const typeRegex = grammar.repository["types-builtin"].patterns[0].match;
  for (const word of ["int", "float", "string", "str"]) {
    assert.ok(new RegExp(typeRegex).test(word), `${word} must match support.type pattern`);
  }

  let Registry;
  let loadWASM;
  try {
    ({ Registry } = require("vscode-textmate"));
    ({ loadWASM } = require("vscode-oniguruma"));
  } catch {
    console.log("PASS  syntax-highlight-smoke.js (grammar structure only)");
    return;
  }

  const wasmPath = path.join(extensionRoot, "node_modules", "vscode-oniguruma", "release", "onig.wasm");
  if (!fs.existsSync(wasmPath)) {
    console.log("PASS  syntax-highlight-smoke.js (grammar structure only)");
    return;
  }

  await loadWASM(fs.readFileSync(wasmPath).buffer);
  const oniguruma = require("vscode-oniguruma");
  const registry = new Registry({
    onigLib: Promise.resolve(oniguruma),
    loadGrammar: async (scopeName) => (scopeName === grammar.scopeName ? grammar : null),
  });
  const tm = await registry.loadGrammar(grammar.scopeName);
  if (!tm) {
    console.log("PASS  syntax-highlight-smoke.js (grammar structure only; textmate init skipped)");
    return;
  }

  function scopesOnLine(lineText, character, ruleStack) {
    const { tokens, ruleStack: nextStack } = tm.tokenizeLine(lineText, ruleStack);
    for (const token of tokens) {
      if (token.startIndex <= character && character < token.endIndex) {
        return { scopes: token.scopes, ruleStack: nextStack };
      }
    }
    return { scopes: [], ruleStack: nextStack };
  }

  function scopesAfterLines(lines, lineIndex, word) {
    let stack = null;
    for (let i = 0; i < lines.length; ++i) {
      const line = lines[i];
      const idx = i === lineIndex ? line.indexOf(word) : -1;
      if (i === lineIndex) {
        assert.ok(idx >= 0, `word ${word} on line ${lineIndex}`);
        const result = scopesOnLine(line, idx, stack);
        return result.scopes;
      }
      ({ ruleStack: stack } = tm.tokenizeLine(line, stack));
    }
    return [];
  }

  const userCase = ["str a = \"a\"", "int b = 1", "float c = 3.14", "str d = x"];
  const intAfterBroken = scopesAfterLines(userCase, 1, "int");
  assert.ok(intAfterBroken.some((s) => s.includes("support.type")), `int after unterminated: ${intAfterBroken.join(" ")}`);
  assert.ok(
    !intAfterBroken.some((s) => s.includes("string.quoted")),
    `int must not be in string scope: ${intAfterBroken.join(" ")}`,
  );

  const closedCase = ["str a = \"a\"", "int b = 1"];
  const intAfterClosed = scopesAfterLines(closedCase, 1, "int");
  assert.ok(intAfterClosed.some((s) => s.includes("support.type")), `int after closed string: ${intAfterClosed.join(" ")}`);

  const unterminatedTypes = ["str a = \"a", "int", "float", "str", "string"];
  for (const word of ["int", "float", "str", "string"]) {
    const idx = unterminatedTypes.indexOf(word);
    const scopes = scopesAfterLines(unterminatedTypes, idx, word);
    assert.ok(scopes.some((s) => s.includes("support.type")), `${word} after broken string: ${scopes.join(" ")}`);
    assert.ok(!scopes.some((s) => s.includes("string.quoted")), `${word} must not be string scope`);
  }

  const allTypesLine = "int a = 1\nfloat b = 3.14\nstr c = \"w\"\nstring d = \"H\"";
  for (const [lineIdx, word, len] of [
    [0, "int", 3],
    [1, "float", 5],
    [2, "str", 3],
    [3, "string", 6],
  ]) {
    const lines = allTypesLine.split("\n");
    let stack = null;
    for (let i = 0; i < lines.length; ++i) {
      if (i === lineIdx) {
        const pos = lines[i].indexOf(word);
        const { scopes } = scopesOnLine(lines[i], pos, stack);
        assert.ok(scopes.some((s) => s.includes("support.type")), `TM ${word}: ${scopes.join(" ")}`);
        break;
      }
      ({ ruleStack: stack } = tm.tokenizeLine(lines[i], stack));
    }
  }

  console.log("PASS  syntax-highlight-smoke.js");
}

main().catch((e) => {
  console.error("FAIL  syntax-highlight-smoke.js", e.message || e);
  process.exit(1);
});
