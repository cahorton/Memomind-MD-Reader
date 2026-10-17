#!/usr/bin/env node
// Puts both halves of MD Reader where MemoMind Desktop Studio looks for them.
//
//   npm run studio [-- --sdk /path/to/plugin-open-platform] [--remove]
//
// Desktop Studio only scans the SDK checkout it imported as its workspace:
//   PhoneSDK/examples/md-reader/   with the package in PhoneSDK/dist/md-reader-<version>.mmpkg
//   GlassSDK/examples/md_reader/   with the package in GlassSDK/build-host/.build/md_reader/
// This copies both plugins there, builds them with the SDK's own tools, and
// replaces earlier copies. Only folders whose manifest carries our plugin id
// are ever replaced or removed. It also copies MemoMind's Web SDK file into
// phone/md-reader/vendor (not committed) and zips samples/vault for import tests.
//
// The SDK defaults to ../plugin-open-platform next to this repo, or set
// MEMOMIND_SDK.

import { execFileSync } from 'node:child_process';
import { cpSync, existsSync, readdirSync, readFileSync, rmSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const sdkArg = args.indexOf('--sdk');
const sdk = resolve(sdkArg >= 0 ? args[sdkArg + 1]
  : process.env.MEMOMIND_SDK || join(root, '..', 'plugin-open-platform'));
const PLUGIN_ID = 'com.memomind.md-reader';
const phoneSource = join(root, 'phone', 'md-reader');
const glassSource = join(root, 'glass', 'md_reader');
const phoneTarget = join(sdk, 'PhoneSDK', 'examples', 'md-reader');
const glassTarget = join(sdk, 'GlassSDK', 'examples', 'md_reader');
const version = JSON.parse(readFileSync(join(phoneSource, 'manifest.json'), 'utf8')).version;

function fail(message) {
  console.error(`studio: ${message}`);
  process.exit(1);
}

function manifestId(dir) {
  try {
    return JSON.parse(readFileSync(join(dir, 'manifest.json'), 'utf8')).id;
  } catch {
    return undefined;
  }
}

/** Remove a previous copy, but only if it is ours. */
function clear(dir) {
  if (!existsSync(dir)) return;
  const found = manifestId(dir);
  if (found !== PLUGIN_ID) fail(`${dir} exists and isn't ours (manifest id ${found ?? 'missing'}); not touching it`);
  rmSync(dir, { recursive: true, force: true });
}

function clearPackages() {
  const dist = join(sdk, 'PhoneSDK', 'dist');
  if (!existsSync(dist)) return;
  for (const name of readdirSync(dist)) {
    if (/^md-reader-\d+\.\d+\.\d+.*\.mmpkg$/u.test(name)) rmSync(join(dist, name));
  }
}

function run(command, commandArgs, cwd, what) {
  try {
    execFileSync(command, commandArgs, { cwd, stdio: 'inherit' });
  } catch {
    fail(`${what} failed. Try it by hand: cd "${cwd}" && ${command} ${commandArgs.join(' ')}`);
  }
}

if (!existsSync(join(sdk, 'PhoneSDK', 'examples')) || !existsSync(join(sdk, 'GlassSDK', 'examples'))) {
  fail(`no MemoMind SDK at ${sdk}. Pass --sdk /path/to/plugin-open-platform or set MEMOMIND_SDK.`);
}

if (args.includes('--remove')) {
  clear(phoneTarget);
  clearPackages();
  clear(glassTarget);
  rmSync(join(sdk, 'GlassSDK', 'build-host', '.build', 'md_reader'), { recursive: true, force: true });
  console.log(`Removed MD Reader from ${sdk}.`);
  process.exit(0);
}

clear(glassTarget);
cpSync(glassSource, glassTarget, { recursive: true, filter: (path) => !path.includes('/.build') });
run('python3', ['build.py', 'build', '--example', 'md_reader'], join(sdk, 'GlassSDK'), 'Building the glasses plugin');
console.log(`Glasses plugin: ${glassTarget}`);

// MemoMind's Web SDK is not redistributed in this repo; take it from the SDK.
const webSdk = join(sdk, 'PhoneSDK', 'examples', 'novel-reader', 'vendor', 'gm-plugin-web-sdk.esm.js');
if (!existsSync(webSdk)) fail(`the Web SDK file is missing at ${webSdk}`);
cpSync(webSdk, join(phoneSource, 'vendor', 'gm-plugin-web-sdk.esm.js'));
clear(phoneTarget);
cpSync(phoneSource, phoneTarget, { recursive: true });
clearPackages();
run('node', ['tools/build-mmpkg.mjs', 'examples/md-reader', `dist/md-reader-${version}.mmpkg`],
  join(sdk, 'PhoneSDK'), 'Packaging the phone plugin');
console.log(`Phone plugin v${version}: ${phoneTarget}`);

rmSync(join(root, 'samples', 'vault.zip'), { force: true });
run('zip', ['-qr', '../vault.zip', '.', '-x', '.*'], join(root, 'samples', 'vault'), 'Zipping the sample vault');
console.log(`Sample vault: ${join(root, 'samples', 'vault.zip')}`);

console.log(`
Next, in Desktop Studio:
  1. Click "Refresh repository" on both sides.
  2. Phone plugin: MD Reader ${version}. Glasses plugin: MD Reader.
  3. In the phone plugin, add the notes in samples/vault (or samples/vault.zip).
To undo: npm run studio -- --remove`);
