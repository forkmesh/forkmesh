#pragma once

#include <QByteArray>

namespace forkmesh::agentsummary {

inline QByteArray script()
{
    return QByteArrayLiteral(R"JS(// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Flamerix AI (https://flamerix.ai)
const IN_COMPILED_BUN =
  typeof Bun !== "undefined" && typeof Bun.main === "string" && Bun.main.startsWith("/$bunfs/");

function install() {
  const fs = require("fs");
  const ANCHORS = ["[AgentSummary] Timer fired", "agent_summary"];
  const RAW_MODE = process.env.CC_AGENT_SUMMARY_MODE || "off";
  const MODE = RAW_MODE === "pass" ? "pass" : "off";
  if (RAW_MODE !== MODE) process.stderr.write(`[no-agent-summary] unknown mode ${RAW_MODE}, forcing off\n`);
  const LOG = process.env.CC_AGENT_SUMMARY_LOG;
  const log = (s) => { if (LOG) { try { fs.appendFileSync(LOG, `${new Date().toISOString()} pid=${process.pid} ${s}\n`); } catch {} } };

  const verdict = new WeakMap();
  function isSummaryTimer(fn) {
    if (typeof fn !== "function") return false;
    let v = verdict.get(fn);
    if (v === undefined) {
      try {
        const src = Function.prototype.toString.call(fn);
        v = ANCHORS.some((a) => src.includes(a));
      } catch { v = false; }
      verdict.set(fn, v);
    }
    return v;
  }

  let matched = 0;
  let suppressed = 0;
  const PROMISIFY = Symbol.for("nodejs.util.promisify.custom");
  const wrappers = new Map();
  const hookedFns = new WeakSet();
  function wrap(orig, name) {
    if (typeof orig !== "function") return orig;
    if (wrappers.has(orig)) return wrappers.get(orig);
    const hooked = function (fn, delay, ...args) {
      if (isSummaryTimer(fn)) {
        matched++;
        if (MODE === "off") {
          suppressed++;
          if (matched <= 20) log(`summary timer #${matched} suppressed (${name}, delay=${delay})`);
          const t = orig(() => {}, 2 ** 31 - 1);
          if (t && typeof t.unref === "function") t.unref();
          return t;
        }
        if (matched <= 20) log(`summary timer #${matched} passed (control, ${name}, delay=${delay})`);
      }
      return orig(fn, delay, ...args);
    };
    Object.defineProperty(hooked, "name", { value: name });
    if (orig.__promisify__) hooked.__promisify__ = orig.__promisify__;
    if (orig[PROMISIFY]) hooked[PROMISIFY] = orig[PROMISIFY];
    wrappers.set(orig, hooked);
    hookedFns.add(hooked);
    return hooked;
  }

  const timers = (() => { try { return require("timers"); } catch { return null; } })();
  const hookedApis = [];
  for (const name of ["setTimeout", "setInterval"]) {
    globalThis[name] = wrap(globalThis[name], name);
    if (hookedFns.has(globalThis[name])) hookedApis.push(`global.${name}`);
    if (timers) {
      try { timers[name] = wrap(timers[name], name); } catch {}
      if (hookedFns.has(timers[name])) hookedApis.push(`timers.${name}`);
    }
  }

  const active = `preload active mode=${MODE} bun=${Bun.version} hooked=${hookedApis.join(",")}`;
  log(active);
  if (LOG) process.on("exit", () => log(`exit: ${suppressed} summary timer(s) suppressed, ${matched} matched`));
  const MARKER = process.env.CC_AGENT_SUMMARY_MARKER;
  if (MARKER) {
    try { fs.writeFileSync(MARKER, `${active} pid=${process.pid}\n`); }
    catch (e) { process.stderr.write(`[no-agent-summary] cannot write marker ${MARKER}: ${e.message}\n`); }
  }
}

if (IN_COMPILED_BUN) install();
)JS");
}

} // namespace forkmesh::agentsummary
