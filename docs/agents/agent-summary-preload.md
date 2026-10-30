# Claude Code agent summaries switched off

Added 2026-10-29. Code: `desktop/src/AgentSummaryPreload.h` (install and
`BUN_OPTIONS`), `desktop/src/AgentSummaryPreloadScript.h` (the script),
`desktop/tests/test_agent_summary_preload.cpp`.

## What it stops

Claude Code (since 2.1.232) starts a timer for each background agent or
subagent. About every 30 seconds the timer sends that agent's whole transcript
to the agent's own model (usually Opus) and asks for a 3–5 word description of
its latest action. The answer only feeds Claude Code's own agents-panel status
line. ForkMesh never shows that line, and it never reaches the model or the
conversation. In its OpenTelemetry export these calls are
`claude_code.api_request` events with `query_source = agent_summary`. Outside
measurements put them at 16–26 % of all Claude Code spend. Claude Code has no
setting to turn them off: `agentProgressSummaries: false` is ORed away, the
interval is a server flag clamped to 30–240 s, and the env vars that stop it
(`CLAUDE_CODE_FORK_SUBAGENT=0`, `CLAUDE_CODE_DISABLE_BACKGROUND_TASKS=1`) also
switch off subagents or background agents.

## How

The fix comes from Flamerix AI's gist
<https://gist.github.com/flamerix-ai/c3905705cc4205e5de3011fa2e74efce>
(`no-agent-summary.js` v2, MIT). We reviewed it and keep a pinned copy: the
desktop never downloads it. The only change from upstream is that comments
were moved here.

- The script is a Bun `--preload`. Claude Code's native binary is a compiled
  Bun program, so `BUN_OPTIONS="--preload <file>"` loads the script before
  Claude Code's own code runs. The binary itself is not modified.
- It wraps `setTimeout`/`setInterval` (global and `timers`). When a callback's
  source text (`Function.prototype.toString`, which works even on bytecode)
  contains `[AgentSummary] Timer fired` or `agent_summary`, it returns an
  inert handle of the same kind with a 2³¹−1 ms delay (a larger delay is
  treated as 1 ms), unref'd, so `clearTimeout` still works and the timer never
  fires. Every other timer passes through. Each callback's verdict is cached
  in a `WeakMap`.
- It copies `__promisify__` and `util.promisify.custom` from the original
  timer. Without them, `util.promisify(setTimeout)` calls the wrapper with the
  callback last and node throws `ERR_INVALID_ARG_TYPE`.
- It installs only when `Bun.main` starts with `/$bunfs/` (a compiled Bun
  binary). Any other Bun program that inherits `BUN_OPTIONS` loads it and does
  nothing. It makes no network calls and writes files only to the optional
  `CC_AGENT_SUMMARY_LOG` / `CC_AGENT_SUMMARY_MARKER` paths.

## Where the desktop applies it

`forkmesh::agentsummary::applyEnvironment(env)` writes the script to
`<data>/agents/no-agent-summary.js` (falling back to `<cache>/agents`) only
when the file is missing or different, then appends
`--preload <path>` to `BUN_OPTIONS`, once. The two Claude Code launch paths
call it:

- `ClaudeStreamSession` (every stream-json Claude Code session), before its
  extra environment, so a session's own `BUN_OPTIONS` entry still wins;
- `AgentRunner::processEnvironment` for the `claude-code` provider in the agent
  phase, before `extraEnvironment`.

Claude Code agents started by the Claude app (`ClaudeDesktopSessions`) and
short `claude -p` helpers (title and triage) are not covered. The helpers
never run long enough for a summary timer.

Rules the helper enforces, all found by testing against Claude Code 2.1.309
with Bun 1.4.4:

- **A missing preload stops Claude Code from starting**
  (`error: preload not found`, exit 1). So the variable is set only after the
  file has been written, and is left alone when the write fails.
- **Bun does not unquote `BUN_OPTIONS`.** A quoted path with a space starts
  Claude Code without loading the preload. Paths with whitespace, quotes or
  backslashes are skipped. That is why the cache folder is the fallback: the
  legacy macOS data folder is `~/Library/Application Support/ForkMesh`.
- **Windows is skipped.** The `/$bunfs/` gate most likely never matches there,
  and the gist was never tested on Windows.
- **The VM workspace drops it.** `applyGuestEnvironmentPolicy` removes
  `BUN_OPTIONS`, because the host path does not exist in the guest.

## Turning it off, checking it

- Set `CC_AGENT_SUMMARY_MODE=pass` in the desktop's environment or in an
  agent's extra environment. The script still loads but arms every timer, so
  summaries come back. This is also the A/B control.
- Set `CC_AGENT_SUMMARY_LOG=/abs/file` to log `preload active mode=…`, the
  first 20 matched timers (`summary timer #1 suppressed (setTimeout,
  delay=30000)`) and a count at exit.
- With OpenTelemetry export on, `query_source = agent_summary` should stay at
  zero. That is the only check that catches every failure.

## Fail-open, after each Claude Code update

The script matches on source text, so a Claude Code update can quietly bring
summaries back without breaking anything else. That happens if the log line or
query-source id is renamed, if the callback is passed through `fn.bind(...)` or
another wrapper, if the strings move outside the callback, or if the timer
moves to `timers/promises` or a scheduler loop. After an update, check that
`grep -a -F '[AgentSummary] Timer fired' "$(readlink -f "$(command -v claude)")"`
still finds the string, and that the log line above appears in a run with a
subagent. Upstream tested 2.1.257–2.1.312. We checked 2.1.309 on 2026-10-29
with `claude -p --model haiku`, `CLAUDE_CODE_AGENT_PROGRESS_SUMMARIES=1`, and
one subagent running `sleep 20` five times (about 2 minutes). With
`pass`: 4 timers matched, `[AgentSummary] Forking` appeared 3 times in
`--debug-file`. With `off`: the first timer was suppressed, 0 forks.

## License and terms

The gist warns that changing a client's runtime behaviour may conflict with
the terms of the service it is used with, and that checking this is on the
user. The script is unofficial and not affiliated with Anthropic. ForkMesh
ships it in every desktop build, so checking those terms is our job.

`no-agent-summary.js` is used under the MIT License:

> Copyright (c) 2026 Flamerix AI (https://flamerix.ai)
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in
> all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.
