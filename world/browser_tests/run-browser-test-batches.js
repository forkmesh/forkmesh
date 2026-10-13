const { spawn, spawnSync } = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const cwd = __dirname;
const playwright = path.join(cwd, "node_modules", ".bin", "playwright");
const forwarded = process.argv.slice(2);

function run(command, args, options = {}) {
  const result = spawnSync(command, args, { cwd, ...options });
  if (result.error) {
    console.error(result.error.message);
    process.exit(1);
  }
  return result;
}

function runStreaming(command, args, options = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, {
      cwd,
      ...options,
      stdio: ["inherit", "pipe", "pipe"],
    });
    let output = "";
    const forward = (stream, destination) => {
      stream.on("data", (chunk) => {
        destination.write(chunk);
        output += chunk.toString();
        if (output.length > 4_000_000) output = output.slice(-4_000_000);
      });
    };
    forward(child.stdout, process.stdout);
    forward(child.stderr, process.stderr);
    child.once("error", reject);
    child.once("close", (status, signal) => {
      resolve({ status: status ?? 1, signal, output });
    });
  });
}

function hasTransportLoss(output, startedAt) {
  if (/Network connection lost|net::ERR_CONNECTION_REFUSED/.test(output)) {
    return true;
  }
  const logDirectory = path.join(
    process.env.XDG_CONFIG_HOME || path.join(os.homedir(), ".config"),
    ".wrangler",
    "logs",
  );
  try {
    return fs.readdirSync(logDirectory).some((name) => {
      if (!name.startsWith("wrangler-") || !name.endsWith(".log")) return false;
      const file = path.join(logDirectory, name);
      if (fs.statSync(file).mtimeMs < startedAt - 5_000) return false;
      const contents = fs.readFileSync(file, "utf8");
      return contents.includes(cwd) && contents.includes("Network connection lost");
    });
  } catch {
    return false;
  }
}

const env = {
  ...process.env,
  FORKMESH_BROWSER_FULL: "1",
};

if (env.FORKMESH_BROWSER_PRESTAGED !== "1") {
  for (const args of [
    ["../../app/tools/build_dashboard_assets.py"],
    ["../../app/tools/build_site_assets.py", "cutover"],
  ]) {
    const staged = run("python3", args, { stdio: "inherit" });
    if (staged.status !== 0) process.exit(staged.status || 1);
  }
}
env.FORKMESH_BROWSER_PRESTAGED = "1";

const listed = run(playwright, ["test", "--list"], {
  env,
  encoding: "utf8",
});
if (listed.status !== 0) {
  process.stdout.write(listed.stdout || "");
  process.stderr.write(listed.stderr || "");
  process.exit(listed.status || 1);
}

const declared = listed.stdout.match(/Total:\s+(\d+)\s+tests?\s+in\s+\d+\s+files?/);
const entries = [...listed.stdout.matchAll(/^\s+(.+\.spec\.js):(\d+):\d+\s+›/gm)];
const total = Number(declared?.[1] || 0);
if (!total || entries.length !== total) {
  console.error(
    `Could not safely batch the browser collection: parsed ${entries.length} of ${total || "unknown"} tests.`,
  );
  process.exit(1);
}

const locations = [];
const locationByName = new Map();
for (const entry of entries) {
  const name = `${entry[1]}:${entry[2]}`;
  let location = locationByName.get(name);
  if (!location) {
    location = { name, count: 0 };
    locationByName.set(name, location);
    locations.push(location);
  }
  location.count += 1;
}

const batchLimit = Math.ceil(total / 10);
const batches = [];
let remainingTests = total;
let remainingBatches = batchLimit;
let current = { locations: [], count: 0 };
for (const location of locations) {
  const target = Math.ceil(remainingTests / remainingBatches);
  if (
    current.count > 0 &&
    current.count + location.count > target &&
    batches.length < batchLimit - 1
  ) {
    batches.push(current);
    remainingTests -= current.count;
    remainingBatches -= 1;
    current = { locations: [], count: 0 };
  }
  current.locations.push(location.name);
  current.count += location.count;
}
batches.push(current);

async function main() {
  let passed = 0;
  for (const [index, batch] of batches.entries()) {
    let complete = false;
    for (let attempt = 1; attempt <= 3; attempt += 1) {
      console.log(
        `\nBrowser batch ${index + 1}/${batches.length}: ${batch.count} tests with a fresh Worker lifecycle${attempt > 1 ? ` (transport retry ${attempt - 1}/2)` : ""}`,
      );
      const startedAt = Date.now();
      const result = await runStreaming(
        playwright,
        ["test", ...batch.locations, ...forwarded],
        { env },
      );
      if (result.status === 0) {
        passed += batch.count;
        complete = true;
        console.log(`Browser aggregate: ${passed}/${total} passed.`);
        break;
      }
      if (!hasTransportLoss(result.output, startedAt)) {
        console.error(
          `Browser batch ${index + 1} failed without a Wrangler transport-loss signature; not retrying.`,
        );
        process.exit(1);
      }
      if (attempt === 3) {
        console.error(
          `Browser batch ${index + 1} exhausted its Wrangler transport retries.`,
        );
        process.exit(1);
      }
      console.error(
        `Wrangler transport was lost in browser batch ${index + 1}; retrying the same tests with a fresh Worker.`,
      );
    }
    if (!complete) process.exit(1);
  }
  const verb = forwarded.includes("--list") ? "collected" : "passed";
  console.log(
    `\nAll ${total} browser tests ${verb} across ${batches.length} fresh Worker lifecycles.`,
  );
}

main().catch((error) => {
  console.error(error?.stack || error);
  process.exit(1);
});
