# FMLocalAI

On-device models: Gemma 4 on the phone's GPU through MLX, which the app target runs
(`ForkMesh/Features/LocalAI/MLXGemmaEngine.swift`). This module is everything else and contains no
MLX: the catalog, device fit, the Hugging Face download, the engine protocol, the view models and
the mesh-node request handler. `import FMLocalAI` (deps FMBase, FMNet).

## Catalog and device fit (`LocalAIModel.swift`)

- `LocalAIModel`: `id` (stored under `localai/model`), `repo`, `revision` (a pinned commit),
  `displayName`, `summary`, `approxDownloadBytes`, `approxMemoryBytes`, `minimumPhysicalMemory`,
  `recommendedPhysicalMemory`, `contextLength`, `filePatterns` (`*.safetensors`, `*.json`,
  `*.jinja`).
- `LocalModelCatalog.gemma4E2B` (default; `mlx-community/gemma-4-e2b-it-4bit`, about 3.6 GB
  download, 2.6 GB in memory) and `.gemma4E4B` (`mlx-community/gemma-4-e4b-it-4bit`, about 5.2 GB,
  4.2 GB). `.recommendedModel(for:)` picks E4B at 11 GB of memory or more, else E2B.
- `DeviceProfile { physicalMemory, availableMemory?, isSimulator }` (the app passes
  `os_proc_available_memory()`); `fit(for:)` gives `.recommended`, `.tight`, `.tooLarge` or
  `.unsupported`.
- `LocalAIFormat`: display strings for bytes, memory, tokens per second, percentages.

The repositories hold the multimodal checkpoint, so downloads are larger than the resident text
model. E2B needs a 6 GB iPhone and is comfortable on 8 GB; E4B needs 8 GB. Without the
`com.apple.developer.kernel.increased-memory-limit` entitlement (which requires enabling
"Increased Memory Limit" for the App ID in the developer portal, then adding the key to both
`Config/ForkMesh-*.entitlements` and `INCREASED_MEMORY_LIMIT` in `tools/asc_bundle_ids.py`), large
prompts can exceed the app's memory limit. Mitigations in place:

- `MLXGemmaEngine` prefills 256 tokens per step instead of 512;
- it refuses to start a reply when `os_proc_available_memory()` is under 400 MiB;
- a memory warning cancels generation and clears MLX's buffer cache;
- `OnDeviceChatModel` clips tool results from earlier sends to `maxHistoryToolOutputChars`
  (1,500); results from the current send keep `maxToolOutputChars` (6,000);
- generation stops on `willResignActive` as well as `didEnterBackground`, because iOS ends an app
  whose GPU work is committed in the background.

## Storage (`LocalModelStore.swift`)

`LocalModelStore.defaultRoot()` is Application Support/LocalModels, excluded from backups. Layout
`<root>/<org>/<name>/`, partial files in `.partial/`, and `.forkmesh-snapshot.json` (repo, revision,
files with sizes) written last. `isComplete(model)` requires the manifest for this revision and
every file at its size. `availableCapacity()` reads "important usage" free space and falls back to
`systemFreeSize` when that reports 0.

## Download (`HuggingFaceSnapshot.swift`, `FileDownloading.swift`)

- Listing: `GET /api/models/<repo>/revision/<rev>?blobs=true`; files from
  `/<repo>/resolve/<rev>/<path>` (redirects to a CDN that honours Range). Only top-level files
  matching the patterns are selected.
- `HuggingFaceSnapshotDownloader`: checks free space first, skips complete files, resumes partial
  ones, moves each file into place only when complete, writes the manifest last. Cancelling the
  task stops it; partial files stay. Progress is throttled to every 4 MiB.
- `HTTPFileDownloader` streams over FMNet's `HTTPClient.stream` with `Range: bytes=<n>-`
  (206 appends, 200 restarts, 416 means complete). `FakeFileDownloader` scripts responses in
  tests.
- Errors: `LocalModelError` (`.http`, `.badListing`, `.emptySnapshot`, `.sizeMismatch`,
  `.insufficientStorage`, `.notDownloaded`, `.engineUnavailable`, `.disabled`, `.io`).

Files come at a pinned commit over TLS and are size-checked; no content checksum is verified
(`lfs.sha256` in the listing would allow adding one).

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

- `LocalChatEvent`: `.text`, `.toolCall`, `.finished(LocalGenerationStats)` (last, once; stop
  reason `.stop/.length/.cancelled`). Cancelling the consumer or dropping the stream stops
  generation.
- `LocalChatMessage` (system, user, assistant with tool calls, tool) converts to and from OpenAI
  Chat Completions JSON. `LocalToolSpec { name, description, parameters }` (JSON Schema),
  `LocalToolCall { id, name, arguments }`.
- `LocalGenerationParameters`: `.chat` (Gemma defaults 1.0/0.95/64, 1024 tokens), `.agent` (0.3,
  2048 tokens).
- `engine.complete(...)` collects one turn (`text`, `toolCalls`, `stats`, token counts, the
  assistant `message`).
- `LocalChatTemplate` builds the dictionary the Jinja chat template receives, so Linux tests its
  shape.
- `FakeLocalChatEngine` scripts events, failures and hangs for tests.

## View models

`LocalModelManager` (`@MainActor @Observable`):

- Prefs: `localai/model` (catalog id; unknown ids fall back to the device's recommendation) and
  `localai/enabled` (default false).
- State per model (`.notDownloaded`, `.downloading`, `.downloaded`, `.loading`, `.ready`,
  `.failed`), download progress, disk usage, resumable bytes, selection, loaded model, device,
  engine availability, fit, `isUsable` (switch on, runtime available, selected model downloaded).
- Actions: `setEnabled` (off unloads), `select`, `download`, `cancelDownload`, `delete`, `load`
  (serialised, unloads others), `unload`, `ensureReady() async throws` (the one way chat, agents
  and peers get a loaded model), `modelForPeers()`, `makeInferenceService(deviceName:)`,
  `describe(error)`.

`OnDeviceChatModel`: streaming chat entries, `send`, `stop` (keeps what streamed), `clear`,
generation and preparing state, live tokens per second, a status line. History is bounded and
starts at a user turn. With a toolbox (`OnDeviceChatToolbox`), each send appends the toolbox's
briefing and runs a tool loop of up to 4 rounds; calls the toolbox flags wait for
`resolveApproval(_:)`. The ForkMesh toolbox is described in [FMAssistant](FMAssistant.md).

## Chat entry (`OnDeviceChatAccess.swift`)

The "Chat with <model>" row is always first in Settings › On-device AI. Its state is a pure
function, `OnDeviceChatAccess.resolve(...)`, returning `title`, `isEnabled`, a `reason` and an
inline `action`:

| State | Enabled | Reason | Action |
| --- | --- | --- | --- |
| Runtime unavailable (Simulator, no MLX in the build) | no | "Not supported on this device. <runtime reason>" | none |
| Device fit `tooLarge` / `unsupported` | no | the fit message | none |
| Switch off | no | "On-device AI is off." (and whether the model is downloaded) | `.turnOn` |
| Downloading | no | progress | `.cancelDownload` |
| Download failed | no | the failure | download again or resume |
| Partly downloaded | no | bytes so far | `.resumeDownload` |
| Not downloaded | no | size | `.download` |
| Downloaded, load failed before | yes | the load failure | none |
| Downloaded | yes | none | opens `OnDeviceChatView` |

The Agents tab's tool row also opens `OnDeviceChatView` when the runtime is available. Tests:
`OnDeviceChatAccessTests` (main-actor test methods must be `async` on Linux XCTest).

## Mesh node (`LocalInferenceService.swift`)

`actor LocalInferenceService`: the phone as an inference node for paired peers, transport-free.
`resolveModel` is the owner's policy (`LocalModelManager.modelForPeers`: switch on, model
downloaded).

- `info()` (`ai.info`): `{ok, available, reason?, model, model_name, context_length, max_tokens,
  busy, device}`; loads nothing.
- `respond(to:)` (`ai.chat`) and `stream(_:)` (`ai.delta`, `ai.tool_call`, then one `ai.done` or
  `ai.error`).
- OpenAI Chat Completions shaped requests and replies; errors `bad_request`,
  `model_unavailable`, `busy`, `generation_failed`. One request at a time; `max_tokens` capped
  (2048 by default).

The local control channel carries these as event frames (see [FMChat](FMChat.md), "On-device AI
for paired desktops"), gated on an explicit user switch.

## Writing GemmaAgentProvider (FMAgents)

`AgentProvider` keeps provider-shaped history. For Gemma keep it OpenAI-shaped, like
`OpenAiProvider`, so the loop and transcripts do not change:

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

Map `LocalEngineError`/`LocalModelError` to `AgentProviderException` with no status code and not
retryable (a missing model will not appear on retry). A small on-device model has a small
effective context: keep history short (16–24 messages) and tool output truncated. Tests use
`FakeLocalChatEngine` and a manager over a temp `LocalModelStore` with a hand-written manifest
(`OnDeviceChatModelTests.readyManager`).

## App target (ForkMesh/Features/LocalAI)

- `MLXGemmaEngine`: `LocalChatEngine` over MLX's `LLMModelFactory` with a directory
  `ModelConfiguration` (`extraEOSTokens: ["<turn|>"]`, `toolCallFormat: .gemma4`), a hand-written
  swift-transformers tokenizer bridge, and `MLXLMCommon.generate`. `Memory.cacheLimit` 64 MiB;
  cache cleared on unload; unavailable on the Simulator; plus the memory and lifecycle safeguards
  above.
- `LocalAIFeature.register(store:http:)` registers `LocalModelManager`, `LocalChatEngine` and
  `LocalInferenceService` in `Services`.
- Views: `LocalAISettingsView` (with `OnDeviceChatEntryCard` first), `OnDeviceChatView`.

## Not verified

The MLX engine is type-checked against the pinned packages but has not run on a device: model
loading, the Gemma 4 chat template, tool-call parsing and memory behaviour need on-device testing.
`HTTPFileDownloader` is tested through `FakeHTTPClient`; real multi-gigabyte transfers and Range
across the CDN redirect are not.
