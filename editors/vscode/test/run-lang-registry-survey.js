const path = require("path");

async function main() {
  const runTests = require("@vscode/test-electron").runTests;
  const extensionDevelopmentPath = process.env.EXT_DEV_PATH
    ? path.resolve(process.env.EXT_DEV_PATH)
    : path.resolve(__dirname, "..");
  const extensionTestsPath = path.resolve(__dirname, "lang-registry-survey-suite.js");
  const vscodeExecutablePath =
    process.env.VSCODE_TEST_EXE ||
    process.env.CURSOR_EXE ||
    "C:\\Users\\emiry\\AppData\\Local\\Programs\\cursor\\Cursor.exe";

  const disableExtensions = process.env.SURVEY_DISABLE_EXTENSIONS !== "0";
  const launchArgs = [
    ...(disableExtensions ? ["--disable-extensions"] : []),
    ...(process.env.CURSOR_CLASSIC === "1" ? ["--classic"] : []),
  ];

  console.log("Survey executable:", vscodeExecutablePath);
  console.log("Survey launchArgs:", launchArgs.join(" "));

  await runTests({
    vscodeExecutablePath,
    extensionDevelopmentPath,
    extensionTestsPath,
    launchArgs,
  });
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
