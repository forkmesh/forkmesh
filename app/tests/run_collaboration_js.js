"use strict";

if (!globalThis.crypto) {
  globalThis.crypto = require("node:crypto").webcrypto;
}

const fs = require("node:fs");
const path = require("node:path");
const collaboration = require(path.join(__dirname, "..", "public", "collaboration.js"));

async function main() {
  const input = JSON.parse(fs.readFileSync(0, "utf8"));
  const result = Object.prototype.hasOwnProperty.call(input, "ndjson")
    ? await (await collaboration.SignedEventLog.fromNdjson(input.ndjson)).fold(input.policy)
    : await collaboration.foldEvents(input.events, input.policy);
  process.stdout.write(JSON.stringify(result));
}

main().catch((error) => {
  process.stderr.write(`${error.stack || error}\n`);
  process.exitCode = 1;
});
