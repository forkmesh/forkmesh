import * as vscode from "vscode";
import { ForkMeshIssue, findIssuesDir, loadIssues } from "./issues";

export class IssueItem extends vscode.TreeItem {
  constructor(public readonly issue: ForkMeshIssue) {
    super(`#${issue.number}  ${issue.title}`, vscode.TreeItemCollapsibleState.None);
    this.contextValue = "forkmeshIssue";
    this.tooltip = new vscode.MarkdownString(
      `**#${issue.number} ${issue.title}**\n\n` +
        (issue.labels.length ? `Labels: ${issue.labels.join(", ")}\n\n` : "") +
        (issue.body ? issue.body.slice(0, 600) : "_no description_")
    );
    const bits: string[] = [];
    if (issue.priority) {
      bits.push(`P${issue.priority}`);
    }
    if (issue.labels.length) {
      bits.push(issue.labels.join(", "));
    }
    this.description = bits.join(" · ");
    this.iconPath = new vscode.ThemeIcon(
      issue.status === "closed" ? "issue-closed" : "issues"
    );
    this.command = {
      command: "forkmesh.openIssue",
      title: "Open Issue",
      arguments: [this],
    };
  }
}

export class VersionItem extends vscode.TreeItem {
  constructor(version: string) {
    super(`ForkMesh v${version}`, vscode.TreeItemCollapsibleState.None);
    this.contextValue = "forkmeshVersion";
    this.iconPath = new vscode.ThemeIcon("versions");
    this.tooltip = "Update & reload the ForkMesh extension";
    this.command = {
      command: "forkmesh.reload",
      title: "Update & Reload ForkMesh",
    };
  }
}

export class MemberItem extends vscode.TreeItem {
  constructor() {
    super("Become a ForkMesh member", vscode.TreeItemCollapsibleState.None);
    this.contextValue = "forkmeshMember";
    this.iconPath = new vscode.ThemeIcon("heart");
    this.tooltip = "Open forkmesh.com membership in your browser";
    this.command = {
      command: "forkmesh.becomeMember",
      title: "Become a Member",
    };
  }
}

export class IssuesProvider implements vscode.TreeDataProvider<vscode.TreeItem> {
  private readonly _onDidChange = new vscode.EventEmitter<void>();
  readonly onDidChangeTreeData = this._onDidChange.event;

  constructor(private readonly version: string) {}

  refresh(): void {
    this._onDidChange.fire();
  }

  getTreeItem(element: vscode.TreeItem): vscode.TreeItem {
    return element;
  }

  getChildren(): vscode.TreeItem[] {
    const items: vscode.TreeItem[] = [new VersionItem(this.version), new MemberItem()];
    const cfg = vscode.workspace.getConfiguration("forkmesh");
    const roots = (vscode.workspace.workspaceFolders ?? []).map(
      (f) => f.uri.fsPath
    );
    const dir = findIssuesDir(roots, cfg.get<string>("issuesPath") || "");
    if (!dir) {
      return items;
    }
    const issues = loadIssues(dir, cfg.get<boolean>("showClosedIssues") || false);
    items.push(...issues.map((i) => new IssueItem(i)));
    return items;
  }
}
