#!/usr/bin/env node
'use strict';

// Completes and verifies the portable Motus Windows bundle.
//
// Qt's deployment step copies the Qt libraries, QML modules, and plugins, but it
// does not copy the MSYS2/MinGW libraries those binaries import (libpcre2-16-0,
// zlib1, libzstd, libbrotli*, libfreetype, libharfbuzz, and friends). On a
// developer machine the loader still finds them through `C:\msys64\mingw64\bin`
// on PATH; on any other computer the process dies with 0xC0000135 before it can
// report anything. This walks the real PE import tables of every executable and
// DLL in the bundle, copies the transitive closure beside motus.exe, and can
// prove the closure is complete without launching anything.
//
//   node scripts/bundle-runtime.cjs complete <bundle> [--search <dir>]... [--flatten]
//   node scripts/bundle-runtime.cjs check <bundle>
//
// `check` is the portable, offline gate: it exits non-zero and names the exact
// missing DLL and its importer instead of surfacing an opaque Windows status.

const fs = require('node:fs');
const path = require('node:path');

const SYSTEM_PREFIXES = ['api-ms-win-', 'ext-ms-win-'];

// Windows-provided libraries that must never be bundled. Anything outside this
// list that is not already in the bundle is a real missing dependency.
const SYSTEM_DLLS = new Set([
  'advapi32.dll', 'authz.dll', 'avicap32.dll', 'avrt.dll', 'bcrypt.dll', 'bcryptprimitives.dll',
  'cabinet.dll', 'cfgmgr32.dll', 'combase.dll', 'comctl32.dll', 'comdlg32.dll', 'crypt32.dll',
  'd2d1.dll', 'd3d9.dll', 'd3d11.dll', 'd3d12.dll', 'dbghelp.dll', 'dcomp.dll', 'dnsapi.dll',
  'dwmapi.dll', 'dwrite.dll', 'dxgi.dll', 'dxva2.dll', 'evr.dll', 'gdi32.dll', 'gdiplus.dll',
  'hid.dll', 'imm32.dll', 'iphlpapi.dll', 'kernel32.dll', 'kernelbase.dll', 'ksuser.dll',
  'mf.dll', 'mfcore.dll', 'mfplat.dll', 'mfreadwrite.dll', 'mpr.dll', 'msimg32.dll',
  'msvcrt.dll', 'mswsock.dll', 'ncrypt.dll', 'netapi32.dll', 'normaliz.dll', 'ntdll.dll',
  'ole32.dll', 'oleacc.dll', 'oleaut32.dll', 'opengl32.dll', 'glu32.dll', 'powrprof.dll',
  'propsys.dll', 'psapi.dll', 'rpcrt4.dll', 'schannel.dll', 'secur32.dll', 'sechost.dll',
  'setupapi.dll', 'shcore.dll', 'shell32.dll', 'shlwapi.dll', 'ucrtbase.dll',
  'uiautomationcore.dll', 'user32.dll', 'userenv.dll', 'usp10.dll', 'uxtheme.dll',
  'version.dll', 'wer.dll', 'wevtapi.dll', 'winhttp.dll', 'wininet.dll', 'winmm.dll',
  'winspool.drv', 'wintrust.dll', 'wldap32.dll', 'ws2_32.dll', 'wsock32.dll', 'wtsapi32.dll',
]);

const DEFAULT_SEARCH = ['C:\\msys64\\mingw64\\bin'];

function isSystemDll(name) {
  const lower = name.toLowerCase();
  return SYSTEM_DLLS.has(lower) || SYSTEM_PREFIXES.some((prefix) => lower.startsWith(prefix));
}

function readUInt32(buffer, offset) {
  return offset + 4 <= buffer.length ? buffer.readUInt32LE(offset) : 0;
}

// Minimal PE reader: enough of the headers to reach the import and delay-import
// directories and translate their RVAs into file offsets.
function importedLibraries(file) {
  let buffer;
  try {
    buffer = fs.readFileSync(file);
  } catch {
    return [];
  }
  if (buffer.length < 0x40 || buffer.readUInt16LE(0) !== 0x5a4d) return [];
  const peOffset = readUInt32(buffer, 0x3c);
  if (peOffset + 24 > buffer.length || readUInt32(buffer, peOffset) !== 0x00004550) return [];

  const coff = peOffset + 4;
  const sectionCount = buffer.readUInt16LE(coff + 2);
  const optionalSize = buffer.readUInt16LE(coff + 16);
  const optional = coff + 20;
  const magic = buffer.readUInt16LE(optional);
  const plus = magic === 0x20b;
  const directories = optional + (plus ? 112 : 96);
  const sections = optional + optionalSize;

  const map = [];
  for (let index = 0; index < sectionCount; index += 1) {
    const header = sections + index * 40;
    if (header + 40 > buffer.length) break;
    map.push({
      virtualSize: readUInt32(buffer, header + 8),
      virtualAddress: readUInt32(buffer, header + 12),
      rawSize: readUInt32(buffer, header + 16),
      rawOffset: readUInt32(buffer, header + 20),
    });
  }

  const toOffset = (rva) => {
    for (const section of map) {
      const size = Math.max(section.virtualSize, section.rawSize);
      if (rva >= section.virtualAddress && rva < section.virtualAddress + size) {
        return section.rawOffset + (rva - section.virtualAddress);
      }
    }
    return -1;
  };

  const readName = (rva) => {
    const offset = toOffset(rva);
    if (offset < 0 || offset >= buffer.length) return '';
    const end = buffer.indexOf(0, offset);
    return buffer.toString('latin1', offset, end < 0 ? buffer.length : end);
  };

  const names = new Set();
  // Import directory (entry 1): 20-byte descriptors, name RVA at +12.
  // Delay-import directory (entry 13): 32-byte descriptors, name RVA at +4.
  for (const [entry, stride, nameField] of [[1, 20, 12], [13, 32, 4]]) {
    const directory = directories + entry * 8;
    if (directory + 8 > buffer.length) continue;
    const tableOffset = toOffset(readUInt32(buffer, directory));
    if (tableOffset < 0) continue;
    for (let cursor = tableOffset; cursor + stride <= buffer.length; cursor += stride) {
      const slice = buffer.subarray(cursor, cursor + stride);
      if (slice.every((byte) => byte === 0)) break;
      const name = readName(readUInt32(buffer, cursor + nameField));
      if (name && /\.(dll|drv|ocx)$/i.test(name)) names.add(name);
    }
  }
  return [...names];
}

// Returns the numeric root resource types embedded in a PE image. Windows shell
// icons require both RT_ICON (3) image resources and an RT_GROUP_ICON (14)
// directory; QApplication's runtime SVG cannot supply either one.
function resourceTypeIds(file) {
  let buffer;
  try {
    buffer = fs.readFileSync(file);
  } catch {
    return [];
  }
  if (buffer.length < 0x40 || buffer.readUInt16LE(0) !== 0x5a4d) return [];
  const peOffset = readUInt32(buffer, 0x3c);
  if (peOffset + 24 > buffer.length || readUInt32(buffer, peOffset) !== 0x00004550) return [];

  const coff = peOffset + 4;
  const sectionCount = buffer.readUInt16LE(coff + 2);
  const optionalSize = buffer.readUInt16LE(coff + 16);
  const optional = coff + 20;
  if (optional + optionalSize > buffer.length) return [];
  const magic = buffer.readUInt16LE(optional);
  const directories = optional + (magic === 0x20b ? 112 : magic === 0x10b ? 96 : optionalSize);
  const sections = optional + optionalSize;
  if (directories + 3 * 8 > sections) return [];

  const map = [];
  for (let index = 0; index < sectionCount; index += 1) {
    const header = sections + index * 40;
    if (header + 40 > buffer.length) break;
    map.push({
      virtualSize: readUInt32(buffer, header + 8),
      virtualAddress: readUInt32(buffer, header + 12),
      rawSize: readUInt32(buffer, header + 16),
      rawOffset: readUInt32(buffer, header + 20),
    });
  }
  const resourceRva = readUInt32(buffer, directories + 2 * 8);
  if (!resourceRva) return [];
  const section = map.find((candidate) => {
    const size = Math.max(candidate.virtualSize, candidate.rawSize);
    return resourceRva >= candidate.virtualAddress &&
      resourceRva < candidate.virtualAddress + size;
  });
  if (!section) return [];
  const root = section.rawOffset + (resourceRva - section.virtualAddress);
  if (root < 0 || root + 16 > buffer.length) return [];
  const entries = buffer.readUInt16LE(root + 12) + buffer.readUInt16LE(root + 14);
  const types = [];
  for (let index = 0; index < entries; index += 1) {
    const entry = root + 16 + index * 8;
    if (entry + 8 > buffer.length) break;
    const identifier = readUInt32(buffer, entry);
    if ((identifier & 0x80000000) === 0) types.push(identifier & 0xffff);
  }
  return [...new Set(types)].sort((left, right) => left - right);
}

function binariesIn(root) {
  const found = [];
  const walk = (directory) => {
    let entries;
    try {
      entries = fs.readdirSync(directory, { withFileTypes: true });
    } catch {
      return;
    }
    for (const entry of entries) {
      const full = path.join(directory, entry.name);
      if (entry.isDirectory()) walk(full);
      else if (/\.(dll|exe)$/i.test(entry.name)) found.push(full);
    }
  };
  walk(root);
  return found;
}

function filesIn(root) {
  const found = [];
  const walk = (directory) => {
    let entries;
    try {
      entries = fs.readdirSync(directory, { withFileTypes: true });
    } catch {
      return;
    }
    for (const entry of entries) {
      const full = path.join(directory, entry.name);
      if (entry.isDirectory()) walk(full);
      else found.push(full);
    }
  };
  walk(root);
  return found;
}

// Resolves an imported name the way the Windows loader will for this bundle:
// beside the executable first, then anywhere else already inside the bundle.
function bundleIndex(root) {
  const index = new Map();
  for (const file of binariesIn(root)) {
    const key = path.basename(file).toLowerCase();
    const atRoot = path.dirname(file) === path.resolve(root);
    if (!index.has(key) || atRoot) index.set(key, file);
  }
  return index;
}

function resolveInSearch(name, searchPaths) {
  for (const directory of searchPaths) {
    const candidate = path.join(directory, name);
    try {
      if (fs.statSync(candidate).isFile()) return candidate;
    } catch { /* keep looking */ }
  }
  return '';
}

// Walks the whole bundle, following imports until nothing new appears.
function analyse(root, searchPaths) {
  const resolvedRoot = path.resolve(root);
  const index = bundleIndex(resolvedRoot);
  const copied = [];
  const missing = [];
  const queue = binariesIn(resolvedRoot);
  const inspected = new Set();

  while (queue.length) {
    const file = queue.shift();
    const key = path.resolve(file).toLowerCase();
    if (inspected.has(key)) continue;
    inspected.add(key);

    for (const name of importedLibraries(file)) {
      const lower = name.toLowerCase();
      if (index.has(lower) || isSystemDll(name)) continue;
      const source = resolveInSearch(name, searchPaths);
      if (!source) {
        missing.push({ name, importer: path.relative(resolvedRoot, file) });
        index.set(lower, ''); // report each missing library once
        continue;
      }
      const destination = path.join(resolvedRoot, path.basename(source));
      copied.push({ name, source, destination, importer: path.relative(resolvedRoot, file) });
      index.set(lower, destination);
      queue.push(source); // the copy inherits the source's own imports
    }
  }
  return { copied, missing };
}

function complete(root, searchPaths, flatten) {
  const resolvedRoot = path.resolve(root);
  if (flatten) {
    // Qt's deploy script leaves a second copy of every Qt library under `bin`.
    // motus.exe sits at the bundle root and never reads it, so collapsing the
    // duplicate halves the shipped bundle.
    const binDirectory = path.join(resolvedRoot, 'bin');
    if (fs.existsSync(binDirectory)) {
      for (const entry of fs.readdirSync(binDirectory, { withFileTypes: true })) {
        if (!entry.isFile() || !/\.(dll|exe)$/i.test(entry.name)) continue;
        const target = path.join(resolvedRoot, entry.name);
        if (!fs.existsSync(target)) fs.copyFileSync(path.join(binDirectory, entry.name), target);
      }
      fs.rmSync(binDirectory, { recursive: true, force: true });
    }
  }

  const { copied, missing } = analyse(resolvedRoot, searchPaths);
  for (const item of copied) fs.copyFileSync(item.source, item.destination);

  if (missing.length) {
    console.error('\nMotus runtime libraries could not be located:');
    for (const item of missing) console.error(`  ${item.name} (imported by ${item.importer})`);
    console.error(`\nSearched: ${searchPaths.join(', ') || '(nothing)'}`);
    return 1;
  }
  console.log(`Motus runtime closure complete: ${copied.length} support librar${copied.length === 1 ? 'y' : 'ies'} added.`);
  for (const item of copied) console.log(`  + ${path.basename(item.destination)}`);
  return 0;
}

function noticeErrors(root) {
  const errors = [];
  const notice = path.join(root, 'THIRD_PARTY_NOTICES.txt');
  const inventoryPath = path.join(root, 'third-party-packages.json');
  for (const required of [notice, inventoryPath]) {
    if (!fs.existsSync(required)) errors.push(`missing ${path.basename(required)}`);
  }
  if (errors.length) return errors;

  let inventory;
  try {
    inventory = JSON.parse(fs.readFileSync(inventoryPath, 'utf8'));
  } catch (error) {
    return [`third-party-packages.json is invalid: ${error.message}`];
  }
  if (inventory.schemaVersion !== 1 || !Array.isArray(inventory.packages)) {
    errors.push('third-party-packages.json has an unsupported schema');
    return errors;
  }

  const recorded = new Set();
  for (const pkg of inventory.packages) {
    if (!pkg.name || !pkg.version || !Array.isArray(pkg.bundledFiles) || !Array.isArray(pkg.licenses)) {
      errors.push('third-party package entry is incomplete');
      continue;
    }
    for (const relative of pkg.bundledFiles) {
      const normalized = relative.split('\\').join('/').toLowerCase();
      recorded.add(normalized);
      if (!fs.existsSync(path.join(root, ...relative.split('/')))) {
        errors.push(`inventory records absent bundle file ${relative}`);
      }
    }
    for (const relative of pkg.licenseFiles ?? []) {
      if (!fs.existsSync(path.join(root, ...relative.split('/')))) {
        errors.push(`inventory records absent license file ${relative}`);
      }
    }
  }

  const own = new Set(['motus.exe', 'motus-mcp.exe', 'motus-bundle.json', 'qt.conf']);
  const generated = ['licenses/', 'third-party-packages.json', 'third_party_notices.txt'];
  for (const file of filesIn(root)) {
    const relative = path.relative(root, file).split(path.sep).join('/').toLowerCase();
    if (!own.has(relative) && !generated.some((prefix) => relative.startsWith(prefix)) &&
        !recorded.has(relative)) {
      errors.push(`third-party bundle file is absent from inventory: ${relative}`);
    }
  }

  const ffmpeg = inventory.packages.find((pkg) =>
    pkg.bundledFiles.includes('ffmpeg.exe') && pkg.bundledFiles.includes('ffprobe.exe'));
  if (!ffmpeg) errors.push('FFmpeg executables have no package/source provenance');
  if (!inventory.ffmpeg?.buildReport || !inventory.ffmpeg?.licenseNotice) {
    errors.push('FFmpeg embedded build and license reports are missing');
  }
  for (const relative of Object.values(inventory.commonLicenseTexts ?? {})) {
    if (!fs.existsSync(path.join(root, ...relative.split('/')))) {
      errors.push(`common license text is absent: ${relative}`);
    }
  }
  return errors;
}

function check(root) {
  const nativeIconTypes = resourceTypeIds(path.join(root, 'motus.exe'));
  if (!nativeIconTypes.includes(3) || !nativeIconTypes.includes(14)) {
    console.error('\nThe Motus executable has no complete native Windows icon resource.');
    console.error('Rebuild from app/assets/motus.ico so Explorer, shortcuts, and the taskbar use the Motus mark.');
    return 1;
  }
  const requiredTools = ['ffmpeg.exe', 'ffprobe.exe'];
  const absentTools = requiredTools.filter((name) => !fs.existsSync(path.join(root, name)));
  if (absentTools.length) {
    console.error(`\nThe Motus bundle is missing native media tools: ${absentTools.join(', ')}`);
    console.error('Re-run the Motus bootstrap so probe and export do not depend on developer PATH.');
    return 1;
  }
  const notices = noticeErrors(root);
  if (notices.length) {
    console.error('\nThe Motus bundle has incomplete third-party provenance/notices:');
    for (const error of notices) console.error(`  ${error}`);
    console.error('Re-run the Motus bootstrap; do not publish a bundle without this inventory.');
    return 1;
  }
  const { missing } = analyse(root, []);
  if (missing.length) {
    console.error('\nThe Motus bundle is not self-contained. Missing libraries:');
    for (const item of missing) console.error(`  ${item.name} (imported by ${item.importer})`);
    console.error('\nRun the Motus bootstrap again to rebuild a complete bundle.');
    return 1;
  }
  console.log('Motus bundle dependency closure verified; no external libraries are required.');
  return 0;
}

function main(argv) {
  const command = argv[0];
  const root = argv[1];
  if (!command || !root || !['complete', 'check'].includes(command)) {
    console.error('Usage: bundle-runtime.cjs <complete|check> <bundle> [--search <dir>]... [--flatten]');
    return 2;
  }
  if (!fs.existsSync(root)) {
    console.error(`Bundle directory not found: ${root}`);
    return 2;
  }
  const searchPaths = [];
  let flatten = false;
  for (let index = 2; index < argv.length; index += 1) {
    if (argv[index] === '--search' && argv[index + 1]) searchPaths.push(argv[index += 1]);
    else if (argv[index] === '--flatten') flatten = true;
  }
  if (command === 'check') return check(root);
  return complete(root, searchPaths.length ? searchPaths : DEFAULT_SEARCH, flatten);
}

if (require.main === module) process.exit(main(process.argv.slice(2)));

module.exports = {
  analyse,
  check: (root) => check(root) === 0,
  importedLibraries,
  isSystemDll,
  noticeErrors,
  resourceTypeIds,
};
