const path = require("path");

async function main() {
  let runTests;
  try {
    runTests = require("@vscode/test-electron").runTests;
  } catch {
    console.error("SKIP lang integration: install @vscode/test-electron to run");
    process.exit(0);
  }

  const extensionDevelopmentPath = process.env.EXT_DEV_PATH
    ? path.resolve(process.env.EXT_DEV_PATH)
    : path.resolve(__dirname, "..");
  const extensionTestsPath = path.resolve(__dirname, "lang-integration-suite.js");
  const vscodeExecutablePath =
    process.env.VSCODE_TEST_EXE ||
    process.env.CURSOR_EXE ||
    "C:\\Users\\emiry\\AppData\\Local\\Programs\\cursor\\Cursor.exe";
  console.log("Using executable:", vscodeExecutablePath);

  await runTests({
    vscodeExecutablePath,
    extensionDevelopmentPath,
    extensionTestsPath,
    launchArgs: ["--disable-extensions", ...(process.env.CURSOR_CLASSIC === "1" ? ["--classic"] : [])],
  });
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
