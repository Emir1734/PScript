const assert = require("assert");

exports.run = async () => {
  const vscode = require("vscode");
  const path = require("path");
  const languages = await vscode.languages.getLanguages();
  const hasPscript = languages.includes("pscript");
  const samplePath =
    process.env.TEST_PHS_PATH ||
    path.join(__dirname, "..", "..", "..", "test.phs");
  const doc = await vscode.workspace.openTextDocument(vscode.Uri.file(samplePath));
  console.log(
    `INTEGRATION languages=${languages.length} pscript=${hasPscript} docLanguageId=${doc.languageId}`,
  );
  if (process.env.CURSOR_CLASSIC === "1" || process.env.VSCODE_TEST_EXE) {
    assert.ok(hasPscript, `pscript not in language registry (${languages.length} languages)`);
  }
};
