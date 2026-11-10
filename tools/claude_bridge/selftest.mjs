#!/usr/bin/env node
/**
 * Teste de fumaça da ponte MCP.
 *
 *   node tools/claude_bridge/selftest.mjs            # só handshake/listagem (grátis)
 *   node tools/claude_bridge/selftest.mjs --task      # inclui uma tarefa real no DeepSeek (~US$ 0,002)
 */

import { spawn } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const runTask = process.argv.includes("--task");

const child = spawn(process.execPath, [path.join(HERE, "server.mjs")], {
  stdio: ["pipe", "pipe", "inherit"],
});

let buffer = "";
const pending = new Map();

child.stdout.setEncoding("utf8");
child.stdout.on("data", (chunk) => {
  buffer += chunk;
  let idx;
  while ((idx = buffer.indexOf("\n")) >= 0) {
    const line = buffer.slice(0, idx).trim();
    buffer = buffer.slice(idx + 1);
    if (!line) continue;
    let msg;
    try {
      msg = JSON.parse(line);
    } catch {
      console.error("stdout não-JSON:", line);
      continue;
    }
    const resolve = pending.get(msg.id);
    if (resolve) {
      pending.delete(msg.id);
      resolve(msg);
    }
  }
});

let seq = 0;
function request(method, params) {
  const id = ++seq;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`timeout em ${method}`)), 150000);
    pending.set(id, (msg) => {
      clearTimeout(timer);
      resolve(msg);
    });
    child.stdin.write(JSON.stringify({ jsonrpc: "2.0", id, method, params }) + "\n");
  });
}

const textOf = (msg) => msg.result?.content?.map((c) => c.text).join("\n") ?? JSON.stringify(msg.result);

try {
  const init = await request("initialize", {
    protocolVersion: "2024-11-05",
    capabilities: {},
    clientInfo: { name: "selftest", version: "1.0.0" },
  });
  console.log("initialize:", init.result?.serverInfo);

  child.stdin.write(JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" }) + "\n");

  const list = await request("tools/list", {});
  console.log("tools:", list.result?.tools.map((t) => t.name).join(", "));

  const agents = await request("tools/call", { name: "opencode_agents", arguments: {} });
  console.log("\nagentes:\n" + textOf(agents));

  if (runTask) {
    const task = await request("tools/call", {
      name: "deepseek_task",
      arguments: {
        task: "Sem editar nada: diga em uma linha quantas cenas .tscn existem em game/ (conte pelo shell).",
        agent: "qa-docs",
        wait_seconds: 60,
      },
    });
    console.log("\ntarefa:\n" + textOf(task));
  }

  console.log("\nOK");
} catch (err) {
  console.error("FALHOU:", err.message);
  process.exitCode = 1;
} finally {
  child.stdin.end();
  child.kill();
}
