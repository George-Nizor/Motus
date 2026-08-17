#!/usr/bin/env node
'use strict';

// Rasterise Motus' canonical SVG mark at Windows shell sizes and pack the PNGs
// into one Vista-compatible ICO. Keeping generation deterministic makes the
// checked-in executable resource auditable instead of depending on an IDE.

const childProcess = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const SIZES = [16, 24, 32, 48, 64, 128, 256];
const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

function usage() {
  console.error('Usage: build-windows-icon.cjs <mark.svg> <motus.ico> [rsvg-convert]');
}

function assertPng(buffer, expectedSize, source) {
  if (buffer.length < 24 || !buffer.subarray(0, 8).equals(PNG_SIGNATURE) ||
      buffer.toString('ascii', 12, 16) !== 'IHDR') {
    throw new Error(`${source} is not a valid PNG`);
  }
  const width = buffer.readUInt32BE(16);
  const height = buffer.readUInt32BE(20);
  if (width !== expectedSize || height !== expectedSize) {
    throw new Error(`${source} is ${width}x${height}; expected ${expectedSize}x${expectedSize}`);
  }
}

function packIco(images) {
  const directorySize = 6 + images.length * 16;
  const header = Buffer.alloc(directorySize);
  header.writeUInt16LE(0, 0);
  header.writeUInt16LE(1, 2);
  header.writeUInt16LE(images.length, 4);
  let offset = directorySize;
  images.forEach(({ size, png }, index) => {
    const entry = 6 + index * 16;
    header.writeUInt8(size === 256 ? 0 : size, entry);
    header.writeUInt8(size === 256 ? 0 : size, entry + 1);
    header.writeUInt8(0, entry + 2);
    header.writeUInt8(0, entry + 3);
    header.writeUInt16LE(1, entry + 4);
    header.writeUInt16LE(32, entry + 6);
    header.writeUInt32LE(png.length, entry + 8);
    header.writeUInt32LE(offset, entry + 12);
    offset += png.length;
  });
  return Buffer.concat([header, ...images.map(({ png }) => png)]);
}

function buildIcon(svgPath, icoPath, renderer) {
  if (!fs.existsSync(svgPath)) throw new Error(`SVG mark not found: ${svgPath}`);
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'motus-icon-'));
  try {
    const images = SIZES.map((size) => {
      const output = path.join(temporary, `motus-${size}.png`);
      const result = childProcess.spawnSync(renderer, [
        '--width', String(size), '--height', String(size), '--output', output, svgPath,
      ], { encoding: 'utf8', windowsHide: true });
      if (result.error) throw result.error;
      if (result.status !== 0) {
        throw new Error(`rsvg-convert failed for ${size}px: ${(result.stderr || '').trim()}`);
      }
      const png = fs.readFileSync(output);
      assertPng(png, size, output);
      return { size, png };
    });
    fs.mkdirSync(path.dirname(icoPath), { recursive: true });
    fs.writeFileSync(icoPath, packIco(images));
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}

function main(argv) {
  if (argv.length < 2 || argv.length > 3) {
    usage();
    return 2;
  }
  const svgPath = path.resolve(argv[0]);
  const icoPath = path.resolve(argv[1]);
  const renderer = argv[2] ? path.resolve(argv[2]) : 'rsvg-convert';
  buildIcon(svgPath, icoPath, renderer);
  console.log(`Generated ${icoPath} (${SIZES.join(', ')} px) from ${svgPath}`);
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

module.exports = { SIZES, assertPng, packIco };
