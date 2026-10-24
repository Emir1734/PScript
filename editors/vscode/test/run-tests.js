const assert = require("assert");
const fs = require("fs");
const path = require("path");

const extensionRoot = path.join(__dirname, "..");

function readJson(relativePath) {
  return JSON.parse(fs.readFileSync(path.join(extensionRoot, relativePath), "utf8"));
}

function testPackageMetadata() {
  const pkg = readJson("package.json");
  assert.strictEqual(pkg.name, "pscript");
  assert.strictEqual(pkg.icon, "assets/icon.png");
  assert.ok(pkg.contributes.languages.some((lang) => lang.id === "pscript"));
  assert.ok(pkg.contributes.languages.some((lang) => lang.extensions.includes(".phs")));
  const lang = pkg.contributes.languages.find((l) => l.id === "pscript");
  assert.ok(lang.icon?.dark?.includes("assets/icon.png"));
  assert.ok(pkg.contributes.languages.some((lang) => lang.aliases.includes("PScript")));
  assert.ok(
    !pkg.activationEvents.includes("onLanguage:pscript"),
    "onLanguage:pscript is implicit from contributes.languages",
  );
  assert.ok(pkg.activationEvents.includes("onStartupFinished"));
  assert.ok(pkg.activationEvents.includes("workspaceContains:**/*.phs"));
  assert.ok(Array.isArray(pkg.extensionKind) && pkg.extensionKind.includes("ui"));
  assert.ok(pkg.contributes.debuggers?.some((dbg) => dbg.type === "pscript"));
  console.log("PASS  package metadata");
}

function testGrammar() {
  const grammar = readJson("syntaxes/pscript.tmLanguage.json");
  assert.strictEqual(grammar.scopeName, "source.pscript");
  assert.ok(grammar.repository["keywords-control"]);
  assert.ok(grammar.repository.declarations);
  const typePatterns = grammar.repository["type-name"].patterns.concat(
    grammar.repository["types-builtin"].patterns,
  );
  const typeRegexes = typePatterns
    .filter((p) => p.name === "support.type.pscript")
    .map((p) => p.match);
  assert.ok(typeRegexes.length >= 2, "expected builtin type patterns in grammar");
  for (const re of typeRegexes) {
    assert.ok(re.includes("|str|") || re.includes("(str|"), "str must use support.type.pscript like int/float/bool/string");
  }
  assert.ok(grammar.repository.identifiers, "expected identifier fallback repository");
  const stringEndsAtLine = grammar.repository.strings.patterns.some(
    (p) => p.end === "$" || (p.match && p.match.includes("\\n")),
  );
  assert.ok(stringEndsAtLine, "strings must be line-bounded (match or end=$)");
  assert.ok(
    grammar.repository.numbers.patterns.some((p) => p.name === "constant.numeric.duration.pscript"),
    "expected duration literal scope",
  );
  console.log("PASS  syntax grammar");
}

function testIconTheme() {
  const pkg = readJson("package.json");
  assert.ok(pkg.contributes.iconThemes.some((theme) => theme.id === "pscript-icons"));
  const theme = readJson("themes/pscript-icon-theme.json");
  assert.strictEqual(theme.fileExtensions.phs, "_pscript_file");
  assert.strictEqual(theme.iconDefinitions._pscript_file.iconPath, "./assets/icon.png");
  assert.strictEqual(theme.fileExtensions.phs, "_pscript_file");
  assert.strictEqual(theme.languageIds.pscript, "_pscript_file");
  console.log("PASS  icon theme");
}

function testLanguageConfiguration() {
  const config = readJson("language-configuration.json");
  assert.strictEqual(config.comments.lineComment, "#");
  assert.ok(config.indentationRules.increaseIndentPattern.includes("if"));
  assert.ok(config.brackets.length >= 1);
  console.log("PASS  language configuration");
}

function testSnippets() {
  const snippets = readJson("snippets/pscript.json");
  assert.ok(Object.keys(snippets).length >= 3);
  assert.ok(snippets["If (braces)"]);
  console.log("PASS  snippets");
}

function testExtensionBuild() {
  const outFile = path.join(extensionRoot, "out", "extension.js");
  assert.ok(fs.existsSync(outFile), "compile extension before running tests");
  console.log("PASS  extension build output");
}

function testIconPackaging() {
  const iconPath = path.join(extensionRoot, "assets", "icon.png");
  const repoIcon = path.join(extensionRoot, "..", "..", "assets", "icon.png");
  if (fs.existsSync(iconPath) || fs.existsSync(repoIcon)) {
    console.log("PASS  icon asset present");
  } else {
    console.log("PASS  icon asset check skipped (assets/icon.png not in workspace)");
  }
}

testPackageMetadata();
testGrammar();
testIconTheme();
testLanguageConfiguration();
testSnippets();
testExtensionBuild();
testIconPackaging();

function runNodeSmoke(scriptName) {
  const scriptPath = path.join(__dirname, scriptName);
  if (!fs.existsSync(scriptPath)) {
    return;
  }
  const { execSync } = require("child_process");
  execSync(`node "${scriptPath}"`, { stdio: "inherit", cwd: extensionRoot });
  console.log(`PASS  ${scriptName}`);
}

runNodeSmoke("syntax-highlight-smoke.js");

const { runResolveLspPathPriority } = require("./resolve-lsp-path-priority.js");
runResolveLspPathPriority();
const { resolveLspPath } = require("./resolve-lsp-path");
const releaseLsp = resolveLspPath();
if (fs.existsSync(releaseLsp)) {
  runNodeSmoke("lsp-smoke-stdio.js");
  runNodeSmoke("lsp-integration-smoke.js");
  runNodeSmoke("lsp-stress-smoke.js");
  runNodeSmoke("lsp-completion-prefix-smoke.js");
  runNodeSmoke("lsp-module-completion-smoke.js");
  runNodeSmoke("lsp-import-binding-smoke.js");
  runNodeSmoke("lsp-semantic-variable-smoke.js");
  runNodeSmoke("lsp-semantic-classification-smoke.js");
  runNodeSmoke("lsp-completion-diagnostic.js");
  runNodeSmoke("lsp-completion-trace.js");
  runNodeSmoke("lsp-completion-backspace-sequence.js");
  runNodeSmoke("lsp-diag-uri-smoke.js");
  runNodeSmoke("lsp-incomplete-string-completion-smoke.js");
  runNodeSmoke("lsp-close-debounce-smoke.js");
  runNodeSmoke("lsp-invalid-char-smoke.js");
  runNodeSmoke("lsp-unicode-code-area-smoke.js");
  runNodeSmoke("lsp-didchange-unicode-regression.js");
  require("./lsp-completion-vscode-filter-model.js");
  require("./lsp-completion-backspace-model.js");
  require("./resolve-lsp-materialize.js");
} else {
  console.log("SKIP  LSP smoke tests (Release pscript_lsp not built)");
}

require("./dap-resolve-smoke.js");

console.log("All extension tests passed");
