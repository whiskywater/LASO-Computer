const fs = require('node:fs');
const path = require('node:path');

const nodeModules = process.argv[2]
  ? path.resolve(process.argv[2])
  : path.join(__dirname, 'node_modules');
const mcpManifest = require(path.join(nodeModules, '@playwright', 'mcp', 'package.json'));
const coreManifest = require(path.join(nodeModules, 'playwright-core', 'package.json'));

if (mcpManifest.version !== '0.0.83' || coreManifest.version !== '1.64.0-alpha-1790635538000') {
  throw new Error('Playwright AppContainer patch only supports the pinned MCP/core versions');
}

const bundle = path.join(nodeModules, 'playwright-core', 'lib', 'coreBundle.js');
let source = fs.readFileSync(bundle, 'utf8');
const original = "return `\\\\\\\\.\\\\pipe\\\\pw-${userNameHash}-${domain}-${name}${suffix}`;";
const patched = "return `\\\\\\\\.\\\\pipe\\\\LOCAL\\\\pw-${userNameHash}-${domain}-${name}${suffix}`;";

if (source.includes(patched)) {
  if (source.split(patched).length !== 2) throw new Error('Unexpected duplicate Playwright pipe definitions');
  process.stdout.write('Playwright AppContainer pipe namespace already patched.\n');
  process.exit(0);
}

if (source.split(original).length !== 2) {
  throw new Error('Pinned Playwright pipe helper did not match the reviewed source pattern');
}
source = source.replace(original, patched);
fs.writeFileSync(bundle, source, 'utf8');
process.stdout.write('Applied the AppContainer LOCAL named-pipe namespace to pinned Playwright core.\n');
