import * as fs from "fs";
import * as path from "path";

/** CMake binary dirs under `build/`, searched in this order when resolving tools. */
const PREFERRED_BUILD_SUBDIRS = ["main", "release", "debug"];

export function cmakeBuildRoots(workspaceRoot: string): string[] {
  const parent = path.join(workspaceRoot, "build");
  if (!fs.existsSync(parent)) {
    return [];
  }
  const names = fs.readdirSync(parent).filter((name) => {
    const full = path.join(parent, name);
    try {
      return fs.statSync(full).isDirectory();
    } catch {
      return false;
    }
  });
  names.sort((a, b) => {
    const ia = PREFERRED_BUILD_SUBDIRS.indexOf(a);
    const ib = PREFERRED_BUILD_SUBDIRS.indexOf(b);
    if (ia >= 0 && ib >= 0) {
      return ia - ib;
    }
    if (ia >= 0) {
      return -1;
    }
    if (ib >= 0) {
      return 1;
    }
    return a.localeCompare(b);
  });
  return names.map((name) => path.join(parent, name));
}

export function newestBinaryUnderRoots(
  roots: string[],
  config: "Debug" | "Release",
  fileName: string,
): string | undefined {
  let best: string | undefined;
  let bestMtime = -1;
  for (const root of roots) {
    const candidates = [
      path.join(root, config, fileName),
      path.join(root, config, "Debug", fileName),
    ];
    for (const candidate of candidates) {
      if (!fs.existsSync(candidate)) {
        continue;
      }
      const mtime = fs.statSync(candidate).mtimeMs;
      if (mtime > bestMtime) {
        bestMtime = mtime;
        best = candidate;
      }
    }
  }
  return best;
}

export function canonicalBinaryUnderRoots(
  roots: string[],
  config: "Debug" | "Release",
  fileName: string,
): string | undefined {
  for (const root of roots) {
    const candidate = path.join(root, config, fileName);
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }
  return undefined;
}
