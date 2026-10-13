import * as vscode from "vscode";
import * as os from "os";
import * as path from "path";
import * as fs from "fs";
import { ForkMeshIssue } from "./issues";

export type Provider = "claude" | "codex";

const LABELS: Record<Provider, string> = {
  claude: "Claude Code",
  codex: "Codex",
};

export function providerLabel(p: Provider): string {
  return LABELS[p];
}

export function buildPrompt(issue: ForkMeshIssue): string {
  const lines: string[] = [];
  lines.push(`You are working in the ForkMesh repository at ${issue.repoRoot}.`);
  lines.push("");
  lines.push(`Resolve issue #${issue.number}: ${issue.title}`);
  if (issue.labels.length > 0) {
    lines.push(`Labels: ${issue.labels.join(", ")}`);
  }
  lines.push("");
  lines.push("Issue description:");
  lines.push("---");
  lines.push(issue.body || "(no description provided)");
  lines.push("---");
  lines.push("");
  lines.push(
    "Implement the change end to end, keeping it consistent with the surrounding code. " +
      "When you are done, summarize what you changed and how to verify it."
  );
  return lines.join("\n");
}

function writePromptFile(issue: ForkMeshIssue, provider: Provider): string {
  const dir = path.join(os.tmpdir(), "forkmesh-ide");
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, `issue-${issue.number}-${provider}.md`);
  fs.writeFileSync(file, buildPrompt(issue));
  return file;
}

function commandTemplate(provider: Provider): string {
  const cfg = vscode.workspace.getConfiguration("forkmesh");
  const key = provider === "claude" ? "claudeCommand" : "codexCommand";
  const fallback =
    provider === "claude"
      ? 'claude "$(cat {promptFile})"'
      : 'codex "$(cat {promptFile})"';
  return (cfg.get<string>(key) || fallback).trim();
}

const CLAUDE_EXTENSION_ID = "anthropic.claude-code";
const CLAUDE_OPEN_COMMAND = "claude-vscode.editor.open";

async function claudeChatAvailable(): Promise<boolean> {
  if (!vscode.extensions.getExtension(CLAUDE_EXTENSION_ID)) {
    return false;
  }
  const commands = await vscode.commands.getCommands(true);
  return commands.includes(CLAUDE_OPEN_COMMAND);
}

async function startClaudeChat(issue: ForkMeshIssue): Promise<boolean> {
  if (!(await claudeChatAvailable())) {
    return false;
  }
  await vscode.commands.executeCommand(
    CLAUDE_OPEN_COMMAND,
    undefined,
    buildPrompt(issue)
  );
  vscode.window.setStatusBarMessage(
    `ForkMesh: opened ${LABELS.claude} chat for issue #${issue.number}`,
    4000
  );
  return true;
}

const terminals = new Map<string, vscode.Terminal>();

export async function startAgent(
  provider: Provider,
  issue: ForkMeshIssue
): Promise<void> {
  if (provider === "claude" && (await startClaudeChat(issue))) {
    return;
  }

  const promptFile = writePromptFile(issue, provider);
  const command = commandTemplate(provider)
    .replace(/\{promptFile\}/g, promptFile)
    .replace(/\{issueNumber\}/g, String(issue.number));

  const key = `${issue.number}:${provider}`;
  let term = terminals.get(key);
  if (!term || term.exitStatus !== undefined) {
    term = vscode.window.createTerminal({
      name: `ForkMesh #${issue.number} · ${LABELS[provider]}`,
      cwd: issue.repoRoot,
      iconPath: new vscode.ThemeIcon(provider === "claude" ? "sparkle" : "rocket"),
    });
    terminals.set(key, term);
  }
  term.show(true);
  term.sendText(command, true);
  vscode.window.setStatusBarMessage(
    `ForkMesh: started ${LABELS[provider]} on issue #${issue.number}`,
    4000
  );
}
