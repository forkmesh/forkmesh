# Cloudflare Workers AI coding agent

`tools/forkmesh_coding_agent.py` is a standard-library coding agent with
pluggable model providers: Cloudflare Workers AI, Ollama, or any
OpenAI-compatible local server. The desktop embeds it.

## Architecture

The loop (`Agent`, `Conversation`, `Memory`, `Session`, `PromptBuilder`) is
separate from transport (`CloudflareProvider`, `OllamaProvider`,
`LocalLlamaProvider`) and from repository tools (`ToolRegistry`, `FileEditor`,
`GitManager`, `SearchEngine`, `CommandExecutor`). Tools include `read_file`,
`write_file`, `replace_text`, `apply_patch`, `undo_edit`, `grep`,
`execute_command`, `git_*`, `run_tests` and `lint_project`. Tool arguments are
coerced to schema, tool results are bounded, and repeated or stalled turns end
the run.

## Desktop

Choosing **Cloudflare AI** creates an isolated worktree and sends each turn
straight to Cloudflare's `/ai/v1/chat/completions` with the account ID and API
token saved in Settings (`CLOUDFLARE_ACCOUNT_ID`, `CLOUDFLARE_API_TOKEN`);
tools run only locally, and no turn passes through the relay. Models come from
Cloudflare's `/ai/models/search`, with a built-in list when that is
unreachable. **Local Llama** uses an Ollama server at
`127.0.0.1:11434`; when `GET /api/version` answers, the native `/api/chat`
route is used, which sizes `num_ctx`, controls thinking, and keeps the model loaded.

## Command line

```bash
export FORKMESH_AGENT_PROVIDER=cloudflare   # or local-llama
export FORKMESH_AGENT_ACCOUNT_ID=… FORKMESH_AGENT_API_TOKEN=…
export FORKMESH_AGENT_MODEL='@cf/google/gemma-4-26b-a4b-it'
python3 tools/forkmesh_coding_agent.py issue.md
```

Other `FORKMESH_AGENT_*` settings: `ENDPOINT`, `TEMPERATURE` (0.2),
`MAX_OUTPUT_TOKENS` (2000), `CONTEXT_WINDOW` (32000), `TIMEOUT` (600),
`STREAMING`, `RETRY_COUNT` (4), `MAX_TURNS` (120), `SESSION_PATH`,
`TEST_COMMAND`, `LINT_COMMAND`, `PREVIEW_EDITS`, `TOOL_OUTPUT_LIMIT`,
`THINKING`, `KEEP_ALIVE`, `LOCAL_API` (`ollama` or `openai`),
`SHOW_REASONING`.

## Safety

Paths are repository-relative with no `..` or symlink escapes; commands have
closed stdin, bounded output and a timeout; global Git config writes are
isolated; nothing is pushed automatically. Tests:
`app/tests/test_coding_agent.py`, `app/tests/test_local_agent.py`. After
editing the tool, run `python3 tools/sync_coding_agent_header.py`.
