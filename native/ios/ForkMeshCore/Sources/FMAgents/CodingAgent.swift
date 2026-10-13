import FMApi
import FMBase
import FMNet
import Foundation

public enum AgentStopReason: String, Sendable {
    case completed, cancelled, turnLimit, error, refused
}

/// The run ended. Exactly one of these is emitted per run.
public struct AgentDone: Equatable, Sendable {
    public let reason: AgentStopReason
    public let turns: Int
    public let inputTokens: Int
    public let outputTokens: Int

    public init(reason: AgentStopReason, turns: Int, inputTokens: Int, outputTokens: Int) {
        self.reason = reason
        self.turns = turns
        self.inputTokens = inputTokens
        self.outputTokens = outputTokens
    }
}

/// What happened in a run, in the order it happened. The UI renders these;
/// the transcript is just the list of them.
public enum AgentEvent: Equatable, Sendable {
    /// The user's prompt, echoed so the transcript is self-contained.
    case userMessage(String)
    /// Assistant prose for one turn.
    case assistantMessage(String)
    /// The model asked for a tool.
    case toolCall(AgentToolCall)
    /// What the tool returned.
    case toolResult(AgentToolResult)
    /// Something the harness itself wants to say (limits, cancellation,
    /// retries).
    case notice(String, isError: Bool)
    /// The run ended.
    case done(AgentDone)

    public static func notice(_ text: String) -> AgentEvent { .notice(text, isError: false) }

    public var done: AgentDone? {
        if case .done(let done) = self { return done }
        return nil
    }

    public var noticeText: String? {
        if case .notice(let text, _) = self { return text }
        return nil
    }

    public var userText: String? {
        if case .userMessage(let text) = self { return text }
        return nil
    }

    public var assistantText: String? {
        if case .assistantMessage(let text) = self { return text }
        return nil
    }

    public var toolCall: AgentToolCall? {
        if case .toolCall(let call) = self { return call }
        return nil
    }

    public var toolResult: AgentToolResult? {
        if case .toolResult(let result) = self { return result }
        return nil
    }
}

/// A small coding agent that runs entirely on the phone.
///
/// The design target is "smallest thing that genuinely works": one loop, a
/// handful of real tools, and no features the core does not need (no
/// sub-agents, no plan mode, no MCP, no permission popups). What it cannot do
/// it says plainly rather than approximating.
///
/// The loop is the whole idea:
///
///   send history → model answers → if it asked for tools, run them,
///   append the results, send again → repeat until it stops asking.
///
/// Everything else here is bounds: a turn cap, tool-output truncation,
/// history trimming, cancellation, and one retry for transient transport
/// failures. The loop is the same whichever provider answers, the on-device
/// Gemma model included.
///
/// `run` hands each event to `onEvent` and waits for it to return before going
/// on, so a `cancel()` or `steer()` made while handling an event is seen
/// before the loop takes its next step.
@MainActor
public final class CodingAgent {
    public let provider: AgentProvider
    public let toolset: AgentToolset

    /// Hard stop on loop length. A stuck model burns tokens quietly; this
    /// makes it stop and say so.
    public let maxTurns: Int

    /// Oldest messages beyond this are dropped between turns. Crude next to
    /// real compaction, but it is honest about what it did and costs nothing.
    public let maxHistoryMessages: Int

    private let client: HTTPClient
    private let retryDelay: TimeInterval
    private let sleep: @Sendable (TimeInterval) async -> Void

    public private(set) var history: [JSONValue] = []
    private var queuedSteering: [String] = []
    private var queuedImages: [PromptAttachment] = []
    public private(set) var lastUserImages: [PromptAttachment] = []
    private var running = false
    private var cancelled = false

    public init(
        provider: AgentProvider,
        toolset: AgentToolset,
        maxTurns: Int = 24,
        maxHistoryMessages: Int = 60,
        client: HTTPClient? = nil,
        retryDelay: TimeInterval = 2,
        sleep: @escaping @Sendable (TimeInterval) async -> Void = { seconds in
            try? await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        }
    ) {
        self.provider = provider
        self.toolset = toolset
        self.maxTurns = maxTurns
        self.maxHistoryMessages = maxHistoryMessages
        self.client = client ?? URLSessionHTTPClient()
        self.retryDelay = retryDelay
        self.sleep = sleep
    }

    public var isRunning: Bool { running }

    /// Whether this session has context to continue from.
    public var hasHistory: Bool { !history.isEmpty }

    /// Queue a message to be delivered at the next turn boundary.
    ///
    /// Steering mid-run is the one interaction that matters most on a phone:
    /// you notice it heading the wrong way and correct it without losing the
    /// work so far. The message lands after the current tool batch finishes,
    /// which keeps the tool-call/result pairing intact.
    public func steer(_ text: String, images: [PromptAttachment] = []) {
        let trimmed = text.dartTrimmed
        if trimmed.isEmpty && images.isEmpty { return }
        queuedSteering.append(trimmed.isEmpty ? "See the attached photos." : trimmed)
        queuedImages.append(contentsOf: images)
    }

    /// Ask the run to stop at the next safe point.
    public func cancel() { cancelled = true }

    /// Run one request to completion, handing events to `onEvent` as they
    /// happen.
    public func run(_ prompt: String, images: [PromptAttachment] = [], onEvent: (AgentEvent) -> Void) async {
        if running {
            onEvent(.notice("This session is already running. Send a follow-up to steer it.", isError: true))
            return
        }
        running = true
        cancelled = false
        defer { running = false }
        var turns = 0
        var inputTokens = 0
        var outputTokens = 0
        func done(_ reason: AgentStopReason) -> AgentEvent {
            .done(AgentDone(reason: reason, turns: turns, inputTokens: inputTokens, outputTokens: outputTokens))
        }

        provider.appendUser(&history, prompt, images: images)
        lastUserImages = images
        onEvent(.userMessage(prompt))

        while true {
            if cancelled {
                onEvent(.notice("Stopped. The work so far is kept."))
                onEvent(done(.cancelled))
                return
            }
            if turns >= maxTurns {
                onEvent(.notice(
                    "Stopped after \(maxTurns) turns without finishing. Send a follow-up to continue from here.",
                    isError: true
                ))
                onEvent(done(.turnLimit))
                return
            }

            trimHistory()
            turns += 1

            let turn: AgentTurn
            do {
                turn = try await sendWithOneRetry()
            } catch let error as AgentProviderException {
                onEvent(.notice(error.message, isError: true))
                onEvent(done(.error))
                return
            } catch {
                onEvent(.notice("Could not reach the model: \(error)", isError: true))
                onEvent(done(.error))
                return
            }

            inputTokens += turn.inputTokens
            outputTokens += turn.outputTokens
            provider.appendAssistant(&history, turn)

            // A refusal is an HTTP 200 with no answer in it. Check before
            // treating the turn as a result, or the run silently continues on
            // nothing.
            if let refusal = turn.refusal {
                onEvent(.notice(refusal, isError: true))
                onEvent(done(.refused))
                return
            }

            let text = turn.text.dartTrimmed
            if !text.isEmpty { onEvent(.assistantMessage(text)) }

            if !turn.wantsTools {
                // The model is done. Deliver any steering as a fresh request
                // rather than ending, so "also update the README" continues the
                // session.
                if let queued = takeSteering() {
                    lastUserImages = takeSteeringImages()
                    provider.appendUser(&history, queued, images: lastUserImages)
                    onEvent(.userMessage(queued))
                    continue
                }
                onEvent(done(.completed))
                return
            }

            var results: [AgentToolResult] = []
            for call in turn.toolCalls {
                onEvent(.toolCall(call))
                // Every call gets a result, cancelled or not: a tool_use with
                // no tool_result is a malformed conversation the next request
                // would reject, and a call rendered with no result leaves a
                // spinner in the transcript that never resolves.
                let result: AgentToolResult
                if cancelled {
                    result = AgentToolResult(
                        callId: call.id,
                        name: call.name,
                        output: "Cancelled by the user before this ran.",
                        isError: true
                    )
                } else {
                    result = await toolset.invoke(call.id, call.name, call.arguments)
                }
                results.append(result)
                onEvent(.toolResult(result))
            }
            provider.appendToolResults(&history, results)

            if let queued = takeSteering() {
                lastUserImages = takeSteeringImages()
                provider.appendUser(&history, queued, images: lastUserImages)
                onEvent(.userMessage(queued))
            }
        }
    }

    /// Collects a whole run (tests, previews).
    public func runToEnd(_ prompt: String) async -> [AgentEvent] {
        var events: [AgentEvent] = []
        await run(prompt) { events.append($0) }
        return events
    }

    /// One retry, for transport blips and rate limits only. A 400 is a bug in
    /// the request and retrying it just burns another round trip.
    private func sendWithOneRetry() async throws -> AgentTurn {
        do {
            return try await sendOnce()
        } catch let error as AgentProviderException {
            if !error.isRetryable { throw error }
            await sleep(retryDelay)
            return try await sendOnce()
        }
    }

    private func sendOnce() async throws -> AgentTurn {
        do {
            return try await provider.send(
                systemPrompt: systemPrompt,
                history: history,
                tools: toolset.tools,
                client: client
            )
        } catch let error as AgentProviderException {
            throw error
        } catch {
            // Socket errors, DNS failures, timeouts: retryable by definition.
            throw AgentProviderException("Could not reach the model: \(error)")
        }
    }

    private func takeSteeringImages() -> [PromptAttachment] {
        let images = queuedImages
        queuedImages.removeAll()
        return images
    }

    private func takeSteering() -> String? {
        if queuedSteering.isEmpty { return nil }
        let joined = queuedSteering.joined(separator: "\n\n")
        queuedSteering.removeAll()
        return joined
    }

    /// Drop the oldest messages when the conversation outgrows its budget.
    ///
    /// The first message is kept because it states the task; without it a
    /// long run forgets what it was asked to do. Trimming happens only between
    /// turns, so a tool_use block is never separated from its tool_result.
    private func trimHistory() {
        if history.count <= maxHistoryMessages { return }
        let surplus = history.count - maxHistoryMessages
        history.removeSubrange(1..<(1 + surplus))
    }

    /// The system prompt.
    ///
    /// Short on purpose. It states the two constraints the model cannot infer
    /// (there is no shell, and edits land in a real repository) and otherwise
    /// leaves it alone. Over-instructing a current model costs quality.
    public var systemPrompt: String {
        "You are a coding agent running inside the ForkMesh mobile app, working in a real Git repository on the user's phone.\n\n"
            + "You have no shell. You cannot run builds, tests, linters, or any command — those tools do not exist here, and the platform forbids them. Work only through the tools you were given.\n\n"
            + "Because you cannot verify by running anything, prefer small, careful edits: read before you write, keep changes close to the surrounding code, and say what you could not check. If a task genuinely needs a build or a test run, do the part you can and say plainly which part needs a desktop.\n\n"
            + "Commit with git_commit when the change is complete."
    }
}
