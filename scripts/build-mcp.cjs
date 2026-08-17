#!/usr/bin/env node
'use strict';

// CMake-free native MCP build for Codex/WSL/Linux/macOS and Windows source
// checkouts. It compiles the exact ve_core sources used by CMake, then publishes
// the executable atomically to build/agent.

const childProcess = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const windows = process.platform === 'win32';
const output = path.join(root, 'build', 'agent', windows ? 'motus-mcp.exe' : 'motus-mcp');
const temporary = `${output}.next`;
const compiler = process.env.CXX || 'g++';

function main() {
  const sources = fs.readdirSync(path.join(root, 'src'))
    .filter((entry) => entry.endsWith('.cpp'))
    .sort()
    .map((entry) => path.join(root, 'src', entry));
  sources.push(path.join(root, 'app', 'mcp_main.cpp'));
  fs.mkdirSync(path.dirname(output), { recursive: true });
  fs.rmSync(temporary, { force: true });
  const compilerArguments = [
    '-std=c++20', '-O2', '-pthread', '-Wall', '-Wextra', '-Wpedantic', '-Wconversion',
    '-Wshadow', '-Werror', `-I${path.join(root, 'include')}`,
    ...sources, '-o', temporary,
  ];
  const built = childProcess.spawnSync(compiler, compilerArguments, {
    cwd: root,
    encoding: 'utf8',
    stdio: ['ignore', 'inherit', 'inherit'],
    windowsHide: true,
  });
  if (built.error) throw built.error;
  if (built.status !== 0) return built.status ?? 1;
  if (!windows) fs.chmodSync(temporary, 0o755);
  fs.rmSync(output, { force: true });
  fs.renameSync(temporary, output);
  console.log(`Motus MCP ready: ${output}`);
  return 0;
}

try {
  process.exitCode = main();
} catch (error) {
  fs.rmSync(temporary, { force: true });
  console.error(`Motus MCP build failed: ${error.message}`);
  process.exitCode = 1;
}
