#!/usr/bin/env node
'use strict';

const fs = require('node:fs');
const path = require('node:path');

const projectRoot = path.resolve(__dirname, '..');
const iconRoot = path.join(projectRoot, 'app', 'assets', 'icons');
const catalogPath = path.join(iconRoot, 'catalog.json');

function document(name, data) {
  if (!/^[A-Za-z][A-Za-z0-9]*$/.test(name) || typeof data !== 'string' || !data.trim()) {
    throw new Error(`invalid icon catalog entry: ${name}`);
  }
  return [
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none">',
    `  <title>${name}</title>`,
    `  <path d="${data}" fill="none" stroke="#E9E4DA" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"/>`,
    '</svg>',
    '',
  ].join('\n');
}

function expectedIcons() {
  const catalog = JSON.parse(fs.readFileSync(catalogPath, 'utf8'));
  return new Map(Object.entries(catalog).map(([name, data]) => [name, document(name, data)]));
}

function validateGeneratedIcons() {
  const errors = [];
  const expected = expectedIcons();
  for (const [name, source] of expected) {
    const target = path.join(iconRoot, `${name}.svg`);
    if (!fs.existsSync(target)) errors.push(`missing generated icon ${name}.svg`);
    else if (fs.readFileSync(target, 'utf8') !== source) errors.push(`stale generated icon ${name}.svg`);
  }
  for (const entry of fs.readdirSync(iconRoot)) {
    if (entry.endsWith('.svg') && !expected.has(entry.slice(0, -4))) {
      errors.push(`orphan generated icon ${entry}`);
    }
  }
  return errors;
}

function generateIcons() {
  for (const [name, source] of expectedIcons()) {
    fs.writeFileSync(path.join(iconRoot, `${name}.svg`), source);
  }
}

function main(argv) {
  if (argv.includes('--check')) {
    const errors = validateGeneratedIcons();
    if (errors.length) {
      for (const error of errors) console.error(error);
      return 1;
    }
    console.log(`Motus QML icon set verified (${expectedIcons().size} packaged SVGs).`);
    return 0;
  }
  generateIcons();
  console.log(`Generated ${expectedIcons().size} Motus QML icons.`);
  return 0;
}

if (require.main === module) {
  try {
    process.exitCode = main(process.argv.slice(2));
  } catch (error) {
    console.error(`Motus icon generation failed: ${error.message}`);
    process.exitCode = 1;
  }
}

module.exports = { expectedIcons, generateIcons, validateGeneratedIcons };
