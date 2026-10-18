# FMAgents

The on-device coding agent, its providers (Claude, OpenAI, and Gemma 4 on this iPhone), the
session service with branch isolation, the node login terminal, quick launch from the composer,
and the Agents screens' view models. `import FMAgents` (deps FMBase, FMCrypto, FMNet, FMAccount,
FMApi, FMCode, FMGit, FMDiff, FMLocalAI). Everything is `@MainActor`. Views:
ForkMesh/Features/Agents.

## Workspace seam (`AgentWorkspace.swift`, `GitAgentWorkspace.swift`)

Tools and the session service use `protocol AgentWorkspace` (listFiles, readFile, writeFile,
status, stageAll, commit, branches, createBranch, checkout, currentBranch) with `Agent`-prefixed
value types, so views importing FMCode see no clashes. `GitAgentWorkspace(gitWorkspace)` adapts
FMCode's `GitWorkspace` and maps its errors to `AgentWorkspaceError` (tools print
"<tool> failed: <message>").

## Providers (`AgentProvider.swift`, `GemmaAgentProvider.swift`)

- `protocol AgentProvider` (`id`, `model`, `send(systemPrompt:history:tools:client:) -> AgentTurn`,
  `appendAssistant/appendToolResults/appendUser`). History is `[JSONValue]` in the provider's own
  shape.
- `AgentTurn { text, toolCalls, raw, inputTokens, outputTokens, refusal, wantsTools }`.
- `AgentProviderException(message, statusCode:, retryable:)`: retryable when there is no status or
  it is 408/409/429/5xx, unless overridden.
- `AnthropicProvider` (`claude-api`; refusals from `stop_reason: refusal`) and `OpenAiProvider`
  (`openai`); 600 s request timeout; both accept photos as image blocks.
- `GemmaAgentProvider` (`on-device`): `manager.ensureReady()` then `engine.complete(...)`, history
  kept OpenAI-shaped; every failure is a non-retryable `AgentProviderException`. History limit 20
  messages, tool output 8000 characters. See [FMLocalAI](FMLocalAI.md).
- `OnDeviceAgentReadiness.of(manager)`: ready, not ready (fixable in settings) or unavailable.

## Loop and tools (`CodingAgent.swift`, `AgentTools.swift`)

- `CodingAgent(provider:toolset:maxTurns: 24, maxHistoryMessages: 60, ...)`: `run(prompt) { event
  in }` is pull-based (a `cancel()`/`steer()` inside the handler is seen before the next step);
  one retry for retryable failures.
- `AgentEvent`: user message, assistant message, tool call, tool result, notice, done
  (`AgentStopReason`: completed, cancelled, turnLimit, error, refused).
- `AgentToolset(workspace, readOnly:)`: `read_file`, `write_file`, `replace_text`,
  `list_directory`, `grep`, `git_status`, `git_commit` (names as the desktop agent); output limit
  24 000 characters; strings handled by UTF-16 code unit; grep uses ICU regular expressions.

## Credentials (`AgentCredentials.swift`)

- `AgentProviderKind`: `.anthropic`, `.openai`, `.onDevice`, with labels, key validation and
  model lists.
- `AgentCredentials` builds the provider and agent (Gemma gets smaller history and tool-output
  bounds).
- `AgentCredentialStore`: `read`, `write`, `clear` under `agent/credentials-v1`
  (`{"kind","apiKey","model"}` in the secure store), `verify` (one cheap request; Gemma checks
  readiness without loading).

## Sessions (`AgentSessionService.swift`)

`AgentSessionService(prefs:staleAfter: 15 days)`: `submit(repoName:prompt:...)` queues a task or
steers the running session for that repository; `start(task:workspace:...)` sweeps stale records,
refuses a dirty worktree or a second run (`AgentStartException`), records the base branch before
creating `agent/<id6>-<words>`, runs, and restores the base branch afterwards unless changes are
uncommitted. Records persist under `agent/branch-records-v1`. `AgentRunHandle` exposes the task,
branches, events and completion.

## Login terminal (`AgentTerminal.swift`)

`TerminalScreen(rows, cols)` is a VT100/xterm subset with scrollback and link detection.
`AgentLoginTerminal(node:provider:channel:session:)` drives a provider CLI login on a node over the
account event channel: start, input, paste, keys, resize, close; exposes the screen, status, exit
code and sign-in link. `close()` waits 50 ms after the close frame because
`AccountEventChannel.send` writes from its own task.

## View models

- `protocol AgentsApi` (ApiService conforms): the reads and queue calls the screens make.
- `AgentsListModel`: subscribes to `AgentListSignal` (0.6 s debounce, no polling), groups running
  runs first, and tracks sealed counts and load state.
  - A ring never supersedes a load in flight, whether a ring or the first load started it. A
    ring that arrives before that load has sent its per-repository reads is answered by those
    reads. A ring that arrives after them queues one more load, which runs once the current load
    has published its rows.
  - The account socket rings as it connects at launch, about when the Agents tab's first load
    is waiting on the relay. Through 1.4.1 that ring discarded the first load and read every
    repository again, so each repository's `agents/list` and queue read went out twice and the
    list stayed on its skeleton until the second pass returned (17 s in one 1.4.1 report).
  - Repositories whose sealed-session or queue read answers 403/404 are not re-read for 10 minutes
    per account (`AgentReadRefusals.shared`, shared with the watch bridge).
  - Branched runs read `publishedPullHeads` (number, head and status only), refreshed when a
    branched run changes or every 5 minutes while some pull is still open.
  - Observable properties are written only when values change, to avoid redrawing the tab.
  - A failed read keeps rows already shown; finished queued runs outside the relay's listed window
    are kept (`AgentSessionList.keeping`, the desktop's rule). It rings on the `agents` and
    `cloud-agents` account topics.
- `AgentTranscriptModel`, `LaunchedAgentSessionModel` (queued run: refresh, follow-up, retry,
  cancel, pull chain).
- `AgentStartModel`: loads targets (mesh repositories and on-device mirrors), plan, mode, node
  choice, start on a node or on the device. `targetsNeedLoading` stays true until a mesh read
  returns repositories, so a launcher prepared before the network was up reloads later.
- `AgentScreenModel`: the on-device agent screen (load, send, stop, events, pull request
  preparation and submission, file references, attachments). `initialCredentials` overrides the
  stored provider for one run.
- `AgentSetupModel`, `AttachFileModel` (2000-file cap, `.git` skipped).
- `RunnerMergeModel`: merge a runner's branch, update it from its base (`updateRunnerAgent`;
  conflicts offer "Fix with agent"), then complete the linked task with the merge note
  (best-effort) and offer the merge commit's Actions.

## Quick launch from the composer (`QuickAgentLaunch.swift`, `AgentModelCatalog.swift`)

`QuickAgentLauncher` is the policy behind the shell composer's Send; it starts a run with no
second form.

- **Repository**: the current context, else the last used, else the account's most recently
  updated, else one on this device, else the most recently updated.
- **Destination**: the last choice for that repository, else the first online node that runs the
  model's provider, else this device, else the preferred node. Remembered under
  `agent/quick/last-repo` and `agent/quick/target/<repo>`.
- **Outcomes**: `.queued`, `.queuedWithoutSession`, `.device`, `.desktop`, `.needsFix(...)` (sign
  in, choose a repository, sign a CLI in on the node, set up this device, mirror first; the text
  stays), `.failed`, and `.done` for the actions below.
- **Desktop destination** (`.desktop(name)`): sends `agents.start` to the account's desktop over
  the control channel (LAN, else relay), trying the public owner, then the node owner, on
  `no_checkout`. Text only (no photos). `QuickLaunchRunOptions` adds YOLO (remembered under
  `agent/desktop/yolo`) and No worktree, sent as `vibe` and `worktree`; see
  `docs/native/DESKTOP_AGENTS.md`.
- **Models**: `AgentModelCatalog` lists the models a queued run can request, narrowed to the
  relay's `models` offer; `NodeModelPreference` keeps the chip's pick under
  `agent/node-model-alias`. On-device Gemma (`on-device-gemma`) is offered when the build and
  device can run it; picking it moves the run to the device and checks readiness (`.needsFix
  (.setUpGemma)` opens Settings › On-device AI).
- **Actions that start no agent** (`QuickPromptActions.swift`): the chip's menu starts with the
  desktop composer's non-agent rows plus a bug report: **Bug report**, **Task · assign**,
  **Manual · create issue**, **ActivityPub · post** and **Take a note**. The pick is remembered
  under `agent/quick/action` (`""` once a model is picked). Until something is picked, the chip
  shows **Bug report** whenever nothing can run an agent (`hasAgent`: no destination, or this
  iPhone with no model or key), so someone with no agent installed still has a working Send.
  Each sends through the app's `ShellPromptActions` and ends in `.done(message)`:
  - Bug report: `POST /api/bug-reports` (screen `composer`, photos attached), no account needed.
  - Task: `POST /api/tasks` like the desktop's `createQuickAddOrganizationTask` (first line as
    the title, up to 160 characters; the whole prompt as details; General; priority 50). The
    composer shows an **Assign to** menu, and the picker sheet an **Assign to** section, listing
    you first and then the board's members when you can manage it; the pick is kept under
    `tasks/composerAssignee`, empty is unassigned.
  - Manual · create issue: a signed issue on the chosen published repository (InboxService).
  - ActivityPub · post: `POST /api/repo/<owner>/<repo>/ap-publish` as a manual `discussion`
    `open`, signed like a release announcement.
  - Take a note: a private account note (`POST /api/notes`). The desktop keeps its notes on the
    computer; the phone has no local notes, so it uses the account's.
  Only the bug report takes photos. A spoken or Watch launch with a named repository always starts
  an agent, whatever the chip shows.

`PromptPhotoRules` (2048 px, JPEG without metadata, size limits) and `PromptMarkupRules` (the
PencilKit markup editor geometry; see `docs/native/CAPTURE_MARKUP.md`) back the composer's
photos. `CaptureAreaRules` holds the long-press capture's maths: the dragged box clamped to the
screen, the 16 pt minimum, the pixel crop and which accessibility elements count as content.

### Pasting images (`PromptPaste.swift`, app `PromptPasteboard.swift`)

Composers that take photos also accept pasted images (long-press Paste and Cmd-V).
`PromptPasteboard.read()` collects image items and images embedded in RTF(D); HTML images count
only as `data:image/...;base64` URIs (no remote fetch). `PromptPasteRules` detects formats by magic
bytes, keeps typed text, and caps the count; images then go through `PromptPhotoPreparer`
(2048 px, JPEG q80, no metadata, 5 MB each). Chat fields reuse this through `ChatTextField`.

## App target (ForkMesh/Features/Agents)

`AgentsFeature.register()` (after `LocalAIFeature.register`) registers `AgentSessionService` and
`AgentCredentialStore`. Entry points: `AgentsScreen`, `AgentStartCard`,
`AgentScreen(repoName:workspace:record:initialPrompt:autoStart:)`, `AgentLoginTerminalScreen`,
`AgentSetupSheet`.

## Not verified

SwiftUI views are syntax-checked on Linux. Gemma as an agent is tested through
`FakeLocalChatEngine`, not the MLX engine; how well a small on-device model follows the tool
protocol is untested. Terminal cell measuring and hardware-keyboard input are untested on a device.
