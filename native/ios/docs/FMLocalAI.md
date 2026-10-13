# FMLocalAI

On-device models: Gemma 4 on the phone's GPU through MLX, which the app target
runs (`ForkMesh/Features/LocalAI/MLXGemmaEngine.swift`). This module is
everything else, and has no MLX in it: the catalog, device fit, the Hugging
Face download, the engine protocol, the view models and the mesh-node request
handler. `import FMLocalAI` (deps FMBase, FMNet).

## Catalog and device fit (`LocalAIModel.swift`)

- `struct LocalAIModel`: `id` (stored under `localai/model`), `repo`, `revision` (a commit, not "main"), `displayName`, `summary`, `approxDownloadBytes`, `approxMemoryBytes` (resident weights), `minimumPhysicalMemory`, `recommendedPhysicalMemory`, `contextLength`, `filePatterns` (`*.safetensors`, `*.json`, `*.jinja`: MLXLMCommon's own download patterns).
- `LocalModelCatalog.gemma4E2B` (default; `mlx-community/gemma-4-e2b-it-4bit` @ `2387675…`, 3.58 GB download, about 2.6 GB in memory) and `.gemma4E4B` (`mlx-community/gemma-4-e4b-it-4bit` @ `475b908…`, 5.18 GB, about 4.2 GB). `.all`, `.defaultModel`, `.model(id:)`, `.recommendedModel(for: DeviceProfile)`.
- `struct DeviceProfile { physicalMemory, availableMemory?, isSimulator }`; `DeviceProfile.current(availableMemory:)` (the app passes `os_proc_available_memory()`); `fit(for:) -> LocalModelFit`: `.recommended`, `.tight(note:)`, `.tooLarge(reason:)`, `.unsupported(reason:)`; `canRun`, `message`.
- `LocalAIFormat.bytes/memory/tokensPerSecond/percent`: the strings the screens show.

Surprise: the repositories hold the multimodal checkpoint (vision and audio towers), so the download is bigger than what MLXLLM keeps in memory. The Gemma 4 text model is not small: its per-layer embeddings alone are 1.3 GB (E2B) at 4 bits. E2B needs a 6 GB iPhone and is comfortable on 8 GB; E4B needs 8 GB. The app does not carry the `com.apple.developer.kernel.increased-memory-limit` entitlement. It was added on 2026-10-01 and removed the same night: the App ID has no Increased Memory Limit capability, the App Store Connect API cannot switch it on (`POST /v1/bundleIdCapabilities` answers 409, not a valid `capabilityType`), and without it signing has no profile carrying the entitlement, so every Codemagic build failed. To bring it back, tick "Increased Memory Limit" for `com.forkmesh.forkmesh` in the developer portal first, then re-add the key to both `Config/ForkMesh-*.entitlements` and `INCREASED_MEMORY_LIMIT` to the app's list in `tools/asc_bundle_ids.py` (`ensure` prints a reminder for it instead of posting it). Until then E4B and E2B on a 6 GB iPhone can be killed for memory.

Crash of 2026-10-01 (1.4.0 (18), iPhone 15 Pro Max, 8 GB): with no entitlement iOS let the app use about 3.4 GB in all (the performance report showed 3,226 MB left at 182 MB used). E2B's 2.6 GB of weights plus the app left a few hundred MB for the KV cache and prefill. The on-device chat had just gained 20 tool specs and a briefing on every send, and tool output of up to 6,000 characters stayed in the history, so prompts reached tens of thousands of tokens. A long prefill crossed the limit and iOS ended the app (a jetsam kill, which leaves no crash report). The fixes:

- the entitlement, which raises the limit on 8 GB phones;
- `MLXGemmaEngine` prefills 256 tokens per step instead of MLX's 512, which halves the attention scratch of each step;
- it refuses to start a reply when `os_proc_available_memory()` is under 400 MiB, with a message instead of a kill;
- a memory warning cancels generation and clears MLX's buffer cache;
- `OnDeviceChatModel` clips tool results from earlier sends to `maxHistoryToolOutputChars` (1,500) when it sends them back. Results from the current send keep `maxToolOutputChars` (6,000).

Generation now also stops on `willResignActive`, not only on `didEnterBackground`. iOS ends an app whose GPU work is committed once it is in the background, and cancelling is cooperative, so the step already running needs that head start. Pulling down Notification Center therefore stops a reply too: what streamed so far stays.

## Storage (`LocalModelStore.swift`)

`LocalModelStore(root:)`; `LocalModelStore.defaultRoot()` = Application Support/LocalModels, excluded from backups. Layout `<root>/<org>/<name>/` (what MLX loads), partial files in `.partial/`, and `.forkmesh-snapshot.json` (`SnapshotManifest`: repo, revision, files with sizes) written last. `isComplete(model)` = manifest for this revision present and every listed file at its size. `directory(for:)`, `diskUsage(for:)`, `partialBytes(for:)`, `totalDiskUsage()`, `availableCapacity()` (iOS "important usage" free space), `delete(_:)`.

## Download (`HuggingFaceSnapshot.swift`, `FileDownloading.swift`)

- `HuggingFaceHub.listingURL(repo:revision:)` = `GET /api/models/<repo>/revision/<rev>?blobs=true` (`siblings[].rfilename`, `size` or `lfs.size`); `fileURL(...)` = `/<repo>/resolve/<rev>/<path>` (302 to the CDN, which honours Range). `parseListing`, `select(files, patterns:)` (top-level files only), `Glob.matches`.
- `HuggingFaceSnapshotDownloader(http:files:store:endpoint:progressGranularity:clock:)`: `listFiles(for:)`, `download(model, progress:) -> URL`. Skips files already in place at their size, resumes partial files, moves each file into place only when complete, writes the manifest last, removes `.partial/`. Checks free space first (`LocalModelError.insufficientStorage`). Cancel the task to stop; partial files stay. Progress (`SnapshotProgress`: completed/total bytes, current file, `fraction`) is throttled to every 4 MiB.
- `protocol FileDownloading`: `download(url, to: partialFile, expectedBytes:, progress:) -> Int64`. Production `HTTPFileDownloader(http:)` over FMNet's `HTTPClient.stream` (FMNet already streams: its timeout covers only the head) with `Range: bytes=<n>-`; 206 appends, 200 restarts, 416 means complete (or restarts once). `FakeFileDownloader` (`.serve`, `.failAfter`, `.hangAfter`, `.fail`; `calls` with start offsets).
- Errors: `LocalModelError` (`.http`, `.badListing`, `.emptySnapshot`, `.sizeMismatch`, `.insufficientStorage`, `.notDownloaded`, `.engineUnavailable`, `.disabled`, `.io`), each with a user-facing `description`.

No checksum: files come at a pinned commit over TLS and are size-checked. `lfs.sha256` is in the listing if someone wants to add a CryptoKit pass.

## Engine protocol (`LocalChatEngine.swift`, `FakeLocalChatEngine.swift`)

```swift
public protocol LocalChatEngine: AnyObject, Sendable {
    var availability: LocalEngineAvailability { get }          // .available / .unavailable(reason:)
    func load(modelDirectory: URL, model: LocalAIModel) async throws
    func unload() async
    func loadedModelId() async -> String?
    func generate(messages: [LocalChatMessage], tools: [LocalToolSpec],
                  parameters: LocalGenerationParameters) -> AsyncThrowingStream<LocalChatEvent, Error>
}
```

- `LocalChatEvent`: `.text(String)`, `.toolCall(LocalToolCall)`, `.finished(LocalGenerationStats)` (last, once: prompt/generated tokens, `tokensPerSecond`, prompt/generate seconds, `stopReason` `.stop/.length/.cancelled`). Cancelling the consuming task, or dropping the stream, stops generation.
- `LocalChatMessage { role (.system/.user/.assistant/.tool), content, toolCalls, toolCallId, name }` with `.system/.user/.assistant(_:toolCalls:)/.tool(_:callId:name:)`; `init(openAI: JSONObject) throws` and `openAIJSON` convert OpenAI Chat Completions JSON both ways (arguments string or object; `LocalChatFormatError`).
- `LocalToolSpec { name, description, parameters: JSONObject }` (JSON Schema, i.e. `AgentTool.schema`). `LocalToolCall { id, name, arguments: JSONObject }`, `argumentsJSON`, `makeId()`, `init(openAI:)`, `openAIJSON`.
- `LocalGenerationParameters { maxTokens, temperature, topP, topK, seed }`; `.chat` (Gemma's defaults 1.0/0.95/64, 1024 tokens), `.agent` (0.3, 2048 tokens).
- `engine.complete(messages:tools:parameters:) -> LocalChatTurn` collects one turn: `text`, `toolCalls`, `stats`, `wantsTools`, `inputTokens`, `outputTokens`, `message` (the assistant message to append).
- `LocalChatTemplate.messages/tools/dictionary/sendable`: the `[String: any Sendable]` the Jinja chat template receives (assistant `tool_calls[].function.arguments` as an object, `{role: "tool", tool_call_id, name}` results, tools as `{type: "function", function: {...}}`). Built here so Linux tests the shape; the app passes it to `UserInput(messages:tools:)` untouched.
- `LocalEngineError`: `.noModelLoaded`, `.unavailable`, `.loadFailed`, `.generationFailed`.
- `FakeLocalChatEngine(availability:)`: `enqueue([.event(...), .fail(...), .hang])`, `enqueueReply(chunks, promptTokens:, tokensPerSecond:)`, `failNextLoad`, `requests`, `loadedDirectories`, `unloadCount`. `generate` throws `.noModelLoaded` until `load` ran.

## View models

`LocalModelManager` (`@MainActor @Observable`), `init(engine:store:downloader:prefs:device:catalog:)`:
- Prefs: `localai/model` (String, catalog id; unknown ids fall back to the device's recommendation) and `localai/enabled` (Bool, default false).
- State: `catalog`, `states[id]` / `state(for:)` (`LocalModelState`: `.notDownloaded`, `.downloading(progress:bytes:)`, `.downloaded`, `.loading`, `.ready`, `.failed(message)`; `isDownloading`, `isOnDisk`), `downloadProgress[id]`, `diskUsage[id]`, `resumableBytes[id]`, `downloadedIds` / `isDownloaded(_:)` (complete on disk, even after a failed load), `availableDiskBytes`, `totalDiskUsage`, `isEnabled`, `selectedModelId`/`selectedModel`, `loadedModelId`/`loadedModel`, `device`, `engineAvailability`, `fit(for:)` (runtime availability folded in), `recommendedModel`, `canDownload(_:)`, `isUsable` (switch on, runtime available, selected model downloaded).
- Actions: `setEnabled(_:)` (off unloads), `select(_:)`, `updateDevice(_:)`, `refresh()`, `download(_:)`, `cancelDownload(_:)` (a cancel is not a failure; partial bytes stay), `waitForDownload(_:)`, `delete(_:) async`, `load(_:) async` (unloads any other first; serialised), `unload() async`, `ensureReady() async throws -> LocalAIModel` (the one way chat, agents and peers get a loaded model; throws `.disabled`, `.engineUnavailable`, `.notDownloaded`, `LocalEngineError.loadFailed`), `modelForPeers()`, `makeInferenceService(deviceName:)`, `static describe(error)`.

`OnDeviceChatModel(manager:systemPrompt:parameters:maxHistoryMessages:now:)` (`@MainActor @Observable`): `entries: [OnDeviceChatEntry]` (`id, role, text, isStreaming, isError`), `draft`, `canSend`, `send()`, `stop()` (keeps what streamed), `clear()`, `isGenerating`, `isPreparing` (loading before the first reply), `liveTokensPerSecond` (chunks/s while streaming), `lastStats`, `errorMessage`, `statusLine` ("Gemma 4 E2B · 38.2 tok/s" or why it cannot run), `waitForReply()`. Error notes stay out of the history sent back; history is bounded and starts at a user turn. With `toolbox:` (an `OnDeviceChatToolbox`) the default system prompt becomes `toolboxSystemPrompt`, the toolbox's `briefing()` is appended to it on each send, and each send runs a tool loop of up to `maxToolRounds` (4) rounds, tool output clipped to `maxToolOutputChars` (6,000); entries then also carry `toolCalls`, `toolCallId`, `toolName` and `email` (an `OnDeviceEmailDraft`; `isToolRequest`, `isToolResult`), `activeTool` names the running tool, and tool turns stay in the history. A call the toolbox's `approval(for:)` flags waits in `pendingApproval` until `resolveApproval(_:)`; `stop()` and `clear()` decline it. The ForkMesh toolbox and the reason for it: [FMAssistant](FMAssistant.md).

## Chat entry (`OnDeviceChatAccess.swift`)

The owner could not find the on-device chat: Settings › On-device AI only showed "Chat with …" once the switch was on, the runtime available and the selected model downloaded, and then at the bottom of the screen. The row is now always the first thing on the screen, and its state is a pure function so it is tested on Linux.

`OnDeviceChatAccess.resolve(model:switchOn:engine:fit:state:isDownloaded:resumableBytes:totalBytes:)` returns `title` ("Chat with Gemma 4 E2B", the selected model's display name), `isEnabled`, `reason` (one line saying what is missing) and `action` (the inline fix), plus `progress` while downloading. `LocalModelManager.chatAccess` feeds it from the manager and `performChatAccess(_:)` runs the action. Checks, in order:

| State | Enabled | Reason | Action |
| --- | --- | --- | --- |
| Runtime unavailable (Simulator, no MLX in the build) | no | "Not supported on this device. <runtime reason>" | none |
| Device fit `tooLarge` / `unsupported` | no | the fit message | none |
| Switch off | no | "On-device AI is off." (adds ", and <model> is not downloaded yet" when it is not) | `.turnOn` ("Turn on") |
| Downloading | no | "Downloading <model>: 42% · 1.5 GB of 3.6 GB." | `.cancelDownload`, with a progress bar |
| Download failed | no | the failure | download again, or resume when partial bytes exist |
| Partly downloaded | no | "<model> is partly downloaded: 1.2 GB of 3.6 GB." | `.resumeDownload` ("Resume download") |
| Not downloaded | no | "<model> is not downloaded yet (3.6 GB)." | `.download` ("Download · 3.6 GB") |
| Downloaded, load failed before | yes | the load failure | none |
| Downloaded | yes | none | none; the row opens `OnDeviceChatView` |

The switch comes before the download so one tap fixes the first missing thing; the reason still names the download when both are missing. Memory and runtime problems come first because turning the switch on cannot fix them. `isEnabled` equals `isUsable` except when the device fit is `tooLarge`, where a load would fail anyway. Downloads go through the manager's existing `download(_:)`, so progress, resume and the per-model card below stay in step. The runtime warning that used to sit under the switch is gone: the chat row now carries it.

The selected model needs no new default: with nothing stored under `localai/model` the manager picks the device's recommended Gemma 4 (E4B at 11 GB of memory or more, E2B otherwise) and falls back to `LocalModelCatalog.defaultModel` (Gemma 4 E2B) when nothing fits.

Outside Settings, the Agents tab's tool row (next to Watch and Saved permissions) has a chat icon that pushes `OnDeviceChatView` for the registered manager, shown only when the runtime is available on this device. That row already holds screens pushed onto the tab's navigation stack, so it needs no new route; the composer's model picker and the AI button were left alone because they choose a provider for an agent run, not a chat. The chat screen's own "Set up" note covers the not-ready case from there.

Tests: `OnDeviceChatAccessTests`. Its methods are `async`: a synchronous test method on a `@MainActor` XCTestCase aborts the Linux test runner with "Could not cast value of type ... @MainActor () throws -> ()".

## Mesh node (`LocalInferenceService.swift`)

`actor LocalInferenceService(engine:deviceName:maxTokensCap:maxMessages:resolveModel:)`: the phone as an agent node for paired peers, with no transport. `resolveModel` is the owner's policy (`LocalModelManager.modelForPeers`: switch on, model downloaded; loads it).

- `info() -> JSONObject` (`ai.info`): `{ok, available, reason?, model, model_name, context_length, max_tokens, busy, device}`; loads nothing.
- `respond(to: JSONObject) -> JSONObject` (`ai.chat`), `stream(_:) -> AsyncStream<JSONObject>` (`ai.delta {text}`, `ai.tool_call {tool_call}`, then one `ai.done {...reply}` or `ai.error {error, message}`; dropping the stream stops generation). `parse(_:)` validates.
- Request and reply are OpenAI Chat Completions shaped: `{messages, tools?, max_tokens?, temperature?, top_p?, top_k?}` -> `{ok: true, model, message: {role, content, tool_calls?}, finish_reason (stop/length/tool_calls/cancelled), usage: {prompt_tokens, completion_tokens}, tokens_per_second}`; errors `{ok: false, error: bad_request|model_unavailable|busy|generation_failed, message}`. One request at a time (`busy`); `max_tokens` capped (2048 by default).

Exposing it (a later agent): the local control channel (FMChat's `LocalControl/`, desktop `LocalControl.h`) already carries authenticated, encrypted request/response frames with method names (`agents.prompt`...) and event frames. Register `ai.info` and `ai.chat` there, answer a request with `respond(to:)` or forward `stream(_:)` items as events tagged with the request id, and cancel the consuming task when the peer disconnects. Gate it on an explicit user consent switch (the handler assumes whoever calls it is allowed), and remember iOS stops GPU work in the background: the engine cancels generation when the app backgrounds, so a peer sees `finish_reason: "cancelled"`.

## Writing GemmaAgentProvider (FMAgents)

`AgentProvider` keeps provider-shaped history. For Gemma keep it OpenAI-shaped, exactly like `OpenAiProvider`, so the loop and transcripts do not change:

```swift
final class GemmaAgentProvider: AgentProvider {
    let manager: LocalModelManager            // Services.require(LocalModelManager.self)
    var id: String { "on-device" }
    var model: String { manager.selectedModel.id }

    func send(systemPrompt: String, history: [JSONValue], tools: [AgentTool]) async throws -> AgentTurn {
        try await manager.ensureReady()        // throws when off / not downloaded: an AgentProviderException, not retryable
        var messages: [LocalChatMessage] = [.system(systemPrompt)]
        for item in history { messages.append(try LocalChatMessage(openAI: item.objectOrEmpty)) }
        let specs = tools.map { LocalToolSpec(name: $0.name, description: $0.description, parameters: $0.schema) }
        let turn = try await manager.engine.complete(messages: messages, tools: specs, parameters: .agent)
        return AgentTurn(
            text: turn.text,
            toolCalls: turn.toolCalls.map { AgentToolCall(id: $0.id, name: $0.name, arguments: $0.arguments) },
            raw: .object(turn.message.openAIJSON),
            inputTokens: turn.inputTokens, outputTokens: turn.outputTokens, refusal: nil)
    }
    func appendAssistant(_ history: inout [JSONValue], _ turn: AgentTurn) { history.append(turn.raw) }
    func appendToolResults(_ history: inout [JSONValue], _ results: [AgentToolResult]) {
        for r in results { history.append(.object(LocalChatMessage.tool(r.output, callId: r.callId, name: r.name).openAIJSON)) }
    }
    func appendUser(_ history: inout [JSONValue], _ text: String) { history.append(.object(LocalChatMessage.user(text).openAIJSON)) }
}
```

Adjust names to whatever the FMAgents port calls them. Notes: `send` takes no HTTP client (nothing goes over the network); map `LocalEngineError`/`LocalModelError` to `AgentProviderException` with `statusCode: nil` but NOT retryable (a missing model will not appear on retry); pass `http.Client`-free cancellation by cancelling the task. A 2B/4B model on a phone has a small effective context: keep `maxHistoryMessages` low (16-24) and tool outputs truncated (`AgentToolset.maxToolOutputChars` is 24 000 characters, several thousand tokens of prefill each turn). Tests: `FakeLocalChatEngine` + a manager over a temp `LocalModelStore` with a hand-written manifest (see `OnDeviceChatModelTests.readyManager`).

## App target (ForkMesh/Features/LocalAI)

- `MLXGemmaEngine`: `LocalChatEngine` over `LLMModelFactory.shared.loadContainer(from:using:configuration:)` with a directory `ModelConfiguration` (`extraEOSTokens: ["<turn|>"]`, `toolCallFormat: .gemma4`), a hand-written swift-transformers `TokenizerLoader`/`Tokenizer` bridge (what the MLXHuggingFace macros generate), `UserInput(messages:tools:)` + `MLXLMCommon.generate(input:parameters:context:tools:)`. Sets `Memory.cacheLimit` to 64 MiB and clears MLX's cache on unload. Unavailable on the Simulator. Cancels generation on `willResignActive` and `didEnterBackground`, cancels and clears the cache on a memory warning, prefills in 256-token steps, and fails a generation up front when under 400 MiB is free (see "Crash of 2026-10-01" above).
- `LocalAIFeature.register(store:http:)` / `makeServices(store:http:) -> LocalAIServices { engine, manager, inference }`, registers `LocalModelManager`, `LocalChatEngine` and `LocalInferenceService` in `Services`; `LocalAIFeature.manager()`, `currentDevice()`, `settingsRoot()`, `chatRoot()`.
- Views: `LocalAISettingsView()` (or `(manager:)`), with `OnDeviceChatEntryCard` as its first row; `OnDeviceChatView()` (or `(manager:)`), also pushed from the Agents tab's tool row.

## Not verified

Nothing here has run on an iPhone. The MLX engine was type-checked on Linux against the pinned packages (see the report), not compiled by Xcode; model loading, the Gemma 4 chat template through swift-jinja, tool-call parsing and memory behaviour are untested. `HTTPFileDownloader` is tested through `FakeHTTPClient` (one chunk per response); a real multi-gigabyte transfer through `URLSessionHTTPClient`, and Range across the CDN redirect, are not.
