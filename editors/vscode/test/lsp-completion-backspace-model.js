const assert = require("assert");

// Models VS Code re-filtering a cached CompletionList when isIncomplete=false
// without sending a new textDocument/completion request (common on Backspace).
function clientRefilter(items, prefix) {
  return items.filter((item) => {
    const ft = item.filterText || item.label;
    return ft.startsWith(prefix);
  });
}

const priResponse = {
  isIncomplete: false,
  items: [
    { label: "print", filterText: "print" },
    { label: "priv", filterText: "priv" },
    { label: "private", filterText: "private" },
  ],
};

const afterBackspaceToPr = clientRefilter(priResponse.items, "pr");
assert.ok(afterBackspaceToPr.some((i) => i.label === "print"), "print survives pri->pr refilter");

const afterBackspaceToP = clientRefilter(priResponse.items, "p");
assert.strictEqual(
  afterBackspaceToP.some((i) => i.label === "pass"),
  false,
  "pass missing when refiltering pri cache to p (bug model)",
);

console.log("PASS  lsp-completion-backspace-model (documents client stale-filter bug)");
