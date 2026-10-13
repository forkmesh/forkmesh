import * as fs from "fs";
import * as path from "path";

export interface ForkMeshIssue {
  number: number;
  title: string;
  status: string;
  labels: string[];
  priority: number;
  assignees: string[];
  body: string;
  issueFile: string;
  repoRoot: string;
}

interface Frontmatter {
  fields: Record<string, string>;
  body: string;
}

function parseFrontmatter(text: string): Frontmatter {
  const fields: Record<string, string> = {};
  if (!text.startsWith("---")) {
    return { fields, body: text };
  }
  const end = text.indexOf("\n---", 3);
  if (end < 0) {
    return { fields, body: text };
  }
  const header = text.slice(3, end).trim();
  for (const line of header.split("\n")) {
    const i = line.indexOf(":");
    if (i < 0) {
      continue;
    }
    const key = line.slice(0, i).trim();
    fields[key] = line.slice(i + 1).trim();
  }
  const afterDashes = text.indexOf("\n", end + 1);
  const body = afterDashes < 0 ? "" : text.slice(afterDashes + 1);
  return { fields, body: body.trim() };
}

function parseList(value: string | undefined): string[] {
  if (!value) {
    return [];
  }
  return value
    .replace(/^\[/, "")
    .replace(/\]$/, "")
    .split(",")
    .map((s) => s.trim().replace(/^["']|["']$/g, ""))
    .filter((s) => s.length > 0);
}

function currentStatus(issueDir: string, fallback: string): string {
  let bestTs = -1;
  let status = fallback;
  let entries: string[] = [];
  try {
    entries = fs.readdirSync(issueDir);
  } catch {
    return fallback;
  }
  for (const name of entries) {
    if (!/^\d+-status\.md$/.test(name)) {
      continue;
    }
    try {
      const fm = parseFrontmatter(
        fs.readFileSync(path.join(issueDir, name), "utf8")
      );
      const ts = parseInt(fm.fields["ts"] ?? "0", 10) || 0;
      if (ts >= bestTs && fm.fields["status"]) {
        bestTs = ts;
        status = fm.fields["status"];
      }
    } catch {
      /* skip unreadable events */
    }
  }
  return status;
}

function readIssue(issuesDir: string, repoRoot: string, dirName: string): ForkMeshIssue | undefined {
  const issueDir = path.join(issuesDir, dirName);
  const issueFile = path.join(issueDir, "issue.md");
  let text: string;
  try {
    text = fs.readFileSync(issueFile, "utf8");
  } catch {
    return undefined;
  }
  const { fields, body } = parseFrontmatter(text);
  const number = parseInt(fields["number"] ?? dirName, 10);
  if (!Number.isFinite(number)) {
    return undefined;
  }
  return {
    number,
    title: fields["title"] || `Issue ${number}`,
    status: currentStatus(issueDir, fields["status"] || "open"),
    labels: parseList(fields["labels"]),
    priority: parseInt(fields["priority"] ?? "0", 10) || 0,
    assignees: parseList(fields["assignees"]),
    body,
    issueFile,
    repoRoot,
  };
}

export function findIssuesDir(roots: string[], override: string): string | undefined {
  if (override && override.trim()) {
    const dir = override.trim();
    return fs.existsSync(dir) ? dir : undefined;
  }
  for (const root of roots) {
    const dir = path.join(root, "issues");
    try {
      if (fs.statSync(dir).isDirectory()) {
        return dir;
      }
    } catch {
      /* keep looking */
    }
  }
  return undefined;
}

export function loadIssues(issuesDir: string, includeClosed: boolean): ForkMeshIssue[] {
  const repoRoot = path.dirname(issuesDir);
  let entries: string[] = [];
  try {
    entries = fs.readdirSync(issuesDir);
  } catch {
    return [];
  }
  const issues: ForkMeshIssue[] = [];
  for (const name of entries) {
    if (!/^\d+$/.test(name)) {
      continue;
    }
    const issue = readIssue(issuesDir, repoRoot, name);
    if (issue && (includeClosed || issue.status === "open")) {
      issues.push(issue);
    }
  }
  issues.sort((a, b) => {
    const pa = a.priority || 999;
    const pb = b.priority || 999;
    if (pa !== pb) {
      return pa - pb;
    }
    return a.number - b.number;
  });
  return issues;
}

export function loadIssueByNumber(repoRoot: string, n: number): ForkMeshIssue | undefined {
  return readIssue(path.join(repoRoot, "issues"), repoRoot, String(n));
}
