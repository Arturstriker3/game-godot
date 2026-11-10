#!/usr/bin/env node
/**
 * Instala (ou conserta) a entrada MCP da ponte no claude_desktop_config.json.
 *
 * O Claude Desktop no Windows é MSIX e guarda o config em caminhos variados
 * conforme a build. Este script procura os candidatos, faz backup e mescla
 * apenas a chave mcpServers, preservando preferences e o resto.
 *
 *   node tools/claude_bridge/install.mjs           # instala/mescla
 *   node tools/claude_bridge/install.mjs --check   # só mostra o que faria
 */

import { existsSync, readFileSync, writeFileSync, copyFileSync, mkdirSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PROJECT_DIR = path.resolve(HERE, "..", "..");
const SERVER = path.join(PROJECT_DIR, "tools", "claude_bridge", "server.mjs");
const NODE = process.execPath;
const checkOnly = process.argv.includes("--check");

const LOCAL = process.env.LOCALAPPDATA ?? "";
const ROAMING = process.env.APPDATA ?? "";
const PACKAGE = "Claude_pzs8sxrjxfjjc";

const CANDIDATES = [
  process.env.CLAUDE_CONFIG,
  path.join(LOCAL, "Claude-Data", PACKAGE, "claude_desktop_config.json"),
  path.join(LOCAL, "Packages", PACKAGE, "LocalCache", "Roaming", "Claude", "claude_desktop_config.json"),
  path.join(ROAMING, "Claude", "claude_desktop_config.json"),
].filter(Boolean);

function readJson(file) {
  try {
    return JSON.parse(readFileSync(file, "utf8"));
  } catch {
    return null;
  }
}

// Prefere o arquivo que já tem conteúdo real (preferences), como faz o app.
const existing = CANDIDATES.filter((f) => existsSync(f));
const target =
  existing.find((f) => readJson(f)?.preferences) ?? existing[0] ?? CANDIDATES[1];

const config = (target && readJson(target)) ?? {};
config.mcpServers = { ...(config.mcpServers ?? {}) };
config.mcpServers["opengs-deepseek"] = {
  command: NODE,
  args: [SERVER],
  env: { OPENGS_PROJECT_DIR: PROJECT_DIR },
};

const problems = [];
if (!existsSync(NODE)) problems.push(`node não encontrado: ${NODE}`);
if (!existsSync(SERVER)) problems.push(`servidor não encontrado: ${SERVER}`);

console.log(`config:  ${target}`);
console.log(`command: ${NODE}`);
console.log(`args:    ${SERVER}`);
console.log(`projeto: ${PROJECT_DIR}`);
console.log(`servidores: ${Object.keys(config.mcpServers).join(", ")}`);

if (problems.length) {
  console.error("\nPROBLEMAS:\n- " + problems.join("\n- "));
  process.exit(1);
}

if (checkOnly) {
  console.log("\n(--check) nada gravado.");
  process.exit(0);
}

mkdirSync(path.dirname(target), { recursive: true });
if (existsSync(target)) {
  const backup = `${target}.bak-${new Date().toISOString().slice(0, 19).replace(/[:T]/g, "")}`;
  copyFileSync(target, backup);
  console.log(`backup:  ${backup}`);
}
writeFileSync(target, JSON.stringify(config, null, 2) + "\n", "utf8");

// Confere relendo o valor real gravado (evita dupla escapagem de barras).
const written = readJson(target).mcpServers["opengs-deepseek"];
const ok = written.command === NODE && written.args[0] === SERVER && existsSync(written.args[0]);
console.log(ok ? "\nOK — entrada gravada corretamente." : "\nFALHOU — valor gravado divergente.");
process.exit(ok ? 0 : 1);
