"use strict";

// forkmesh-conformance-adapter-v1 adapter for the WebCrypto collaboration
// library (app/public/collaboration.js). One JSON request per stdin line, one
// JSON response per stdout line; see www/docs/protocol/certification.md.

if (!globalThis.crypto) {
  globalThis.crypto = require("node:crypto").webcrypto;
}

const path = require("node:path");
const readline = require("node:readline");
const collaboration = require(path.join(__dirname, "..", "..", "..", "..", "app", "public", "collaboration.js"));

const PROTOCOL = "forkmesh-conformance-adapter-v1";
const decoder = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });

function text(hex) {
  return decoder.decode(Buffer.from(hex, "hex"));
}

async function fold(ndjson, policy) {
  const log = await collaboration.SignedEventLog.fromNdjson(ndjson);
  return log.fold(policy);
}

class Rejection extends Error {
  constructor(code) {
    super(code);
    this.code = code;
  }
}

const operations = {
  "canonical.parse": async (input) => {
    let raw;
    try {
      raw = text(input.rawHex);
    } catch (_error) {
      throw new Rejection("invalid_json");
    }
    await collaboration.parseCanonicalEvent(raw);
    return null;
  },
  "canonical.encode": (input) => collaboration.canonicalJson(input.value),
  "identifier.repository": (input) => collaboration.repositoryId(input.verifiedRoot),
  "identifier.policy": (input) => collaboration.policyId(input.policy),
  "identifier.object": (input) => collaboration.objectId(
    input.repository, input.family, input.creator, input.objectKey, input.references,
  ),
  // The library verifies envelopes while folding, so a case is valid when the
  // fold accepts exactly the case's event on top of its context ancestors.
  "event.verify": async (input) => {
    const lines = input.context.map((hex) => `${text(hex)}\n`);
    const before = new Set((await fold(lines.join(""), input.policy)).audit.accepted);
    const result = await fold(lines.join("") + `${text(input.rawHex)}\n`, input.policy);
    const accepted = result.audit.accepted.filter((id) => !before.has(id));
    if (accepted.length === 1) {
      return accepted[0];
    }
    const reasons = result.audit.rejected.map((item) => item.reason);
    throw new Rejection(reasons.length === 1 ? reasons[0] : `accepted=${accepted.length}`);
  },
  "log.fold": async (input) => {
    const result = await fold(input.ndjson, input.policy);
    return {
      stateDigest: result.stateDigest,
      telemetry: result.telemetry,
      rejectionReasons: result.telemetry.rejectionReasons,
    };
  },
};

function write(message) {
  process.stdout.write(`${JSON.stringify(message)}\n`);
}

async function main() {
  const lines = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
  for await (const line of lines) {
    if (!line.trim()) continue;
    const request = JSON.parse(line);
    if (request.type === "hello") {
      write({
        type: "hello",
        protocol: PROTOCOL,
        implementation: { name: "forkmesh-webcrypto-javascript", version: "1.0.0" },
        suiteVersions: ["1.0.0-rc.1"],
        operations: Object.keys(operations).sort(),
      });
      continue;
    }
    const handler = operations[request.operation];
    if (!handler) {
      write({ id: request.id, unsupported: true });
      continue;
    }
    try {
      write({ id: request.id, valid: true, value: (await handler(request.input)) ?? null });
    } catch (error) {
      write({ id: request.id, valid: false, error: (error && error.code) || String(error) });
    }
  }
}

main().catch((error) => {
  process.stderr.write(`${error.stack || error}\n`);
  process.exitCode = 1;
});
