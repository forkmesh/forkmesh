"use strict";

if (!globalThis.crypto) {
  globalThis.crypto = require("node:crypto").webcrypto;
}

const fs = require("node:fs");
const path = require("node:path");
const trustLists = require(path.join(__dirname, "..", "public", "trust-lists.js"));

async function main() {
  const input = JSON.parse(fs.readFileSync(0, "utf8"));
  const result = await trustLists.resolve(input.documents || [], {
    now: input.now,
    follows: input.follows || [],
    overrides: input.overrides || [],
  });
  process.stdout.write(JSON.stringify(result));
}

main().catch((error) => {
  process.stderr.write(`${error.stack || error}\n`);
  process.exitCode = 1;
});
