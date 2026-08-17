'use strict';

// Tests for the portable-bundle runtime collector. These build synthetic PE
// files so the import-table walk is exercised deterministically on any host,
// including Linux CI, without needing a Windows toolchain or a Qt deployment.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { test } = require('node:test');

const {
  analyse,
  check,
  importedLibraries,
  isSystemDll,
  resourceTypeIds,
} = require('../scripts/bundle-runtime.cjs');
const {
  generateThirdPartyNotices,
  inspectBundle,
  loadPackages,
  parseSections,
  sourcePackageUrl,
} = require('../scripts/generate-third-party-notices.cjs');
const {
  expectedIcons,
  validateGeneratedIcons,
} = require('../scripts/generate-qml-icons.cjs');

const SECTION_RVA = 0x1000;
const SECTION_OFFSET = 0x400;

// Minimal PE32+ image whose import directory names the given libraries.
function synthesizePe(libraries, resourceTypes = []) {
  const image = Buffer.alloc(0x1000, 0);
  image.writeUInt16LE(0x5a4d, 0); // MZ
  image.writeUInt32LE(0x80, 0x3c); // e_lfanew
  const pe = 0x80;
  image.writeUInt32LE(0x00004550, pe); // PE\0\0
  const coff = pe + 4;
  image.writeUInt16LE(0x8664, coff); // machine: x64
  image.writeUInt16LE(1, coff + 2); // one section
  image.writeUInt16LE(240, coff + 16); // optional header size
  const optional = coff + 20;
  image.writeUInt16LE(0x20b, optional); // PE32+
  const directories = optional + 112;
  const sections = optional + 240;

  // The import descriptors and the name strings share one section.
  const descriptorsRva = SECTION_RVA;
  const descriptorSize = 20;
  let nameCursor = descriptorsRva + descriptorSize * (libraries.length + 1);
  libraries.forEach((library, index) => {
    const descriptor = SECTION_OFFSET + index * descriptorSize;
    image.writeUInt32LE(nameCursor, descriptor + 12); // Name RVA
    image.writeUInt32LE(1, descriptor); // non-zero so the terminator is unambiguous
    const nameOffset = SECTION_OFFSET + (nameCursor - SECTION_RVA);
    image.write(library, nameOffset, 'latin1');
    nameCursor += library.length + 1;
  });

  image.writeUInt32LE(descriptorsRva, directories + 1 * 8); // import directory RVA
  image.writeUInt32LE(descriptorSize * (libraries.length + 1), directories + 1 * 8 + 4);

  if (resourceTypes.length) {
    const resourcesRva = SECTION_RVA + 0x300;
    const resourcesOffset = SECTION_OFFSET + 0x300;
    image.writeUInt16LE(resourceTypes.length, resourcesOffset + 14); // numeric root entries
    resourceTypes.forEach((type, index) => {
      const entry = resourcesOffset + 16 + index * 8;
      image.writeUInt32LE(type, entry);
      image.writeUInt32LE(0x80000040 + index * 0x20, entry + 4);
    });
    image.writeUInt32LE(resourcesRva, directories + 2 * 8);
    image.writeUInt32LE(0x100, directories + 2 * 8 + 4);
  }

  image.write('.rdata', sections, 'latin1');
  image.writeUInt32LE(0x400, sections + 8); // virtual size
  image.writeUInt32LE(SECTION_RVA, sections + 12); // virtual address
  image.writeUInt32LE(0x400, sections + 16); // raw size
  image.writeUInt32LE(SECTION_OFFSET, sections + 20); // raw offset
  return image;
}

function workspace() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'motus-bundle-test-'));
  return {
    root,
    write(relative, libraries, resourceTypes = []) {
      const target = path.join(root, relative);
      fs.mkdirSync(path.dirname(target), { recursive: true });
      fs.writeFileSync(target, synthesizePe(libraries, resourceTypes));
      return target;
    },
    dispose() {
      fs.rmSync(root, { recursive: true, force: true });
    },
  };
}

function writePackage(database, pkg) {
  const directory = path.join(database, `${pkg.name}-${pkg.version}`);
  fs.mkdirSync(directory, { recursive: true });
  const section = (name, values) => `%${name}%\n${values.join('\n')}\n\n`;
  fs.writeFileSync(path.join(directory, 'desc'), [
    section('NAME', [pkg.name]),
    section('VERSION', [pkg.version]),
    section('BASE', [pkg.base]),
    section('DESC', [pkg.description ?? pkg.name]),
    section('URL', [pkg.upstream ?? 'https://example.invalid/']),
    section('BUILDDATE', ['1700000000']),
    section('LICENSE', pkg.licenses),
  ].join(''));
  fs.writeFileSync(path.join(directory, 'files'), section('FILES', pkg.files));
}

test('import table names are read from a PE image', () => {
  const area = workspace();
  try {
    const file = area.write('motus.exe', ['Qt6Core.dll', 'KERNEL32.dll']);
    assert.deepEqual(importedLibraries(file).sort(), ['KERNEL32.dll', 'Qt6Core.dll']);
  } finally {
    area.dispose();
  }
});

test('non-PE and truncated files are ignored rather than throwing', () => {
  const area = workspace();
  try {
    const text = path.join(area.root, 'notes.dll');
    fs.writeFileSync(text, 'this is not an executable');
    assert.deepEqual(importedLibraries(text), []);
    assert.deepEqual(importedLibraries(path.join(area.root, 'absent.dll')), []);
  } finally {
    area.dispose();
  }
});

test('native Windows icon resources are distinguished from a runtime window icon', () => {
  const area = workspace();
  try {
    const unbranded = area.write('unbranded.exe', [], [24]);
    const branded = area.write('branded.exe', [], [3, 14, 24]);
    assert.deepEqual(resourceTypeIds(unbranded), [24]);
    assert.deepEqual(resourceTypeIds(branded), [3, 14, 24]);
  } finally {
    area.dispose();
  }
});

test('every QML icon is a current packaged SVG asset rather than a runtime Shape path', () => {
  assert.deepEqual(validateGeneratedIcons(), []);
  const icons = expectedIcons();
  for (const required of [
    'folder', 'save', 'import', 'undo', 'redo', 'cut', 'delete', 'graph', 'export',
    'play', 'pause', 'stepBack', 'stepForward', 'marker', 'magnet', 'zoomIn',
    'zoomOut', 'lock', 'unlock', 'eye', 'eyeOff', 'volume', 'mute', 'relink',
  ]) assert.ok(icons.has(required), `missing ${required} icon`);
  const qml = fs.readFileSync(path.join(__dirname, '..', 'app', 'qml', 'Main.qml'), 'utf8');
  assert.doesNotMatch(qml, /QtQuick\.Shapes|PathSvg|ShapePath/);
  assert.match(qml, /qrc:\/qt\/qml\/Motus\/app\/assets\/icons\//);
  for (const match of qml.matchAll(/iconName:\s*"([A-Za-z][A-Za-z0-9]*)"/g)) {
    assert.ok(icons.has(match[1]), `QML references absent icon ${match[1]}`);
  }
});

test('Windows-supplied libraries are never treated as bundle dependencies', () => {
  assert.ok(isSystemDll('KERNEL32.dll'));
  assert.ok(isSystemDll('api-ms-win-crt-runtime-l1-1-0.dll'));
  assert.ok(isSystemDll('winspool.drv'));
  assert.ok(!isSystemDll('Qt6Core.dll'));
  assert.ok(!isSystemDll('libpcre2-16-0.dll'));
});

test('a bundle missing a support library is reported with its importer', () => {
  const area = workspace();
  try {
    area.write('motus.exe', ['Qt6Core.dll']);
    area.write('Qt6Core.dll', ['libpcre2-16-0.dll', 'KERNEL32.dll']);
    const { copied, missing } = analyse(area.root, []);
    assert.equal(copied.length, 0);
    assert.deepEqual(missing.map((item) => item.name), ['libpcre2-16-0.dll']);
    assert.equal(missing[0].importer, 'Qt6Core.dll');
  } finally {
    area.dispose();
  }
});

test('transitive dependencies are collected from the search path', () => {
  const area = workspace();
  const supply = workspace();
  try {
    area.write('motus.exe', ['Qt6Core.dll']);
    area.write('Qt6Core.dll', ['libpcre2-16-0.dll']);
    supply.write('libpcre2-16-0.dll', ['libzstd.dll', 'KERNEL32.dll']);
    supply.write('libzstd.dll', ['KERNEL32.dll']);

    const { copied, missing } = analyse(area.root, [supply.root]);
    assert.deepEqual(missing, []);
    // libzstd is reached only through the library that was itself just copied.
    assert.deepEqual(copied.map((item) => item.name).sort(), ['libpcre2-16-0.dll', 'libzstd.dll']);
    for (const item of copied) assert.equal(path.dirname(item.destination), path.resolve(area.root));
  } finally {
    area.dispose();
    supply.dispose();
  }
});

test('plugins and QML modules are walked, not only the bundle root', () => {
  const area = workspace();
  const supply = workspace();
  try {
    area.write('motus.exe', [], [3, 14, 24]);
    area.write(path.join('share', 'qt6', 'plugins', 'imageformats', 'qjpeg.dll'), ['libjpeg-8.dll']);
    supply.write('libjpeg-8.dll', []);

    const { copied, missing } = analyse(area.root, [supply.root]);
    assert.deepEqual(missing, []);
    assert.deepEqual(copied.map((item) => item.name), ['libjpeg-8.dll']);
    assert.equal(copied[0].importer, path.join('share', 'qt6', 'plugins', 'imageformats', 'qjpeg.dll'));
  } finally {
    area.dispose();
    supply.dispose();
  }
});

test('libraries already present anywhere in the bundle are not re-copied', () => {
  const area = workspace();
  const supply = workspace();
  try {
    area.write('motus.exe', ['Qt6Core.dll']);
    area.write('Qt6Core.dll', []);
    supply.write('Qt6Core.dll', []);
    const { copied, missing } = analyse(area.root, [supply.root]);
    assert.deepEqual(copied, []);
    assert.deepEqual(missing, []);
  } finally {
    area.dispose();
    supply.dispose();
  }
});

test('portable capability check requires bundled ffmpeg and ffprobe executables', () => {
  const area = workspace();
  try {
    area.write('motus.exe', [], [3, 14, 24]);
    assert.equal(check(area.root), false);
    area.write('ffmpeg.exe', []);
    area.write('ffprobe.exe', []);
    // Native binaries without package/source provenance are still not a
    // publishable portable bundle.
    assert.equal(check(area.root), false);
  } finally {
    area.dispose();
  }
});

test('pacman sections preserve multi-value license and file fields', () => {
  assert.deepEqual(parseSections('%NAME%\nexample\n\n%LICENSE%\nMIT\nBSD-3-Clause\n\n'), {
    NAME: ['example'],
    LICENSE: ['MIT', 'BSD-3-Clause'],
  });
  assert.equal(sourcePackageUrl({ base: 'mingw-w64-example', version: '2:1.2.3-4' }),
    'https://mirror.msys2.org/mingw/sources/mingw-w64-example-1.2.3-4.src.tar.zst');
});

test('generated notices inventory every staged runtime binary and preserve GPL evidence', () => {
  const area = workspace();
  const database = workspace();
  const mingw = workspace();
  try {
    area.write('motus.exe', [], [3, 14, 24]);
    area.write('ffmpeg.exe', []);
    area.write('ffprobe.exe', []);
    area.write('libx264-165.dll', []);
    area.write('Qt6Core.dll', []);

    writePackage(database.root, {
      name: 'mingw-w64-x86_64-ffmpeg', version: '9.0-2', base: 'mingw-w64-ffmpeg',
      licenses: ['spdx:GPL-3.0-or-later'],
      files: ['mingw64/bin/ffmpeg.exe', 'mingw64/bin/ffprobe.exe'],
    });
    writePackage(database.root, {
      name: 'mingw-w64-x86_64-libx264', version: '0.165.r3222.b35605a-3', base: 'mingw-w64-x264',
      licenses: ['custom'], files: ['mingw64/bin/libx264-165.dll'],
    });
    writePackage(database.root, {
      name: 'mingw-w64-x86_64-qt6-base', version: '6.11.1-1', base: 'mingw-w64-qt6-base',
      licenses: ['spdx:LGPL-3.0-only'],
      files: [
        'mingw64/bin/Qt6Core.dll',
        'mingw64/share/licenses/qt6-base/GPL-3.0-only.txt',
        'mingw64/share/licenses/qt6-base/GPL-2.0-or-later.txt',
      ],
    });
    const licenseRoot = path.join(mingw.root, 'share', 'licenses', 'qt6-base');
    fs.mkdirSync(licenseRoot, { recursive: true });
    fs.writeFileSync(path.join(licenseRoot, 'GPL-3.0-only.txt'), 'GNU GPL version 3 test fixture\n');
    fs.writeFileSync(path.join(licenseRoot, 'GPL-2.0-or-later.txt'), 'GNU GPL version 2 test fixture\n');
    fs.mkdirSync(path.join(mingw.root, 'include'), { recursive: true });
    fs.writeFileSync(path.join(mingw.root, 'include', 'x264.h'), [
      '/*****************************************************************************',
      ' * Copyright (C) 2003-2025 x264 project',
      ' * GNU General Public License version 2 or later',
      ' *****************************************************************************/',
      '',
    ].join('\n'));

    const loaded = loadPackages(database.root);
    assert.equal(loaded.length, 3);
    assert.deepEqual(inspectBundle(area.root, loaded).unmatched, []);
    const inventory = generateThirdPartyNotices({
      bundleRoot: area.root,
      databaseRoot: database.root,
      mingwRoot: mingw.root,
      ffmpegVersion: 'ffmpeg version 9.0\nconfiguration: --enable-gpl --enable-version3 --enable-libx264',
      ffmpegLicense: 'ffmpeg is licensed under the GNU General Public License version 3 or later.',
    });
    assert.equal(inventory.packages.length, 3);
    assert.match(fs.readFileSync(path.join(area.root, 'THIRD_PARTY_NOTICES.txt'), 'utf8'),
      /Release checks:[\s\S]*corresponding source/i);
    assert.equal(check(area.root), true);
    fs.writeFileSync(path.join(area.root, 'unrecorded-runtime.dat'), 'third-party payload');
    assert.equal(check(area.root), false);
  } finally {
    area.dispose();
    database.dispose();
    mingw.dispose();
  }
});
