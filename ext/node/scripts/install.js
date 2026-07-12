#!/usr/bin/env node

const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

const packageRoot = path.resolve(__dirname, "..");

function prebuiltPath() {
  return path.join(
    packageRoot,
    "prebuilds",
    `${process.platform}-${process.arch}`,
    "caeneus.node",
  );
}

function localAddonPath() {
  return path.join(packageRoot, "build", "Release", "caeneus.node");
}

function nativeLibraryName() {
  return process.platform === "win32" ? "caeneus-static.lib" : "libcaeneus.a";
}

function sourceBuildArguments() {
  const root =
    process.env.CAENEUS_ROOT || path.resolve(packageRoot, "..", "..");
  const includeDir =
    process.env.CAENEUS_INCLUDE_DIR || path.join(root, "include");
  const libraryDir =
    process.env.CAENEUS_LIBRARY_DIR || path.join(root, "zig-out", "lib");
  return [
    "rebuild",
    `--caeneus_root=${root}`,
    `--caeneus_include_dir=${includeDir}`,
    `--caeneus_lib_dir=${libraryDir}`,
  ];
}

function buildFromSource() {
  const nodeGyp = require.resolve("node-gyp/bin/node-gyp.js");
  const result = spawnSync(process.execPath, [nodeGyp, ...sourceBuildArguments()], {
    cwd: packageRoot,
    stdio: "inherit",
  });
  if (result.error) {
    throw result.error;
  }
  if (result.status !== 0) {
    process.exit(result.status || 1);
  }
}

if (fs.existsSync(prebuiltPath()) || fs.existsSync(localAddonPath())) {
  process.exit(0);
}

const root =
  process.env.CAENEUS_ROOT || path.resolve(packageRoot, "..", "..");
const localLibrary = path.join(
  process.env.CAENEUS_LIBRARY_DIR || path.join(root, "zig-out", "lib"),
  nativeLibraryName(),
);
const shouldBuild =
  process.env.CAENEUS_BUILD_FROM_SOURCE === "1" ||
  (fs.existsSync(path.join(root, "build.zig")) && fs.existsSync(localLibrary));

if (shouldBuild) {
  try {
    buildFromSource();
    process.exit(0);
  } catch (error) {
    console.error("Caeneus source build failed:", error);
    process.exit(1);
  }
}

console.error(
  `No Caeneus native addon for ${process.platform}-${process.arch}. ` +
    "The published package does not support this platform. " +
    "For a source build, install node-gyp and set " +
    "CAENEUS_BUILD_FROM_SOURCE=1, CAENEUS_ROOT, and CAENEUS_LIBRARY_DIR.",
);
process.exit(1);
