"use strict";

// Runs the WebCrypto collaboration implementation against the protocol suite
// vectors it claims (conformance/protocol/v1/manifest.json) and prints one
// result per case. Validity and values are normative; error codes are
// reported so the Python suite can compare them.

if (!globalThis.crypto) {
  globalThis.crypto = require("node:crypto").webcrypto;
}

const fs = require("node:fs");
const path = require("node:path");
const collaboration = require(path.join(__dirname, "..", "public", "collaboration.js"));

const vectorDir = path.join(__dirname, "..", "..", "conformance", "protocol", "v1", "vectors");
const load = (name) => JSON.parse(fs.readFileSync(path.join(vectorDir, `${name}.json`), "utf8"));
const decoder = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });
const results = [];

async function outcome(call) {
  try {
    return { valid: true, value: await call() };
  } catch (error) {
    return { valid: false, error: (error && error.code) || String(error) };
  }
}

function record(set, testCase, actual, matches = () => true) {
  const expect = testCase.expect;
  let ok = actual.valid === expect.valid;
  if (ok && actual.valid) ok = matches(actual.value);
  results.push({
    set,
    id: testCase.id,
    ok,
    expectedError: expect.valid ? null : expect.error,
    actualError: actual.valid ? null : actual.error,
    value: actual.valid ? actual.value : null,
  });
}

async function fold(ndjson, policy) {
  const log = await collaboration.SignedEventLog.fromNdjson(ndjson);
  return log.fold(policy);
}

async function main() {
  const canonical = load("canonical-json");
  for (const testCase of canonical.parse) {
    let text;
    try {
      text = decoder.decode(Buffer.from(testCase.rawHex, "hex"));
    } catch (_error) {
      record("canonical-json", testCase, { valid: false, error: "invalid_json" });
      continue;
    }
    record("canonical-json", testCase, await outcome(() => collaboration.parseCanonicalEvent(text)));
  }
  for (const testCase of canonical.encode) {
    record(
      "canonical-json",
      testCase,
      await outcome(() => collaboration.canonicalJson(testCase.value)),
      (value) => value === testCase.expect.canonicalText,
    );
  }

  const identifiers = load("identifiers");
  for (const testCase of identifiers.repositories) {
    record(
      "identifiers",
      testCase,
      await outcome(() => collaboration.repositoryId(testCase.verifiedRoot)),
      (value) => value === testCase.expect.repositoryId,
    );
  }
  for (const testCase of identifiers.policies) {
    record(
      "identifiers",
      testCase,
      await outcome(() => collaboration.policyId(testCase.policy)),
      (value) => value === testCase.expect.policyId,
    );
  }
  for (const testCase of identifiers.objects) {
    record(
      "identifiers",
      testCase,
      await outcome(() => collaboration.objectId(
        testCase.repository, testCase.family, testCase.creator, testCase.objectKey, testCase.references,
      )),
      (value) => value === testCase.expect.objectId,
    );
  }

  const events = load("events");
  const policy = events.policies.primary;
  const byId = new Map(events.cases.map((testCase) => [testCase.id, testCase]));
  for (const testCase of events.cases) {
    const lines = [...testCase.context.map((id) => byId.get(id).rawText), testCase.rawText];
    const result = await fold(lines.map((line) => `${line}\n`).join(""), policy);
    const contextIds = testCase.context.map((id) => byId.get(id).expect.eventId);
    const accepted = result.audit.accepted;
    const extra = accepted.filter((id) => !contextIds.includes(id));
    const reasons = result.audit.rejected.map((item) => item.reason);
    const actual = extra.length === 1 && extra[0] === testCase.expect.eventId
      ? { valid: true, value: extra[0] }
      : { valid: false, error: reasons.length === 1 ? reasons[0] : `accepted=${extra.length}` };
    record("events", testCase, actual);
  }

  const folds = load("fold");
  for (const scenario of folds.scenarios) {
    const result = await fold(scenario.ndjson, folds.policies.primary);
    const telemetry = Object.fromEntries(
      Object.keys(scenario.expect.telemetry).map((key) => [key, result.telemetry[key]]),
    );
    const matches = result.stateDigest === scenario.expect.stateDigest
      && JSON.stringify(telemetry) === JSON.stringify(scenario.expect.telemetry)
      && JSON.stringify(result.telemetry.rejectionReasons) === JSON.stringify(scenario.expect.rejectionReasons);
    results.push({ set: "fold", id: scenario.id, ok: matches, value: { stateDigest: result.stateDigest, telemetry } });
  }

  process.stdout.write(JSON.stringify(results));
}

main().catch((error) => {
  process.stderr.write(`${error.stack || error}\n`);
  process.exitCode = 1;
});
