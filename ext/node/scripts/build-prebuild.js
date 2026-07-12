#!/usr/bin/env node

const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

const packageRoot = path.resolve(__dirname, "..");
const target = process.argv[2] || `${process.platform}-${process.arch}`;
const allowedTargets = new Set([
  "linux-x64",
  "linux-arm64",
  "darwin-arm64",
  "win32-x64",
]);

if (!allowedTargets.has(target)) {
  throw new Error(`unsupported prebuild target: ${target}`);
}

const root =
  process.env.CAENEUS_ROOT || path.resolve(packageRoot, "..", "..");
const includeDir =
  process.env.CAENEUS_INCLUDE_DIR || path.join(root, "include");
const libraryDir =
  process.env.CAENEUS_LIBRARY_DIR || path.join(root, "zig-out", "lib");
const nodeGyp = require.resolve("node-gyp/bin/node-gyp.js");
const result = spawnSync(
  process.execPath,
  [
    nodeGyp,
    "rebuild",
    `--caeneus_root=${root}`,
    `--caeneus_include_dir=${includeDir}`,
    `--caeneus_lib_dir=${libraryDir}`,
  ],
  { cwd: packageRoot, stdio: "inherit" },
);

if (result.error) {
  throw result.error;
}
if (result.status !== 0) {
  process.exit(result.status || 1);
}

const destination = path.join(packageRoot, "prebuilds", target);
fs.mkdirSync(destination, { recursive: true });
fs.copyFileSync(
  path.join(packageRoot, "build", "Release", "caeneus.node"),
  path.join(destination, "caeneus.node"),
);
console.log(`wrote ${path.join("prebuilds", target, "caeneus.node")}`);
