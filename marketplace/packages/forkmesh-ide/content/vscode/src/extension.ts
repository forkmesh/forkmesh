import * as vscode from "vscode";
import * as fs from "fs";
import * as path from "path";
import { IssueItem, IssuesProvider } from "./issuesProvider";
import { Provider, providerLabel, startAgent } from "./agents";
import { loadIssueByNumber } from "./issues";
import {
  REQUESTS_DIR,
  Registration,
  TaskRequest,
  clearRegistration,
  ensureDirs,
  takeRequest,
  writeRegistration,
  writeResponse,
} from "./registry";

const HEARTBEAT_MS = 20_000;
const MEMBERSHIP_URL = "https://forkmesh.com/#membership";

function currentRegistration(): Registration {
  return {
    ide: vscode.env.appName,
    version: vscode.version,
    pid: process.pid,
    ts: Date.now(),
    workspaces: (vscode.workspace.workspaceFolders ?? []).map((f) => f.uri.fsPath),
    agents: { claude: true, codex: true },
  };
}

async function handleRequest(req: TaskRequest): Promise<void> {
  const issue = loadIssueByNumber(req.repoPath, req.issueNumber);
  if (!issue) {
    writeResponse(req.id, {
      ok: false,
      message: `Issue #${req.issueNumber} not found under ${req.repoPath}`,
    });
    vscode.window.showWarningMessage(
      `ForkMesh: could not find issue #${req.issueNumber} to run.`
    );
    return;
  }
  await startAgent(req.provider, issue);
  writeResponse(req.id, {
    ok: true,
    message: `Started ${providerLabel(req.provider)} on issue #${issue.number}`,
  });
}

function drainExistingRequests(): void {
  let files: string[] = [];
  try {
    files = fs.readdirSync(REQUESTS_DIR);
  } catch {
    return;
  }
  for (const f of files) {
    if (!f.endsWith(".json")) {
      continue;
    }
    const req = takeRequest(path.join(REQUESTS_DIR, f));
    if (req) {
      void handleRequest(req);
    }
  }
}

export function activate(context: vscode.ExtensionContext): void {
  ensureDirs();

  const version = String(context.extension.packageJSON.version ?? "?");
  const provider = new IssuesProvider(version);
  context.subscriptions.push(
    vscode.window.registerTreeDataProvider("forkmeshIssues", provider)
  );

  const run = (item: IssueItem | undefined, p: Provider) => {
    if (!item) {
      vscode.window.showInformationMessage("ForkMesh: select an issue first.");
      return;
    }
    startAgent(p, item.issue).catch((err) =>
      vscode.window.showErrorMessage(`ForkMesh: failed to start ${p}: ${err}`)
    );
  };

  context.subscriptions.push(
    vscode.commands.registerCommand("forkmesh.refresh", () => provider.refresh()),
    vscode.commands.registerCommand("forkmesh.startClaude", (i: IssueItem) =>
      run(i, "claude")
    ),
    vscode.commands.registerCommand("forkmesh.startCodex", (i: IssueItem) =>
      run(i, "codex")
    ),
    vscode.commands.registerCommand("forkmesh.openIssue", (i: IssueItem) => {
      if (i?.issue?.issueFile) {
        vscode.window.showTextDocument(vscode.Uri.file(i.issue.issueFile));
      }
    }),
    vscode.commands.registerCommand("forkmesh.openSettings", () =>
      vscode.commands.executeCommand("workbench.action.openSettings", "forkmesh")
    ),
    vscode.commands.registerCommand("forkmesh.becomeMember", () =>
      vscode.env.openExternal(vscode.Uri.parse(MEMBERSHIP_URL))
    ),
    vscode.commands.registerCommand("forkmesh.reload", async () => {
      const choice = await vscode.window.showInformationMessage(
        `Reload the window to load the latest ForkMesh extension? (current v${version})`,
        { modal: true },
        "Reload"
      );
      if (choice === "Reload") {
        await vscode.commands.executeCommand("workbench.action.reloadWindow");
      }
    })
  );

  const watcher = vscode.workspace.createFileSystemWatcher("**/issues/**");
  watcher.onDidCreate(() => provider.refresh());
  watcher.onDidChange(() => provider.refresh());
  watcher.onDidDelete(() => provider.refresh());
  context.subscriptions.push(watcher);
  context.subscriptions.push(
    vscode.workspace.onDidChangeWorkspaceFolders(() => provider.refresh()),
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration("forkmesh")) {
        provider.refresh();
      }
    })
  );

  writeRegistration(currentRegistration());
  const heartbeat = setInterval(
    () => writeRegistration(currentRegistration()),
    HEARTBEAT_MS
  );
  context.subscriptions.push({ dispose: () => clearInterval(heartbeat) });
  context.subscriptions.push(
    vscode.window.onDidChangeWindowState(() =>
      writeRegistration(currentRegistration())
    )
  );

  drainExistingRequests();
  try {
    const reqWatcher = fs.watch(REQUESTS_DIR, (_event, filename) => {
      if (!filename || !filename.toString().endsWith(".json")) {
        return;
      }
      const full = path.join(REQUESTS_DIR, filename.toString());
      setTimeout(() => {
        if (!fs.existsSync(full)) {
          return;
        }
        const req = takeRequest(full);
        if (req) {
          void handleRequest(req);
        }
      }, 80);
    });
    context.subscriptions.push({ dispose: () => reqWatcher.close() });
  } catch {
    /* fs.watch may be unsupported on some platforms; requests still drain on
       startup and on each refresh below. */
  }

  context.subscriptions.push({ dispose: () => clearRegistration() });
}

export function deactivate(): void {
  clearRegistration();
}
