#!/usr/bin/env node
'use strict';

// Builds an evidence-based third-party inventory for the portable Windows
// bundle. The runtime comes from MSYS2 packages, so the package database is the
// only reliable way to connect flattened DLL names back to versions, license
// expressions, upstreams, and exact source-package provenance.

const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const OWN_FILES = new Set(['motus.exe', 'motus-mcp.exe', 'motus-bundle.json', 'qt.conf']);
const GENERATED_PREFIXES = ['licenses/', 'third-party-packages.json', 'third_party_notices.txt'];

function parseSections(text) {
  const result = {};
  let section = '';
  for (const raw of text.replace(/\r/g, '').split('\n')) {
    const marker = raw.match(/^%([^%]+)%$/);
    if (marker) {
      section = marker[1];
      result[section] ??= [];
    } else if (raw === '') {
      section = '';
    } else if (section) {
      result[section].push(raw);
    }
  }
  return result;
}

function first(sections, name) {
  return sections[name]?.[0] ?? '';
}

function loadPackages(databaseRoot) {
  const packages = [];
  for (const entry of fs.readdirSync(databaseRoot, { withFileTypes: true })) {
    if (!entry.isDirectory()) continue;
    const directory = path.join(databaseRoot, entry.name);
    const descPath = path.join(directory, 'desc');
    const filesPath = path.join(directory, 'files');
    if (!fs.existsSync(descPath) || !fs.existsSync(filesPath)) continue;
    const desc = parseSections(fs.readFileSync(descPath, 'utf8'));
    const files = parseSections(fs.readFileSync(filesPath, 'utf8')).FILES ?? [];
    const name = first(desc, 'NAME');
    if (!name.startsWith('mingw-w64-x86_64-')) continue;
    packages.push({
      name,
      version: first(desc, 'VERSION'),
      base: first(desc, 'BASE') || name.replace(/^mingw-w64-x86_64-/, 'mingw-w64-'),
      description: first(desc, 'DESC'),
      upstream: first(desc, 'URL'),
      licenses: desc.LICENSE ?? [],
      buildDate: Number(first(desc, 'BUILDDATE')) || 0,
      files: files.filter((file) => !file.endsWith('/')),
    });
  }
  return packages;
}

function filesBelow(root) {
  const found = [];
  const walk = (directory) => {
    for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
      const full = path.join(directory, entry.name);
      if (entry.isDirectory()) walk(full);
      else found.push(path.relative(root, full).split(path.sep).join('/'));
    }
  };
  walk(root);
  return found;
}

function isGeneratedOrOwned(relative) {
  const lower = relative.toLowerCase();
  return OWN_FILES.has(lower) || GENERATED_PREFIXES.some((prefix) => lower.startsWith(prefix));
}

function ownerCandidates(relative) {
  const candidates = [`mingw64/${relative}`];
  if (!relative.includes('/')) candidates.push(`mingw64/bin/${relative}`);
  // Qt sometimes deploys translations/resources one level above their prefix.
  candidates.push(`mingw64/share/qt6/${relative}`);
  return candidates;
}

function inspectBundle(bundleRoot, packages) {
  const owners = new Map();
  for (const pkg of packages) {
    for (const file of pkg.files) {
      if (owners.has(file) && owners.get(file) !== pkg) {
        throw new Error(`MSYS2 package ownership collision for ${file}`);
      }
      owners.set(file, pkg);
    }
  }

  const used = new Map();
  const unmatched = [];
  for (const relative of filesBelow(bundleRoot)) {
    if (isGeneratedOrOwned(relative)) continue;
    const owner = ownerCandidates(relative).map((candidate) => owners.get(candidate)).find(Boolean);
    if (!owner) {
      unmatched.push(relative);
      continue;
    }
    if (!used.has(owner.name)) used.set(owner.name, { pkg: owner, bundledFiles: [] });
    used.get(owner.name).bundledFiles.push(relative);
  }
  return { used: [...used.values()], unmatched };
}

function sourcePackageUrl(pkg) {
  const versionWithoutEpoch = pkg.version.replace(/^\d+:/, '');
  return `https://mirror.msys2.org/mingw/sources/${pkg.base}-${versionWithoutEpoch}.src.tar.zst`;
}

function runFfmpeg(bundleRoot, argument) {
  const executable = path.join(bundleRoot, 'ffmpeg.exe');
  const result = spawnSync(executable, ['-hide_banner', argument], {
    cwd: bundleRoot,
    encoding: 'utf8',
    windowsHide: true,
  });
  const output = `${result.stdout ?? ''}${result.stderr ?? ''}`.trim();
  if (result.error || result.status !== 0 || !output) {
    throw new Error(`Could not inspect staged FFmpeg (${argument}): ${result.error?.message ?? output}`);
  }
  return output;
}

function copyPackageLicenses(bundleRoot, mingwRoot, used) {
  const generatedRoot = path.join(bundleRoot, 'licenses', 'msys2-packages');
  fs.rmSync(generatedRoot, { recursive: true, force: true });
  fs.mkdirSync(generatedRoot, { recursive: true });

  const copiedByPackage = new Map();
  for (const item of used) {
    const copied = [];
    const prefix = 'mingw64/share/licenses/';
    for (const installed of item.pkg.files.filter((file) => file.startsWith(prefix))) {
      const source = path.join(mingwRoot, installed.slice('mingw64/'.length).split('/').join(path.sep));
      if (!fs.existsSync(source)) continue;
      const relative = installed.slice(prefix.length);
      const destination = path.join(generatedRoot, relative.split('/').join(path.sep));
      fs.mkdirSync(path.dirname(destination), { recursive: true });
      fs.copyFileSync(source, destination);
      copied.push(`licenses/msys2-packages/${relative}`);
    }
    copiedByPackage.set(item.pkg.name, copied.sort());
  }

  // The current FFmpeg and x264 MSYS2 binary packages do not install their own
  // COPYING files. Qt's package contains verbatim GNU license texts, so stage
  // those as common terms and identify their provenance in the inventory.
  const common = [
    ['qt6-base/GPL-3.0-only.txt', 'GPL-3.0.txt'],
    ['qt6-base/GPL-2.0-or-later.txt', 'GPL-2.0-or-later.txt'],
  ];
  for (const [sourceRelative, destinationName] of common) {
    const source = path.join(mingwRoot, 'share', 'licenses', ...sourceRelative.split('/'));
    if (!fs.existsSync(source)) throw new Error(`Required GNU license text is missing: ${source}`);
    const destination = path.join(bundleRoot, 'licenses', destinationName);
    fs.mkdirSync(path.dirname(destination), { recursive: true });
    fs.copyFileSync(source, destination);
  }

  const x264Header = path.join(mingwRoot, 'include', 'x264.h');
  if (!fs.existsSync(x264Header)) throw new Error(`x264 copyright notice is missing: ${x264Header}`);
  const header = fs.readFileSync(x264Header, 'utf8');
  const closing = '*****************************************************************************/';
  const end = header.indexOf(closing);
  if (end < 0 || !header.slice(0, end).includes('GNU General Public License')) {
    throw new Error('Could not extract the installed x264 copyright/license notice.');
  }
  const x264Notice = path.join(bundleRoot, 'licenses', 'x264', 'NOTICE.txt');
  fs.mkdirSync(path.dirname(x264Notice), { recursive: true });
  fs.writeFileSync(x264Notice,
    `Extracted from the x264.h installed with the packaged libx264 build.\n\n${header.slice(0, end + closing.length).trim()}\n`);
  return copiedByPackage;
}

function generateThirdPartyNotices({ bundleRoot, databaseRoot, mingwRoot, ffmpegVersion, ffmpegLicense }) {
  const packages = loadPackages(databaseRoot);
  const { used, unmatched } = inspectBundle(bundleRoot, packages);
  if (unmatched.length) {
    throw new Error(`Could not resolve bundled files to MSYS2 packages:\n  ${unmatched.join('\n  ')}`);
  }
  if (!used.some((item) => item.pkg.name === 'mingw-w64-x86_64-ffmpeg')) {
    throw new Error('The staged FFmpeg executables were not resolved to an MSYS2 package.');
  }
  if (!used.some((item) => item.pkg.name === 'mingw-w64-x86_64-libx264')) {
    throw new Error('The current GPL FFmpeg closure did not resolve its bundled libx264 library.');
  }

  const buildReport = ffmpegVersion ?? runFfmpeg(bundleRoot, '-version');
  const licenseNotice = ffmpegLicense ?? runFfmpeg(bundleRoot, '-L');
  const copiedByPackage = copyPackageLicenses(bundleRoot, mingwRoot, used);
  const packageRecords = used.map(({ pkg, bundledFiles }) => ({
    name: pkg.name,
    version: pkg.version,
    basePackage: pkg.base,
    description: pkg.description,
    upstream: pkg.upstream,
    licenses: pkg.licenses,
    buildDateUtc: pkg.buildDate ? new Date(pkg.buildDate * 1000).toISOString() : '',
    sourcePackage: sourcePackageUrl(pkg),
    bundledFiles: bundledFiles.sort(),
    licenseFiles: copiedByPackage.get(pkg.name) ?? [],
  })).sort((left, right) => left.name.localeCompare(right.name));
  const packagesWithoutInstalledLicenseText = packageRecords
    .filter((pkg) => pkg.licenseFiles.length === 0 &&
      !['mingw-w64-x86_64-ffmpeg', 'mingw-w64-x86_64-libx264'].includes(pkg.name))
    .map((pkg) => pkg.name);

  const inventory = {
    schemaVersion: 1,
    provenance: 'Generated from the local MSYS2 pacman database and the staged PE/runtime tree.',
    distributionStatus: 'Release-blocked pending corresponding-source publication and license-compatibility review.',
    distributionBlockers: [
      'Publish or accompany exact complete corresponding source and build materials for GPL components.',
      'Review the Motus/Qt-plugin/FFmpeg license boundary and release terms.',
      'Acquire authoritative license/notice texts for packages whose MSYS2 binary package installs none.',
    ],
    packagesWithoutInstalledLicenseText,
    commonLicenseTexts: {
      'GPL-3.0-or-later': 'licenses/GPL-3.0.txt',
      'GPL-2.0-or-later': 'licenses/GPL-2.0-or-later.txt',
    },
    ffmpeg: {
      package: 'mingw-w64-x86_64-ffmpeg',
      buildReport,
      licenseNotice,
    },
    packages: packageRecords,
  };

  const lines = [
    'MOTUS THIRD-PARTY SOFTWARE NOTICES',
    '==================================',
    '',
    'This generated inventory records the components actually staged from the local MSYS2',
    'installation. It is factual build provenance, not legal advice. licenses/ contains texts',
    'installed by those packages plus explicit GNU/x264 notices; it is not complete where an',
    'MSYS2 binary package installs no license file.',
    '',
    'DISTRIBUTION STATUS',
    '-------------------',
    'Public distribution is blocked pending two release checks:',
    '1. Publish or accompany the exact complete corresponding source and build materials for',
    '   the GPL components, and make that offer/access available with the binary distribution.',
    '2. Review the application/plugin license boundary and release terms with qualified counsel.',
    '3. Acquire and stage authoritative license/notice texts for every package listed below as',
    '   installing no license file; a metadata label and source URL are not a license text.',
    '',
    'The staged MSYS2 FFmpeg package is labelled GPL-3.0-or-later and its embedded build report',
    'contains --enable-gpl, --enable-version3, and --enable-libx264. The bundle also contains',
    'libx264, whose installed header states GPL version 2 or later. The source-package URLs below',
    'are exact provenance pointers; they do not by themselves certify that Instrumenta has met',
    'its distribution obligations or that those archives will remain available.',
    '',
    'PACKAGES WITHOUT AN INSTALLED LICENSE TEXT',
    '-----------------------------------------',
    ...packagesWithoutInstalledLicenseText.map((name) => `- ${name}`),
    '',
    'FFmpeg EMBEDDED LICENSE NOTICE',
    '------------------------------',
    licenseNotice,
    '',
    'FFmpeg BUILD REPORT',
    '-------------------',
    buildReport,
    '',
    'PACKAGE INVENTORY',
    '-----------------',
  ];
  for (const pkg of packageRecords) {
    lines.push('', `${pkg.name} ${pkg.version}`,
      `  Description: ${pkg.description || '(not recorded)'}`,
      `  Upstream: ${pkg.upstream || '(not recorded)'}`,
      `  License metadata: ${pkg.licenses.join('; ') || '(not recorded)'}`,
      `  Exact MSYS2 source package: ${pkg.sourcePackage}`,
      `  Bundled files (${pkg.bundledFiles.length}): ${pkg.bundledFiles.join(', ')}`,
      `  Package license files: ${pkg.licenseFiles.join(', ') || '(package installs none)'}`);
  }

  fs.writeFileSync(path.join(bundleRoot, 'third-party-packages.json'), `${JSON.stringify(inventory, null, 2)}\n`);
  fs.writeFileSync(path.join(bundleRoot, 'THIRD_PARTY_NOTICES.txt'), `${lines.join('\n')}\n`);
  const ffmpegDirectory = path.join(bundleRoot, 'licenses', 'ffmpeg');
  fs.mkdirSync(ffmpegDirectory, { recursive: true });
  fs.writeFileSync(path.join(ffmpegDirectory, 'BUILD-CONFIGURATION.txt'), `${buildReport}\n`);
  fs.writeFileSync(path.join(ffmpegDirectory, 'NOTICE.txt'), `${licenseNotice}\n`);
  return inventory;
}

function main(argv) {
  const bundleRoot = argv[0];
  let databaseRoot = '';
  let mingwRoot = '';
  for (let index = 1; index < argv.length; index += 1) {
    if (argv[index] === '--pacman-db' && argv[index + 1]) databaseRoot = argv[++index];
    else if (argv[index] === '--mingw-root' && argv[index + 1]) mingwRoot = argv[++index];
  }
  if (!bundleRoot || !databaseRoot || !mingwRoot) {
    console.error('Usage: generate-third-party-notices.cjs <bundle> --pacman-db <dir> --mingw-root <dir>');
    return 2;
  }
  try {
    const inventory = generateThirdPartyNotices({ bundleRoot, databaseRoot, mingwRoot });
    console.log(`Motus third-party inventory generated for ${inventory.packages.length} MSYS2 packages.`);
    return 0;
  } catch (error) {
    console.error(`Motus third-party inventory failed: ${error.message}`);
    return 1;
  }
}

if (require.main === module) process.exit(main(process.argv.slice(2)));

module.exports = { generateThirdPartyNotices, inspectBundle, loadPackages, parseSections, sourcePackageUrl };
