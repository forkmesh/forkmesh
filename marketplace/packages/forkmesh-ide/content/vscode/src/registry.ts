import * as os from "os";
import * as path from "path";
import * as fs from "fs";

// These paths are the file protocol shared with the desktop client.
export const FORKMESH_DIR = path.join(os.homedir(), ".forkmesh", "ide");
export const REQUESTS_DIR = path.join(FORKMESH_DIR, "requests");
export const RESPONSES_DIR = path.join(FORKMESH_DIR, "responses");
export const REGISTRATION_FILE = path.join(FORKMESH_DIR, "registration.json");

export interface Registration {
  ide: string;
  version: string;
  pid: number;
  ts: number;
  workspaces: string[];
  agents: { claude: boolean; codex: boolean };
}

export interface TaskRequest {
  id: string;
  ts: number;
  repoPath: string;
  issueNumber: number;
  issueTitle?: string;
  provider: "claude" | "codex";
}

export function ensureDirs(): void {
  fs.mkdirSync(REQUESTS_DIR, { recursive: true });
  fs.mkdirSync(RESPONSES_DIR, { recursive: true });
}

export function writeRegistration(reg: Registration): void {
  try {
    ensureDirs();
    fs.writeFileSync(REGISTRATION_FILE, JSON.stringify(reg, null, 2));
  } catch {
    /* best effort: detection is a convenience, never block the IDE on it */
  }
}

export function clearRegistration(): void {
  try {
    fs.rmSync(REGISTRATION_FILE, { force: true });
  } catch {
    /* ignore */
  }
}

export function writeResponse(
  id: string,
  body: { ok: boolean; message: string }
): void {
  try {
    ensureDirs();
    fs.writeFileSync(
      path.join(RESPONSES_DIR, `${id}.json`),
      JSON.stringify({ id, ts: Date.now(), ...body }, null, 2)
    );
  } catch {
    /* ignore */
  }
}

export function takeRequest(file: string): TaskRequest | undefined {
  try {
    const raw = fs.readFileSync(file, "utf8");
    fs.rmSync(file, { force: true });
    const req = JSON.parse(raw) as TaskRequest;
    if (!req || typeof req.issueNumber !== "number" || !req.id) {
      return undefined;
    }
    return req;
  } catch {
    return undefined;
  }
}
