# FMAgents

The on-device coding agent, its providers (Claude, OpenAI, and Gemma 4 on this
iPhone), the session service with branch isolation, the node login terminal,
and the Agents screens' view models. `import FMAgents` (deps FMBase, FMCrypto,
FMNet, FMAccount, FMApi, FMCode, FMGit, FMDiff, FMLocalAI). Everything is
`@MainActor`.

Views: ForkMesh/Features/Agents.

## Workspace seam (`AgentWorkspace.swift`, `GitAgentWorkspace.swift`)

The tools and the session service talk to `protocol AgentWorkspace` (listFiles,
readFile, writeFile, status, stageAll, commit, branches, createBranch,
checkout; `currentBranch()`), with `Agent`-prefixed value
types (`AgentFileEntry`, `AgentFileContent`, `AgentChange(Kind)`,
`AgentWorktreeStatus`, `AgentBranch`, `AgentAuthor`), so a view importing FMCode
too sees no clashes. `GitAgentWorkspace(gitWorkspace)` adapts FMCode's
`GitWorkspace` and turns `GitWorkspaceException` into `AgentWorkspaceError`
(the tools print "<tool> failed: <message>"). `AgentAuthor.fromAccount(nodeName:email:)`.

## Providers (`AgentProvider.swift`, `GemmaAgentProvider.swift`)

- `protocol AgentProvider` (`id`, `model`, `send(systemPrompt:history:tools:client:) -> AgentTurn`, `appendAssistant/appendToolResults/appendUser(&history, ...)`). History is `[JSONValue]` in the provider's own shape.
- `AgentTurn { text, toolCalls: [AgentToolCall], raw, inputTokens, outputTokens, refusal, wantsTools }`, `AgentToolCall { id, name, arguments: JSONObject }`.
- `AgentProviderException(message, statusCode:, retryable:)`: `isRetryable` = no status, 408/409/429/5xx, unless `retryable` overrides.
- `AnthropicProvider(apiKey:model:maxTokens:baseUrl:)` (`claude-api`; Opus 5 / Fable 5 get `fallbacks: "default"` + the beta header; refusals from `stop_reason: refusal`), `OpenAiProvider` (`openai`; JSON-string arguments). 600 s request timeout. Errors: "Claude API error 401: <message>".
- `GemmaAgentProvider(manager: LocalModelManager? = nil, parameters: .agent)` (`on-device`): `manager` nil = `Services.get(LocalModelManager.self)` at each turn. `send` calls `manager.ensureReady()` (loads the model) then `manager.engine.complete(...)`; history stays OpenAI-shaped (`LocalChatMessage.openAIJSON`). Every failure (switch off, not downloaded, engine unavailable, generation error, no manager) is an `AgentProviderException` with `retryable: false`, message = `LocalModelManager.describe`. `maxHistoryMessages` 20, `maxToolOutputChars` 8000.
- `OnDeviceAgentReadiness.of(manager)`: `.ready(modelName:)`, `.notReady(reason:)` (fixable in On-device AI settings: `canBeFixedInSettings`), `.unavailable(reason:)`; `message`. Loads nothing.

## Loop and tools (`CodingAgent.swift`, `AgentTools.swift`)

- `CodingAgent(provider:toolset:maxTurns: 24, maxHistoryMessages: 60, client:, retryDelay: 2, sleep:)`. `run(prompt) { event in }` hands each `AgentEvent` to the closure and waits for it (pull-based: a `cancel()`/`steer()` made inside the handler is seen before the next step). `runToEnd(prompt) -> [AgentEvent]`. `steer`, `cancel`, `history`, `systemPrompt`, `isRunning`, `hasHistory`. One retry for retryable failures; non-provider errors become "Could not reach the model: ...".
- `enum AgentEvent`: `.userMessage`, `.assistantMessage`, `.toolCall`, `.toolResult`, `.notice(text, isError:)`, `.done(AgentDone { reason, turns, inputTokens, outputTokens })`; accessors `userText`, `assistantText`, `toolCall`, `toolResult`, `noticeText`, `done`. `AgentStopReason`: completed, cancelled, turnLimit, error, refused.
- `AgentToolset(workspace, readOnly:)`: `tools` (read_file, write_file, replace_text, list_directory, grep, git_status, git_commit; names as the desktop agent), `invoke(callId, name, args) -> AgentToolResult`, `author`, `outputLimit` (24 000 by default). Strings are handled by UTF-16 code unit; grep uses NSRegularExpression (ICU, not JS syntax).

## Credentials (`AgentCredentials.swift`)

- `AgentProviderKind`: `.anthropic`, `.openai`, `.onDevice` (raw value is what the keychain stores). `label` ("Gemma 4"), `pickerLabel` ("Gemma 4 (on this iPhone)"), `needsKey`, `looksLikeKey`, `keyPrefix`, `consoleUrl`, `models` (Gemma: the Local AI catalog).
- `AgentCredentials(kind:apiKey:model:)`, `.onDevice(model:)`, `isUsable`, `maskedKey`, `modelLabel`, `build(localModels:)` (selects the catalog model in the manager for `.onDevice`), `makeAgent(toolset:localModels:client:)` (Gemma gets the smaller history and tool-output bounds).
- `AgentCredentialStore(secureStore:http:localModels:)`: `read()`, `write(_)`, `clear()` under `agent/credentials-v1` (`{"kind","apiKey","model"}`), `verify(_) async -> String?` (one cheap request; Gemma: readiness, no load).

## Sessions (`AgentSessionService.swift`)

`AgentSessionService(prefs:staleAfter: 15 days, clock:)`, `@Observable`, registered by `AgentsFeature.register()`. `load()`, `submit(repoName:prompt:priority:id:) -> AgentTask?` (nil = steered the running session of that repo), `nextTask`, `removeTask`, `start(task:workspace:buildAgent:author:readOnly:) async throws -> AgentRunHandle` (sweeps stale records, refuses a dirty worktree or a second run with `AgentStartException(refusal, message)`, records the branch before creating `agent/<id6>-<words>`, runs eagerly, restores the base branch afterwards unless changes are uncommitted), `cancelActive()`, `isRunning`, **`runningRepo`**, `activeRun`, `lastRun`, `records`, `queue`, `sweepStaleRecords(workspace:repoName:now:)`, `agentBranchName(_:_:)`, `addListener`. Records persist under `agent/branch-records-v1` with ISO 8601 dates.
`AgentRunHandle` (`@Observable`): `task`, `branch`, `baseBranch`, `events`, `isComplete`, `lastDone`, `waitUntilDone()`.

## Login terminal (`AgentTerminal.swift`)

`TerminalScreen(rows, cols)` (VT100/xterm subset: `feed`, `grid`, `scrollback`, `allRows`, `cx/cy`, `resized`, `links()`, `wrappedUrlTargets`), `TerminalCell`, `TerminalRow` (class), `TerminalKey` + `terminalKeyBytes`, `terminalControlBytes`, `agentLoginProviders`, `agentProviderLabel`, `isAgentLoginProvider`. `AgentLoginTerminal(node:provider:channel: AccountEventChannel, session:openTimeout:inputFlush:)` (`@Observable`): `start(cols:rows:)`, `input`, `paste`, `key`, `resize(cols:rows:)`, `close() async`; `screen`, `revision` (bumped per output burst; the screen is drawn into in place), `statusText`, `statusLevel`, `live`, `exitCode`, `closed`, `links`, `signInLink`.

Surprise: `AccountEventChannel.send` writes from a task of its own, so `close()` waits 50 ms after the close frame before disposing the channel.

## View models (`AgentsListModel.swift`, `AgentStartModel.swift`, `AgentScreenModel.swift`)

- `protocol AgentsApi` (ApiService conforms): the reads and queue calls the screens make; tests fake it.
- `AgentsListModel(api:signal:ownerAccountName:hasDesktopNode:debounce:)`: `start()`/`stop()` (subscribes to `AgentListSignal`, 0.6 s debounce, no timer; a ring during a ring-driven load waits for it and starts one more, rather than cancelling it), `refresh()`, `reload()`, `enabled`, `groups`, `entries: [AgentEntry]` (running first), `desktopRunning`, counts, `sealedCount`, `listState`, `isPendingFirstLoad`, `isFailed`, `loadError`, `retry(repo, session) -> String?`, `ownerAccount(for:)`. A repository whose sealed-session read (`agents/list`) or queue read (`agent-sessions`, then `agent-bots`) answers 403 or 404 is not read again for `refusalTtl` (10 minutes) per account (`isRefusal`). Every `agents` ring re-reads every repository, and repositories the account can see but not run agents on (another node's `vm1/forkmesh`, say) cost a refused request on each ring; the 2026-10-01 performance report logged them as errors. The refusals live in an `AgentReadRefusals` passed to `init(refusals:)` (default `nil`, which makes a fresh store inside the main-actor init; a default argument of `AgentReadRefusals()` is evaluated outside the main actor and does not compile with the Linux Swift toolchain). The app passes `AgentReadRefusals.shared` from the Agents screen and the watch bridge, so a refusal holds across both models and across a recreated screen model. A later report still showed each refused read twice in five minutes. The ledger read for runs with a branch is `publishedPullHeads` (`AgentsApi`; the default forwards to `publishedPulls`), because the list only matches number, head and status. A ring-driven load keeps the pulls it read before unless a branched run is new or changed status, branch, pull number or merged flag, or the read is `pullsRefreshInterval` (5 minutes) old and some run's pull is not merged or closed; `refresh()` always reads. `init(now:)` sets the clock for that age. `LaunchedAgentSessionModel.refresh` (the queued run screen) reads `publishedPullHeads` as well; it only builds `chain`. `AgentsLogic`: `desktopDetail`, `launchedDetail`, `taskChain`, `retryDetail`, `statusTone`/`chipTone` (`AgentBadgeTone`).
  A load writes `groups`, `loadError` and `hasLoaded` only when they change, because every write to an `@Observable` property redraws the Agents tab; `isPendingFirstLoad` and `isFailed` read `groups` first for the same reason.
  A failed repository listing keeps the groups already shown (with `loadError` set); a repository whose sealed or queue read fails keeps that source's previous rows. Queued runs the relay's answer left out stay when they are finished and not newer than its `finishedBefore` (`AgentSessionList.keeping`, in FMApi; the same rule as the desktop's `cloudAgentBeyondListedWindow`), and `AgentControlCenter` applies it to the All agents cloud rows, which also survive a failed read. The list rings on both the `agents` and `cloud-agents` account topics.
- `AgentTranscriptModel` (desktop session + transcript), `LaunchedAgentSessionModel` (queued run: `refresh`, `send` follow-up, `retry`, `cancel`, `chain`, `followUpClosedText`).
- `AgentStartModel(api:records:signedIn:credentialStore:localModels:initialPrompt:)`: `loadTargets()` (mesh repositories and on-device mirrors folded; mirrors first), `targetsNeedLoading`, `selectTarget`, `plan`, `mode`, `nodeChoice`, `canStart`, `submitLabel`, `runsOnText`, `signInProviders`, `startOnNode()`, `deviceStart() -> AgentDeviceStart?` (`.open(record, prompt:)` / `.clone(owner:name:prompt:)`), `deviceCredentials`, `deviceRunsOnGemma`, `deviceModelLabel`, `gemmaReadiness`.
- `AgentScreenModel(repoName:workspace:gitWorkspace:record:pullRequests:sessions:store:author:localModels:agentFactory:initialPrompt:initialImages:initialCredentials:autoStart:desktop:)`: `load() -> Bool` (true = auto-send now; `initialCredentials`, when given, wins over the stored provider), `send() -> AgentSendOutcome` (`.needsSetup` opens setup), `stop`, `events`, `handle`, `modelChipLabel`, `gemmaReadiness`, `prCandidate`, `preparePullRequest()`, `submitPullRequest(_:title:)`, `insertReference(_:into:at:)`, `attach`.
- `AgentSetupModel(store:initial:localModels:)`: `selectKind`, `selectModel` (Gemma: also selects in the manager), `models`, `readiness`, `save() -> AgentCredentials?`, `remove()`, `problem`. `AttachFileModel(root:)`: `scan()`, `matches`, `query`, 2000-file cap, `.git` skipped.

## App target (ForkMesh/Features/Agents)

`enum AgentsFeature { @MainActor static func register() }` (no arguments; call after `LocalAIFeature.register`): registers `AgentSessionService` (loaded) and `AgentCredentialStore` from `KeyValueStore`/`SecureStore`/`HTTPClient`/`LocalModelManager` in `Services`. Helpers: `sessions()`, `credentialStore()`, `author()`, `pullRequests()` (needs `PublishService` + `InboxService`), `localRecords()` (needs `LocalRepoStore`), `localModels()`.
Entry points: `AgentsScreen()`, `AgentStartCard()` (takes the shell's `AgentPromptHandoff` prompt, calls `started()`), `AgentScreen(repoName:workspace:record:initialPrompt:autoStart:)`; also `AgentLoginTerminalScreen(node:provider:)`, `AgentSetupSheet(initial:onSaved:)`.

## Quick launch from the composer (`QuickAgentLaunch.swift`, `AgentModelCatalog.swift`)

`QuickAgentLauncher(form:prefs:account:)` is the policy behind the shell composer's Send: it wraps an `AgentStartModel` and starts the run at once, with no second form. The repository is the current context (a repository route, or the on-device repository page on screen), else the last one used, else the account's own most recently updated one, else one already on this device, else the most recently updated one; where it runs is the last choice for that repository, else the first online node that runs the model's provider, else this device, else the preferred node. `send(prompt:images:)` returns a `QuickLaunchOutcome`: `.queued` or `.queuedWithoutSession` (the relay took it), `.device` (open the local repository's agent), `.desktop` (the linked desktop started it), `.needsFix(QuickLaunchFix, message:)` (sign in, choose a repository, sign a CLI in on the node, set up this device, mirror here first; the text stays), or `.failed`. The choices are remembered under `agent/quick/last-repo` and `agent/quick/target/<repo>` (`QuickLaunchMemory`).

The shell prepares the launcher once at launch, often before the relay or the network is up. `AgentStartModel.loadTargets()` swallows a failed `repositories()` read (an on-device mirror can still be worked on), and until 2026-10-13 that empty list was kept for the rest of the session: the composer said "Choose a repository", the picker's Repository section was empty and its "Runs on" said "Nothing can run this repository yet", while the Repos tab, which reads the same catalog on its own, listed every repository. `targetsNeedLoading` is now true until a mesh read has succeeded and returned at least one repository, and `prepare()`, `send(...)` and the picker sheet (which calls `prepare()` when it opens) load the list again while it is. With no repository chosen, the picker's "Runs on" says "Choose a repository below." instead, and an empty Repository section says whether it is loading or could not load.

The picker's "Runs on" list also offers the account's desktop, `.desktop(name)`, whenever `desktop` (a `QuickLaunchDesktop`; the app sets `HubQuickLaunchDesktop` over `HostsFeature.hub`) names one and the repository is on the mesh. Sending there calls the desktop's `agents.start` over the control channel (LAN, else the relay) with the chip's provider and model id, instead of queueing on the relay, so it needs no agent-capable catalog record. The desktop matches the repository by owner and name among its own checkouts: the launcher tries the public owner, then the node owner, when the desktop answers `no_checkout`. Photos are refused for this destination because `agents.start` takes text only. The choice is stored as `desktop:<name>`. Android mirrors this (`QuickLaunchDestination.Desktop`, `QuickLaunchBackend.startOnDesktop`). For this destination the launcher also carries `runOptions` (`QuickLaunchRunOptions`: YOLO, remembered under `agent/desktop/yolo`, and No worktree, for the next run only), sent as `agents.start`'s `vibe` and `worktree`; see `docs/native/DESKTOP_AGENTS.md`.

`AgentModelCatalog` lists the models a queued run can ask for (Opus 5.5 first, then Opus 5, Sonnet 5, Haiku 4.5, Fable 5 and the GPT-5.6/Codex models), narrowed to the relay's `models` offer; `NodeModelPreference` keeps the chip's pick under `agent/node-model-alias`. `AgentStartModel` gained `attachments`, `nodeModel`, `nodeModelChoices`, `lastQueued` and `hasContent`; a queued launch now sends the chip's alias and provider instead of `auto` with no model. `AgentsApi` has a `launchAgentSession(..., attachments:)` requirement whose default throws when photos are passed, so fakes without photo support keep compiling.

The chip also offers on-device Gemma (`AgentModelCatalog.onDeviceGemma`, provider `on-device`, alias `on-device-gemma`; `AgentModelChoice.runsOnDevice`) last, when this build can run agents on the phone and the MLX runtime is available here (`AgentStartModel.offersOnDeviceGemma`). It is a different model from the "Gemma 4 E4B…" entries, which queue a run on a node's Ollama. Picking it moves the run to `.device` (the "Runs on" list shows only this iPhone, with the Local AI model's name) and switching back re-resolves the destination. Send checks `onDeviceGemmaReadiness`: off or not downloaded returns `.needsFix(.setUpGemma)`, whose button opens Settings › On-device AI; a device MLX cannot run on returns `.failed`. A ready Gemma returns `.device(record:prompt:images:credentials:)` with `AgentCredentials.onDevice(model:)` for the Local AI selection; the shell hands them to the agent screen through `AgentPromptHandoff.offer(images:credentials:)`, and `AgentScreenModel(initialCredentials:)` uses them instead of the stored provider, so a saved API key is left as it was. A repository that must be mirrored to the phone first still goes through the full form, which runs with the stored provider. Added 2026-10-04 so the local Gemma can be picked from the mobile model dropdown.

`PromptPhotoRules` (`PromptAttachments.swift`) holds the photo rules the app's `PromptPhotoPreparer` applies: the 2048 px target size, the JPEG metadata strip and the size limits. `AnthropicProvider` and `OpenAiProvider` have `appendUser(_:_:images:)`, which writes the photos as image blocks; `GemmaAgentProvider` keeps the text-only default.

`PromptMarkupRules` (`PromptMarkup.swift`) is the geometry of the app's PencilKit markup editor: the aspect fit of the photo in the editor, the flattened image's pixel size (the same 2048 px cap) and the ink scale from canvas points to those pixels. See [docs/native/CAPTURE_MARKUP.md](../../../docs/native/CAPTURE_MARKUP.md).

## Pasting images into the composer (`PromptPaste.swift`, app `PromptPasteboard.swift`)

Every `PromptComposer` that takes photos (the shell's new-agent composer, the session
follow-up, the on-device agent) also takes pasted images, from the long-press Paste menu and
Cmd-V. `ComposerUITextView` offers Paste whenever the pasteboard holds an image, RTF(D) or HTML;
`PromptPasteboard.read()` collects each item's representations and, for rich text, the
attachments of the RTF(D) document; HTML images count only as `data:image/...;base64` URIs (no
remote fetch). `PromptPasteRules.paste` makes one image per image item (png, jpeg, heic, heif,
tiff, gif, webp, bmp — by magic bytes, not the declared type) and keeps the typed text of rich-text
items. `PromptPasteRules.admit` caps them at the photo limit and says how many were left out. The
images then go through the same `PromptPhotoPreparer` as a picked photo: 2048 px, JPEG q80, no
metadata, 5 MB each.

The chat fields reuse `ComposerUITextView` and `PromptPasteboard` through `ChatTextField`; see
FMChat.md, "Pasted images and the + menu".

## Runner merge: update from base, linked task (2026-10-04)

`RunnerMergeModel.update()` merges the run's base into its branch (`RunnerAgentsApi.updateRunnerAgent`);
`updated`, `updateTitle` ("Update from <base>") and `successLine` report it, and a conflict sets
`conflicted` so the sheet offers "Fix with agent". After a successful `merge()` the model reads the
session, finds the linked task (`RunnerAgentText.taskId`) and completes it with the merge note
(`RunnerAgentsApi.completeTask`), best-effort: a failure only sets `taskNote`, shown under the landed
report with the cleanup notice (`landedNotice`). `repo` and `mergeOid` feed the landed report's
Actions button, which opens `RepoActionsScreen(repo:commit:)` filtered to the merge commit
(`RepoActionsModel.commitFilter`, "Show all runs" clears it). The run screen offers the update while
the run is idle, has a branch, is not merged and the relay says `canMerge`.

## Not verified

The SwiftUI views were only parsed (no Mac). Gemma 4 as an agent is tested through `FakeLocalChatEngine` (tool-call round trip included), not the MLX engine; whether a 2B/4B model follows the tool protocol well enough is untested. The terminal grid's cell measuring and hardware-keyboard input (not ported) are untested on a device.
