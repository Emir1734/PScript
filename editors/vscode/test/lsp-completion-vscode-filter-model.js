const assert = require("assert");

// Models VS Code suggest filtering for LSP items converted with textEdit (fromEdit=true).
// The replace range length must match the current typed prefix or items are dropped.
function filterWithTextEditRange(items, currentPrefixLength) {
  return items.filter((item) => {
    if (!item.textEdit?.range) {
      return true;
    }
    const span =
      item.textEdit.range.end.character - item.textEdit.range.start.character;
    return span === currentPrefixLength;
  });
}

function filterLabelOnly(items, typedPrefix) {
  return items.filter((item) => {
    const ft = item.filterText || item.label;
    return ft.toLowerCase().startsWith(typedPrefix.toLowerCase());
  });
}

const priResponseItems = [
  {
    label: "print",
    filterText: "print",
    textEdit: { range: { start: { line: 0, character: 0 }, end: { line: 0, character: 3 } }, newText: "print" },
  },
  { label: "priv", filterText: "priv", textEdit: { range: { start: { line: 0, character: 0 }, end: { line: 0, character: 3 } }, newText: "priv" } },
];

const afterBackspaceToPrTextEdit = filterWithTextEditRange(priResponseItems, 2);
assert.strictEqual(
  afterBackspaceToPrTextEdit.length,
  0,
  "textEdit range mismatch hides all items on pri->pr without a new LSP request",
);

const afterBackspaceToPrLabel = filterLabelOnly(priResponseItems, "pr");
assert.ok(afterBackspaceToPrLabel.some((i) => i.label === "print"), "label-only filter keeps print on pri->pr");

console.log("PASS  lsp-completion-vscode-filter-model");
