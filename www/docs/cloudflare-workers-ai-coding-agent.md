# Cloudflare Workers AI coding agent

ForkMesh ships a provider-neutral coding runtime backed by Cloudflare Workers
AI. The same runtime runs an installed local model (Gemma 4 through Ollama, or
any llama.cpp-style OpenAI-compatible server) as a first-class agent backend.
It uses only the Python standard library and is embedded in the desktop
release; the canonical, testable source is `tools/forkmesh_coding_agent.py`.

## Architecture

The runtime separates model transport from repository operations:

| Component | Responsibility |
| --- | --- |
| `Agent` | Reusable model/tool/final-answer loop |
| `Planner` | Captures Git state before starting work |
| `Conversation` | Role-separated chat and tool history |
| `Memory` | Repository context, project summary, edited files, token budget, and compaction |
| `Session` | Atomic JSON persistence and usage metadata |
| `ToolRegistry` | Extensible JSON-schema catalog and dispatch |
| `CloudflareProvider` | Workers AI chat completions, SSE, tool calls, retries, and cancellation |
| `OllamaProvider` | Ollama's native `/api/chat`: context sizing, thinking control, keep-alive, exact token counts, tools-capable model discovery |
| `LocalLlamaProvider` | llama.cpp-style OpenAI-compatible transport (used when the local endpoint is not Ollama) |
| `PromptBuilder` | System, developer, memory, conversation, and tool-role separation |
| `CommandExecutor` | Bounded asynchronous shell execution |
| `GitManager` | Status, diff, commit, branch, stash, and restore |
| `FileEditor` | Whole-file, exact-text, and patch edits with preview and undo |
| `SearchEngine` | Bounded directory, glob, and text search |

The Cloudflare Agents SDK is not required. ForkMesh keeps the loop local because
the desktop worktree, Git identity isolation, session UI, and patch capture are
already owned by the desktop agent runner. The provider boundary is deliberately
small so another chat-completions provider can be added without changing tools
or session state.

The default tools are `read_file` (whole file or a `start_line`–`end_line`
range), `write_file`, `replace_text`, `apply_patch`, `undo_edit`,
`search_files`, `grep`, `list_directory`, `execute_command`, `git_status`,
`git_diff`, `git_commit` (stages every change, or only `paths`), `git_log`,
`git_branch`, `git_stash`, `git_restore`, `run_tests`, and `lint_project`. A
compatibility `bash` tool remains available for smaller instruction models.

The loop is built for small models as much as large ones. The first prompt
carries the tracked-file tree so the model does not guess paths; argument
types are coerced to the tool schema and a missing argument or unknown tool
comes back with the tool's signature; a tool call the model writes as JSON
text (a fenced block, `<tool_call>`, or a bare object) runs as if it had used
the structured channel; every tool result is bounded; a reply cut off at the
output cap earns one nudge to work in smaller steps; identical repeated calls
are not re-run and three stalled turns end the run; and old tool outputs are
elided before the conversation is summarized.

## Desktop: Cloudflare Workers AI

Choose **Cloudflare AI** in an issue or prompt composer, then choose a model
offered by the relay. The desktop creates an isolated worktree, signs a
time-limited run ticket, and sends each provider turn to `/api/ai/agent`. Tool
calls run only in the desktop worktree; the relay never receives filesystem
access. The model catalog comes from `/api/forkbot/models`, so deploying a new
model does not require rebuilding the desktop.

The relay requires the Workers AI binding in `app/wrangler.toml`:

```toml
[ai]
binding = "AI"
```

Set `FORKBOT_AI_MODEL` in the deployed Worker configuration to the desired
default. Keep tokens in Worker secrets or the desktop's local settings, never
in a repository.

## Complete Gemma example

The library can also call Cloudflare's OpenAI-compatible API directly. Model,
account, token, endpoint, generation limits, and retry policy are runtime
configuration; no model identifier is compiled into `CloudflareProvider`.

```bash
export FORKMESH_AGENT_PROVIDER=cloudflare
export FORKMESH_AGENT_ACCOUNT_ID='your-account-id'
export FORKMESH_AGENT_API_TOKEN='your-workers-ai-token'
export FORKMESH_AGENT_MODEL='@cf/google/gemma-4-26b-a4b-it'
export FORKMESH_AGENT_TEMPERATURE=0.2
export FORKMESH_AGENT_MAX_OUTPUT_TOKENS=2000
export FORKMESH_AGENT_CONTEXT_WINDOW=32000
export FORKMESH_AGENT_TIMEOUT=600
export FORKMESH_AGENT_STREAMING=true
export FORKMESH_AGENT_RETRY_COUNT=4
export FORKMESH_AGENT_SESSION_PATH=/tmp/forkmesh-gemma-session.json

python3 tools/forkmesh_coding_agent.py issue.md
```

`issue.md` contains the coding request. Run this command from the checked-out
worktree. The account ID derives the standard endpoint; set
`FORKMESH_AGENT_ENDPOINT` to use a custom Workers AI or AI Gateway endpoint.
`CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID` are accepted as credential
fallbacks.

The loop sends the role-separated conversation and tool schemas, executes each
requested tool, records the result as a tool message, and continues until the
model gives a final answer. Tool turns stay quiet: their planning text and
private `<think>`, `<analysis>`, or `<reasoning>` blocks never enter the run
log, while the actual tool call and result remain visible. A completed final
answer is then shown as one concise reply. Reasoning deltas remain hidden unless
`FORKMESH_AGENT_SHOW_REASONING=true` is explicitly set.

## Local Llama

ForkMesh detects an installed Ollama-compatible server at
`http://127.0.0.1:11434/v1/chat/completions`. Start Ollama, install a
tools-capable model (the marketplace's **Gemma 4** package pulls one), then
choose **Local Llama** in the desktop. Leave the model blank to select the
first installed model that can call tools, or enter its installed name under
**Settings → Agents → Local Llama model**.

When the endpoint's host answers `GET /api/version`, the runtime switches to
Ollama's native `/api/chat` route. That route lets the agent size the
model's context (`num_ctx`) to its own `FORKMESH_AGENT_CONTEXT_WINDOW` plus
headroom, capped at the model's trained length; turn thinking off (the
default, since on a CPU-bound host it multiplies the time per step) or on
through **Settings → Agents → Local Llama thinking**; keep the model loaded
for 30 minutes between tool runs; and read exact prompt and generation
counts, which the session's network panel shows alongside tokens per second.
A model without the `tools` capability is refused with a message naming a
model that has it. Set `FORKMESH_AGENT_LOCAL_API=openai` to force the
OpenAI-compatible route (llama.cpp, vLLM), or `ollama` to skip the probe.

The equivalent command-line session is:

```bash
export FORKMESH_AGENT_PROVIDER=local-llama
export FORKMESH_AGENT_ENDPOINT=http://127.0.0.1:11434/v1/chat/completions
export FORKMESH_AGENT_MODEL='the-name-shown-by-ollama-list'
python3 tools/forkmesh_coding_agent.py issue.md
```

For llama.cpp, point the endpoint at its OpenAI-compatible chat-completions
route and set the model explicitly. Local mode sends no Cloudflare credentials
and has no provider charge.

### Ollama serving parameters

The native route sends `num_ctx`, `num_predict`, and `temperature` itself; only
the OpenAI-compatible route depends on the installed tag, where Ollama's
4096-token default context silently truncates conversations sized for
`FORKMESH_AGENT_CONTEXT_WINDOW`. On either route a partial GPU offload can
abort the runner: Gemma-class models split across a
Vulkan device terminate with
`GGML_ASSERT(n_inputs < GGML_SCHED_MAX_SPLIT_INPUTS) failed`, which the endpoint
returns as an HTTP 500. Pin both on the tag before selecting it:

```bash
cat > Modelfile <<'EOF'
FROM gemma4:e4b
PARAMETER num_ctx 32768
PARAMETER num_gpu 0
EOF
ollama create gemma4:e4b -f Modelfile
```

`num_gpu 0` keeps every layer on the CPU, and `ollama show <model> --parameters`
reports what a tag currently serves. Measure before offloading; a many-core host
can outrun a small consumer GPU on both prefill and generation.

## Configuration reference

All values are optional unless required by the selected transport:

| Environment variable | Meaning | Default |
| --- | --- | --- |
| `FORKMESH_AGENT_PROVIDER` | `cloudflare`, `local-llama`, or another compatible provider | `cloudflare` |
| `FORKMESH_AGENT_MODEL` | Runtime model identifier | Provider/discovery default |
| `FORKMESH_AGENT_ENDPOINT` | Full chat-completions URL | Derived for Cloudflare; Ollama URL locally |
| `FORKMESH_AGENT_API_TOKEN` | Bearer token for direct HTTP | Empty |
| `FORKMESH_AGENT_ACCOUNT_ID` | Cloudflare account ID used to derive the endpoint | Empty |
| `FORKMESH_AGENT_TEMPERATURE` | Sampling temperature | `0.2` |
| `FORKMESH_AGENT_MAX_OUTPUT_TOKENS` | Maximum tokens per provider turn | `2000` |
| `FORKMESH_AGENT_CONTEXT_WINDOW` | Conversation budget before compaction | `32000` |
| `FORKMESH_AGENT_TIMEOUT` | Provider and command timeout in seconds | `600` |
| `FORKMESH_AGENT_STREAMING` | Enable SSE text streaming | `true` |
| `FORKMESH_AGENT_RETRY_COUNT` | Retries for rate limits, 5xx, and network failures | `4` |
| `FORKMESH_AGENT_RETRY_BASE_DELAY` | Initial exponential-backoff delay | `1` |
| `FORKMESH_AGENT_MAX_TURNS` | Hard agent-loop limit | `120` |
| `FORKMESH_AGENT_MAX_IDLE_TURNS` | Consecutive turns that only redo earlier work before the run gives up | `100` |
| `FORKMESH_AGENT_SHOW_REASONING` | Emit provider reasoning deltas | `false` |
| `FORKMESH_AGENT_SESSION_PATH` | Optional durable JSON session path | Empty |
| `FORKMESH_AGENT_TEST_COMMAND` | Command used by `run_tests` | Empty |
| `FORKMESH_AGENT_LINT_COMMAND` | Command used by `lint_project` | Empty |
| `FORKMESH_AGENT_PREVIEW_EDITS` | Preview structured edits instead of applying them | `false` |
| `FORKMESH_AGENT_TOOL_OUTPUT_LIMIT` | Characters kept per tool result (head and tail) | `12000` |
| `FORKMESH_AGENT_THINKING` | Local models: `off`, `on`, or `low`/`medium`/`high` where supported | `off` |
| `FORKMESH_AGENT_KEEP_ALIVE` | How long Ollama keeps the model loaded between turns | `30m` |
| `FORKMESH_AGENT_LOCAL_API` | `ollama` or `openai` to skip the local backend probe | Auto-detected |

## Editing and recovery

Structured paths are repository-relative and reject absolute paths, `..`
escapes, and symlink escapes. `write_file`, `replace_text`, and `apply_patch`
support previews. Applied `FileEditor` batches can be reverted with `undo_edit`;
Git-level recovery is available through stash and restore. Commands have closed
stdin, bounded output, and a timeout. ForkMesh isolates attempted global Git
configuration writes so an agent cannot replace the user's identity, disables
interactive credential prompts, and never pushes automatically.

Run the core unit suites with:

```bash
python3 -m pytest -q app/tests/test_coding_agent.py app/tests/test_local_agent.py
```

When changing `tools/forkmesh_coding_agent.py`, regenerate the desktop asset:

```bash
python3 tools/sync_coding_agent_header.py
```
