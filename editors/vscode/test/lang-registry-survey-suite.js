const assert = require("assert");

/** Read-only survey: language registry + sample doc languageId (no extension changes). */
exports.run = async () => {
  const vscode = require("vscode");
  const path = require("path");
  const languages = await vscode.languages.getLanguages();
  const samplePath =
    process.env.TEST_PHS_PATH ||
    path.join(__dirname, "..", "..", "..", "test.phs");
  const doc = await vscode.workspace.openTextDocument(vscode.Uri.file(samplePath));

  const thirdPartyIds = (process.env.SURVEY_LANG_IDS || "vue,terraform,kotlin")
    .split(",")
    .map((s) => s.trim())
    .filter(Boolean);
  const thirdParty = Object.fromEntries(
    thirdPartyIds.map((id) => [id, languages.includes(id)]),
  );

  console.log(
    `SURVEY host=${process.env.SURVEY_HOST || "unknown"} languages=${languages.length} pscript=${languages.includes("pscript")} testPhs=${doc.languageId} thirdParty=${JSON.stringify(thirdParty)}`,
  );

  if (process.env.SURVEY_ASSERT_PSCRIPT === "1") {
    assert.ok(languages.includes("pscript"), "pscript not in registry");
  }
};
