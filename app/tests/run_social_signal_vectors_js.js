"use strict";

// Runs the JavaScript social-signal implementation (app/public/social-signals.js).
//   node run_social_signal_vectors_js.js vectors   check the committed vectors
//   node run_social_signal_vectors_js.js interop   stdin: bundles to merge, a seed
//       to sign one new signal and one revocation with; stdout: the merged
//       self-audience bundle and folded state, for Python to verify.
// Results mirror tools/social_signals.py run_vectors so the gate can compare
// case counts one to one.

if (!globalThis.crypto) {
  globalThis.crypto = require("node:crypto").webcrypto;
}

const fs = require("node:fs");
const path = require("node:path");
const { isDeepStrictEqual } = require("node:util");
const signals = require(path.join(__dirname, "..", "public", "social-signals.js"));

const VECTORS = path.join(__dirname, "..", "..", "conformance", "social-signals", "v1", "vectors.json");

async function outcome(call) {
  try {
    return { valid: true, value: await call() };
  } catch (error) {
    if (!(error instanceof signals.SocialSignalError)) throw error;
    return { valid: false, error: error.code };
  }
}

function bundleInput(testCase) {
  if (testCase.emptySignals) {
    return { schema: signals.BUNDLE_SCHEMA, exportedAt: 0, signals: new Array(testCase.emptySignals).fill({}), revocations: [] };
  }
  return testCase.input;
}

async function runVectors() {
  const vectors = JSON.parse(fs.readFileSync(VECTORS, "utf8"));
  const results = [];
  const record = (group, id, ok, detail) => results.push({ group, id, ok: Boolean(ok), detail: ok ? null : detail });

  for (const testCase of vectors.records) {
    const actual = await outcome(() => signals.validateSignal(testCase.input, testCase.asOf));
    const got = actual.valid
      ? { valid: true, signalId: actual.value.signalId, summary: signals.describe(actual.value) }
      : actual;
    record("records", testCase.id, isDeepStrictEqual(got, testCase.expect), got);
  }
  for (const testCase of vectors.revocations) {
    const actual = await outcome(() => signals.validateRevocation(testCase.input, testCase.asOf));
    const got = actual.valid ? { valid: true, revocationId: actual.value.revocationId } : actual;
    record("revocations", testCase.id, isDeepStrictEqual(got, testCase.expect), got);
  }
  for (const scenario of vectors.scenarios) {
    const expect = scenario.expect;
    for (const order of scenario.orders) {
      const store = new signals.SignalStore();
      const reports = new Map();
      for (const index of order) reports.set(index, await store.importBundle(scenario.bundles[index], scenario.asOf));
      const state = await store.fold(scenario.asOf);
      record("merge", `${scenario.id}:order-${order.join("")}`, isDeepStrictEqual(state, expect.state), state.digest);
      if (order.every((value, index) => index === 0 || order[index - 1] < value)) {
        const imports = order.map((index) => reports.get(index));
        record("import", scenario.id, isDeepStrictEqual(imports, expect.imports), imports);
        for (const [audience, bundle] of Object.entries(expect.exports)) {
          const [name, signerName] = audience.split("-");
          const signer = signerName ? vectors.keys[signerName].publicKey : null;
          const exported = store.exportBundle(name, scenario.asOf, signer);
          record("export", `${scenario.id}:${audience}`, isDeepStrictEqual(exported, bundle), exported);
          const roundTrip = new signals.SignalStore();
          await roundTrip.importBundle(JSON.parse(JSON.stringify(exported)), scenario.asOf);
          record("export", `${scenario.id}:${audience}:reimport`,
            isDeepStrictEqual(roundTrip.exportBundle(name, scenario.asOf), bundle), null);
        }
        for (const testCase of expect.recommendations) {
          const got = await store.recommend(testCase.controls);
          record("recommend", `${scenario.id}:${testCase.id}`, isDeepStrictEqual(got, testCase.output), got);
        }
        for (const testCase of vectors.invalidControls) {
          const got = await outcome(() => store.recommend(testCase.controls));
          record("controls", testCase.id, isDeepStrictEqual(got, { valid: false, error: testCase.error }), got);
        }
      }
    }
  }
  for (const testCase of vectors.invalidBundles) {
    const got = await outcome(() => new signals.SignalStore().importBundle(bundleInput(testCase), 0));
    record("bundles", testCase.id, isDeepStrictEqual(got, { valid: false, error: testCase.error }), got);
  }
  return { implementation: "javascript", results };
}

async function runInterop(input) {
  const seed = Uint8Array.from(Buffer.from(input.seed, "hex"));
  const store = new signals.SignalStore();
  const reports = [];
  for (const bundle of input.bundles) reports.push(await store.importBundle(bundle, input.asOf));
  const revocation = await signals.signRevocation(seed, input.revoke, input.asOf, "withdrawn");
  const signed = await signals.signSignal(seed, input.sign);
  reports.push(await store.importBundle({
    schema: signals.BUNDLE_SCHEMA, exportedAt: input.asOf, signals: [signed], revocations: [revocation],
  }, input.asOf));
  return { reports, bundle: store.exportBundle("self", input.asOf), state: await store.fold(input.asOf) };
}

async function main() {
  const command = process.argv[2] || "vectors";
  let output;
  if (command === "vectors") {
    output = await runVectors();
  } else if (command === "interop") {
    output = await runInterop(JSON.parse(fs.readFileSync(0, "utf8")));
  } else {
    throw new Error(`unknown command ${command}`);
  }
  process.stdout.write(`${JSON.stringify(output)}\n`);
  if (command === "vectors" && output.results.some((item) => !item.ok)) process.exitCode = 1;
}

main().catch((error) => {
  process.stderr.write(`${error.stack || error}\n`);
  process.exitCode = 2;
});
