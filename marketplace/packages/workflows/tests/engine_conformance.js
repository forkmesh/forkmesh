"use strict";

const fs = require("fs");
const vm = require("vm");

const [enginePath, specPath, casesPath] = process.argv.slice(2);
const context = {};
vm.createContext(context);
vm.runInContext(fs.readFileSync(enginePath, "utf8") + "\nthis.ForkMeshWorkflows = ForkMeshWorkflows;", context);
const engine = context.ForkMeshWorkflows;
engine.load(fs.readFileSync(specPath, "utf8"));
const cases = JSON.parse(fs.readFileSync(casesPath, "utf8"));
const failures = [];

function plain(value) {
  return JSON.parse(JSON.stringify(value));
}

function withoutMs(value) {
  if (Array.isArray(value)) return value.map(withoutMs);
  if (value && typeof value === "object") {
    const out = {};
    for (const key of Object.keys(value)) {
      if (key !== "ms") out[key] = withoutMs(value[key]);
    }
    return out;
  }
  return value;
}

function same(a, b) {
  return JSON.stringify(sorted(a)) === JSON.stringify(sorted(b));
}

function sorted(value) {
  if (Array.isArray(value)) return value.map(sorted);
  if (value && typeof value === "object") {
    const out = {};
    for (const key of Object.keys(value).sort()) out[key] = sorted(value[key]);
    return out;
  }
  return value;
}

for (const item of cases.runs) {
  const responses = item.responses.slice();
  const requests = [];
  const report = engine.runWith({
    workflow: item.workflow,
    payload: item.payload,
    credentials: item.credentials,
    trigger: item.trigger,
    nowMs: cases.nowMs,
    workflowId: cases.workflowId,
    clock: () => 0
  }, (request) => {
    requests.push(plain(request));
    return responses.length ? responses.shift() : { status: 200, text: "{\"ok\": true, \"id\": 42, \"items\": [{\"n\": 1}, {\"n\": 2}]}" };
  });
  const got = { requests, report: withoutMs(plain(report)) };
  const want = { requests: item.expect.requests, report: withoutMs(item.expect.report) };
  if (!same(got, want)) failures.push({ name: item.name, got, want });
}

for (const item of cases.clean) {
  const got = plain(engine.clean(item.input));
  const want = item.error ? { error: item.error } : { workflow: item.workflow, error: "" };
  if (!same(got, want)) failures.push({ name: "clean " + JSON.stringify(item.input).slice(0, 60), got, want });
  if (item.workflow && engine.scheduleMinutes(item.workflow) !== item.scheduleMinutes) {
    failures.push({ name: "schedule", got: engine.scheduleMinutes(item.workflow), want: item.scheduleMinutes });
  }
}

for (const item of cases.safeUrl) {
  const got = engine.safeUrl(item.url);
  if (got !== item.safe) failures.push({ name: "safeUrl " + item.url.slice(0, 80), got, want: item.safe });
}

const catalog = plain(engine.catalog());
for (const key of Object.keys(cases.catalog)) {
  if (!same(catalog[key], cases.catalog[key])) failures.push({ name: "catalog " + key });
}

const started = JSON.parse(engine.start(JSON.stringify({
  workflow: cases.runs[0].workflow, payload: cases.runs[0].payload, trigger: "manual", nowMs: cases.nowMs
})));
if (!started.done) failures.push({ name: "start without requests should finish", got: started });

process.stdout.write(JSON.stringify({ checked: cases.runs.length + cases.clean.length + cases.safeUrl.length, failures }) + "\n");
