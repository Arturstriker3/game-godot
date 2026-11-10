#!/usr/bin/env node
/**
 * Ponte MCP local entre o Claude Desktop e o OpenCode.
 *
 * O Claude Desktop executa servidores MCP na própria máquina do usuário, não
 * na nuvem. Este servidor expõe ferramentas que rodam o `opencode-cli` dentro
 * deste repositório, usando os agentes e as chaves já configuradas no OpenCode.
 *
 * Protocolo: MCP sobre stdio (JSON-RPC 2.0, uma mensagem JSON por linha).
 * stdout é reservado ao protocolo; todo log vai para stderr.
 */

import { spawn } from "node:child_process";
import { readdirSync, readFileSync, existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PROJECT_DIR = process.env.OPENGS_PROJECT_DIR || path.resolve(HERE, "..", "..");

/** Descobre o opencode-cli mais recente instalado pelo app desktop. */
function findOpencodeCli() {
  const base = path.join(process.env.APPDATA || "", "ai.opencode.desktop", "cli");
  if (!existsSync(base)) return null;
  const dirs = readdirSync(base)
    .filter((d) => /^\d+\.\d+\.\d+/.test(d))
    .sort((a, b) => {
      const pa = a.split("-")[0].split(".").map(Number);
      const pb = b.split("-")[0].split(".").map(Number);
      for (let i = 0; i < 3; i++) if (pa[i] !== pb[i]) return pb[i] - pa[i];
      return 0;
    });
  for (const dir of dirs) {
    const candidate = path.join(base, dir, "opencode-cli.exe");
    if (existsSync(candidate)) return candidate;
  }
  return null;
}

const OPENCODE_CLI = process.env.OPENCODE_CLI || findOpencodeCli() || "opencode-cli";

const DEFAULT_MODEL = process.env.OPENGS_CLI_MODEL || "deepseek/deepseek-flash";
const DEFAULT_AGENT = process.env.OPENGS_CLI_AGENT || "orchestrator";
// A ponte do Claude Desktop derruba chamadas acima de ~60s; fique abaixo disso.
const MAX_WAIT_SECONDS = 50;
const DEFAULT_TIMEOUT_SECONDS = 900;
const MAX_TEXT_CHARS = 8000;

const VERSION = "1.1.0";

/* ------------------------------------------------------------------ */
/* Tarefas em segundo plano                                            */
/* ------------------------------------------------------------------ */

/** @type {Map<string, any>} */
const tasks = new Map();
let taskSeq = 0;

function log(...args) {
  process.stderr.write(`[claude-bridge] ${args.join(" ")}\n`);
}

function summarizeTools(tools) {
  if (!tools.length) return "nenhuma";
  const counts = new Map();
  for (const t of tools) counts.set(t, (counts.get(t) || 0) + 1);
  return [...counts.entries()].map(([name, n]) => (n > 1 ? `${name}(${n})` : name)).join(", ");
}

function trimText(text) {
  if (text.length <= MAX_TEXT_CHARS) return text;
  const head = text.slice(0, MAX_TEXT_CHARS - 1200);
  const tail = text.slice(-1000);
  return `${head}\n\n[... ${text.length - MAX_TEXT_CHARS} caracteres omitidos ...]\n\n${tail}`;
}

function startTask({ task, agent, model, timeoutSeconds }) {
  const id = `t${++taskSeq}`;
  const args = ["run", "--archived", "--auto", "--format", "json", "-m", model];
  if (agent) args.push("--agent", agent);
  args.push(task);

  const record = {
    id,
    task,
    agent,
    model,
    status: "running",
    startedAt: Date.now(),
    endedAt: null,
    cost: 0,
    tokens: { input: 0, output: 0, reasoning: 0 },
    tools: [],
    text: "",
    cursor: 0, // até onde o texto já foi entregue (modo delta)
    lastAction: "",
    stderr: "",
    sessionID: null,
    exitCode: null,
    timer: null,
  };

  log(`iniciando ${id} (${model}, agent=${agent || "-"})`);

  let child;
  try {
    child = spawn(OPENCODE_CLI, args, {
      cwd: PROJECT_DIR,
      windowsHide: true,
      env: process.env,
      // stdin fechado: o CLI espera EOF para saber que não há entrada interativa.
      stdio: ["ignore", "pipe", "pipe"],
    });
  } catch (err) {
    record.status = "error";
    record.stderr = String(err);
    record.endedAt = Date.now();
    tasks.set(id, record);
    return record;
  }

  record.child = child;
  let pending = "";

  child.stdout.setEncoding("utf8");
  child.stdout.on("data", (chunk) => {
    pending += chunk;
    let idx;
    while ((idx = pending.indexOf("\n")) >= 0) {
      const line = pending.slice(0, idx).trim();
      pending = pending.slice(idx + 1);
      if (line) handleLine(record, line);
    }
  });

  child.stderr.setEncoding("utf8");
  child.stderr.on("data", (chunk) => {
    record.stderr = (record.stderr + chunk).slice(-4000);
  });

  child.on("error", (err) => {
    record.status = "error";
    record.stderr = (record.stderr + "\n" + String(err)).slice(-4000);
  });

  child.on("close", (code) => {
    if (pending.trim()) handleLine(record, pending.trim());
    record.exitCode = code;
    record.endedAt = Date.now();
    if (record.status === "running") {
      record.status = record.text.trim() ? "done" : code === 0 ? "empty" : "error";
    }
    if (record.timer) clearTimeout(record.timer);
    log(`finalizado ${id}: ${record.status} (${((record.endedAt - record.startedAt) / 1000).toFixed(1)}s)`);
  });

  const timeoutMs = Math.max(30, timeoutSeconds) * 1000;
  record.timer = setTimeout(() => {
    if (record.status !== "running") return;
    record.status = "timeout";
    record.endedAt = Date.now();
    try {
      child.kill();
    } catch {
      /* ignora */
    }
    log(`timeout ${id}`);
  }, timeoutMs);
  record.timer.unref?.();

  tasks.set(id, record);
  return record;
}

function handleLine(record, line) {
  let msg;
  try {
    msg = JSON.parse(line);
  } catch {
    record.stderr = (record.stderr + "\n" + line).slice(-4000);
    return;
  }

  const part = msg.part ?? {};
  if (msg.sessionID) record.sessionID = msg.sessionID;

  if (part.type === "text" && part.text) {
    record.text += part.text;
    return;
  }
  if (part.type === "tool" && part.tool) {
    record.tools.push(part.tool);
    const st = part.state ?? {};
    const inp = st.input ?? {};
    let detail = st.title || inp.filePath || inp.path || inp.command || inp.pattern || inp.url || "";
    detail = String(detail).replace(/\s+/g, " ").slice(0, 90);
    const failed = st.status === "error" ? " ✗" : "";
    record.lastAction = detail ? `${part.tool}: ${detail}` : part.tool;
    record.text += `\n· ${record.lastAction}${failed}\n`;
    return;
  }
  if (part.type === "step-finish" || msg.type === "step_finish") {
    if (typeof part.cost === "number") record.cost += part.cost;
    const t = part.tokens;
    if (t) {
      record.tokens.input += t.input || 0;
      record.tokens.output += t.output || 0;
      record.tokens.reasoning += t.reasoning || 0;
    }
    return;
  }
  if (msg.type === "error" || msg.error) {
    record.stderr = (record.stderr + "\n" + JSON.stringify(msg.error ?? msg)).slice(-4000);
  }
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function waitForTask(record, seconds) {
  const deadline = Date.now() + Math.min(Math.max(seconds, 0), MAX_WAIT_SECONDS) * 1000;
  while (record.status === "running" && Date.now() < deadline) {
    await sleep(500);
  }
  return record;
}

/** Junta linhas de ferramenta repetidas em sequência ("· read: x" ×3). */
function compactLines(text) {
  const out = [];
  for (const line of text.split("\n")) {
    if (!line.trim()) continue;
    const prev = out[out.length - 1];
    if (prev && prev.line === line) prev.n++;
    else out.push({ line, n: 1 });
  }
  return out.map(({ line, n }) => (n > 1 ? `${line} ×${n}` : line)).join("\n");
}

function formatTask(record, { delta = false } = {}) {
  const elapsed = ((record.endedAt ?? Date.now()) - record.startedAt) / 1000;
  const lines = [
    `tarefa: ${record.id} · status: ${record.status}`,
    `modelo: ${record.model}${record.agent ? ` · agente: ${record.agent}` : ""}`,
    `tempo: ${elapsed.toFixed(1)}s · custo: US$ ${record.cost.toFixed(4)} · tokens: ${record.tokens.input} in / ${record.tokens.output} out`,
    `ferramentas: ${summarizeTools(record.tools)}`,
  ];
  if (record.status === "running") {
    if (record.lastAction) lines.push(`agora: ${record.lastAction}`);
    lines.push(`ainda rodando — consulte com deepseek_result(task_id="${record.id}").`);
  }
  const body = delta ? record.text.slice(record.cursor) : record.text;
  record.cursor = record.text.length;
  if (body.trim()) {
    lines.push("", delta ? "--- novo desde a última consulta ---" : "--- saída ---", trimText(compactLines(body.trim())));
  } else if (delta) {
    lines.push("", "(nada novo desde a última consulta)");
  }
  if (record.status !== "done" && record.stderr.trim()) {
    lines.push("", "--- stderr (fim) ---", record.stderr.trim().split("\n").slice(-12).join("\n"));
  }
  return lines.join("\n");
}

/* ------------------------------------------------------------------ */
/* Ferramentas                                                         */
/* ------------------------------------------------------------------ */

function listAgents() {
  const dir = path.join(PROJECT_DIR, ".opencode", "agents");
  if (!existsSync(dir)) return [];
  return readdirSync(dir)
    .filter((f) => f.endsWith(".md"))
    .map((file) => {
      const raw = readFileSync(path.join(dir, file), "utf8");
      const fm = raw.match(/^---\r?\n([\s\S]*?)\r?\n---/);
      const body = fm ? fm[1] : "";
      const get = (key) => body.match(new RegExp(`^${key}:\\s*(.+)$`, "m"))?.[1]?.trim() ?? null;
      return {
        name: file.replace(/\.md$/, ""),
        model: get("model"),
        mode: get("mode"),
        description: get("description"),
      };
    });
}

const TOOLS = [
  {
    name: "deepseek_task",
    title: "Delegar tarefa ao OpenCode/DeepSeek",
    description:
      "Delega uma tarefa de código neste repositório (OpenGS Game) a um agente do OpenCode rodando localmente com DeepSeek. " +
      "O agente tem acesso real ao disco, ao Git, aos testes headless e aos MCPs (Godot/Blender). " +
      "Retorna um resumo curto: status, custo, ferramentas usadas e a resposta final do agente. " +
      "Se demorar mais que wait_seconds, retorna status 'running' — continue com deepseek_result.",
    inputSchema: {
      type: "object",
      properties: {
        task: {
          type: "string",
          description:
            "Instrução objetiva, com escopo e critério de aceite. Ex.: 'Rode tools/economy_ai_test.gd e diga se passou'.",
        },
        agent: {
          type: "string",
          description:
            "Agente OpenCode: orchestrator (padrão, roteia), godot-engineer, game-systems-designer, ui-ux-designer, world-3d-engineer, qa-docs, senior-architect.",
        },
        model: {
          type: "string",
          description: "Modelo no formato provider/modelo. Padrão: deepseek/deepseek-flash.",
        },
        wait_seconds: {
          type: "number",
          description: `Quanto esperar por resultado antes de devolver 'running' (0-${MAX_WAIT_SECONDS}, padrão 30).`,
        },
        timeout_seconds: {
          type: "number",
          description: `Limite total da tarefa em segundo plano antes de encerrar (padrão ${DEFAULT_TIMEOUT_SECONDS}).`,
        },
      },
      required: ["task"],
      additionalProperties: false,
    },
  },
  {
    name: "deepseek_result",
    title: "Consultar tarefa em andamento",
    description:
      "Consulta uma tarefa iniciada por deepseek_task. Por padrão espera até 45s e devolve só o que é novo " +
      "desde a última consulta (barato para acompanhar em loop até status != running).",
    inputSchema: {
      type: "object",
      properties: {
        task_id: { type: "string", description: "ID retornado por deepseek_task (ex.: t1)." },
        wait_seconds: {
          type: "number",
          description: `Quanto esperar antes de responder (0-${MAX_WAIT_SECONDS}, padrão 45). Retorna antes se a tarefa terminar.`,
        },
        full: {
          type: "boolean",
          description: "true devolve a saída inteira; padrão false devolve só o que é novo desde a última consulta.",
        },
      },
      required: ["task_id"],
      additionalProperties: false,
    },
  },
  {
    name: "opencode_agents",
    title: "Listar agentes e modelos do OpenCode",
    description:
      "Lista os agentes definidos em .opencode/agents com modelo e descrição, sem gastar tokens de modelo. " +
      "Útil para escolher quem recebe a próxima tarefa.",
    inputSchema: { type: "object", properties: {}, additionalProperties: false },
  },
];

async function callTool(name, args) {
  if (name === "opencode_agents") {
    const agents = listAgents();
    const text = agents
      .map((a) => `- ${a.name} (${a.model ?? "modelo padrão"}, ${a.mode ?? "mode?"}): ${a.description ?? ""}`)
      .join("\n");
    return {
      content: [
        {
          type: "text",
          text: `projeto: ${PROJECT_DIR}\nmodelo padrão: ${DEFAULT_MODEL}\nagente padrão: ${DEFAULT_AGENT}\n\n${text}`,
        },
      ],
    };
  }

  if (name === "deepseek_task") {
    const task = String(args?.task ?? "").trim();
    if (!task) throw new Error("campo 'task' é obrigatório");
    const record = startTask({
      task,
      agent: args?.agent ? String(args.agent) : DEFAULT_AGENT,
      model: args?.model ? String(args.model) : DEFAULT_MODEL,
      timeoutSeconds: Number(args?.timeout_seconds) || DEFAULT_TIMEOUT_SECONDS,
    });
    await waitForTask(record, Number(args?.wait_seconds ?? 30));
    // deepseek_result seguinte devolve só o que vier depois deste retorno.
    return { content: [{ type: "text", text: formatTask(record) }] };
  }

  if (name === "deepseek_result") {
    const record = tasks.get(String(args?.task_id ?? ""));
    if (!record) throw new Error(`tarefa não encontrada: ${args?.task_id}`);
    await waitForTask(record, Number(args?.wait_seconds ?? 45));
    return { content: [{ type: "text", text: formatTask(record, { delta: !args?.full }) }] };
  }

  throw new Error(`ferramenta desconhecida: ${name}`);
}

/* ------------------------------------------------------------------ */
/* Protocolo MCP                                                       */
/* ------------------------------------------------------------------ */

function send(payload) {
  process.stdout.write(JSON.stringify(payload) + "\n");
}

function reply(id, result) {
  send({ jsonrpc: "2.0", id, result });
}

function replyError(id, code, message) {
  send({ jsonrpc: "2.0", id, error: { code, message } });
}

async function handle(msg) {
  const { id, method, params } = msg;
  const isRequest = id !== undefined && id !== null;

  switch (method) {
    case "initialize":
      return isRequest
        ? reply(id, {
            protocolVersion: params?.protocolVersion ?? "2024-11-05",
            capabilities: { tools: { listChanged: false } },
            serverInfo: { name: "opengs-claude-bridge", version: VERSION },
            instructions:
              "Ferramentas para delegar trabalho ao OpenCode/DeepSeek rodando localmente em G:\\projetos\\opengs-game. " +
              "Use deepseek_task para qualquer tarefa que precise ler, editar, testar ou gerar algo no projeto; " +
              "você recebe só um resumo e mantém o contexto curto.",
          })
        : undefined;

    case "notifications/initialized":
    case "initialized":
      return undefined;

    case "ping":
      return isRequest ? reply(id, {}) : undefined;

    case "tools/list":
      return isRequest
        ? reply(id, { tools: TOOLS.map(({ name, title, description, inputSchema }) => ({ name, title, description, inputSchema })) })
        : undefined;

    case "tools/call":
      if (!isRequest) return undefined;
      try {
        const result = await callTool(params?.name, params?.arguments ?? {});
        return reply(id, result);
      } catch (err) {
        return reply(id, {
          content: [{ type: "text", text: `erro: ${err?.message ?? err}` }],
          isError: true,
        });
      }

    case "resources/list":
      return isRequest ? reply(id, { resources: [] }) : undefined;

    case "prompts/list":
      return isRequest ? reply(id, { prompts: [] }) : undefined;

    default:
      return isRequest ? replyError(id, -32601, `método não suportado: ${method}`) : undefined;
  }
}

let buffer = "";
process.stdin.setEncoding("utf8");
process.stdin.on("data", (chunk) => {
  buffer += chunk;
  let idx;
  while ((idx = buffer.indexOf("\n")) >= 0) {
    const line = buffer.slice(0, idx).trim();
    buffer = buffer.slice(idx + 1);
    if (!line) continue;
    let msg;
    try {
      msg = JSON.parse(line);
    } catch (err) {
      log("linha inválida:", err.message);
      continue;
    }
    handle(msg).catch((err) => {
      log("falha ao tratar mensagem:", err);
      if (msg.id !== undefined) replyError(msg.id, -32603, String(err?.message ?? err));
    });
  }
});

process.stdin.on("end", () => {
  for (const record of tasks.values()) {
    if (record.status === "running") {
      try {
        record.child?.kill();
      } catch {
        /* ignora */
      }
    }
  }
  process.exit(0);
});

log(`pronto — projeto: ${PROJECT_DIR}`);
log(`cli: ${OPENCODE_CLI}`);
