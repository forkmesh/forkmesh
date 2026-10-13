import FMBase
import Foundation
import Observation

public enum GeniePhase: Equatable, Sendable {
    case idle
    case greeting
    case listening
    case thinking
    case working(String)
    case speaking
    case confirming(String)
    case handingOff
    case finished

    public var caption: String {
        switch self {
        case .idle: return "Tap the microphone to talk"
        case .greeting, .speaking: return ""
        case .listening: return "Listening…"
        case .thinking: return "Thinking…"
        case .working(let tool): return "\(tool)…"
        case .confirming: return "Say yes or no"
        case .handingOff: return "Asking an agent…"
        case .finished: return ""
        }
    }

    public var isSpeaking: Bool { self == .greeting || self == .speaking }
}

public enum GenieHeard: Equatable, Sendable {
    case words(String)
    case silence
    case failed(String)
}

@MainActor
public protocol GenieVoice: AnyObject {
    func say(_ text: String) async
    func listen(onWords: @escaping @MainActor (String) -> Void) async -> GenieHeard
    func listenAmbient(onWords: @escaping @MainActor (String) -> Void) async -> GenieHeard
    func stopListening()
    func stopSpeaking()
}

extension GenieVoice {
    public func listenAmbient(onWords: @escaping @MainActor (String) -> Void) async -> GenieHeard {
        await listen(onWords: onWords)
    }
}

public struct GenieOverheard: Equatable, Sendable {
    public let id: Int
    public let text: String
    public let spot: GenieSpot?
    public let final: Bool

    public init(id: Int, text: String, spot: GenieSpot?, final: Bool) {
        self.id = id
        self.text = text
        self.spot = spot
        self.final = final
    }

    public var segments: [GenieSegment] { GenieSpotter.segments(text, highlight: spot?.phrase) }
}

public struct GenieLine: Identifiable, Equatable, Sendable {
    public let id: Int
    public let fromUser: Bool
    public let text: String
}

@MainActor
@Observable
public final class GenieSession {
    public nonisolated static let greeting = "How can I help you?"
    public nonisolated static let handOffNotice = "I don't know how to do that yet, so I'm asking an agent to do it and teach me for next time."
    public nonisolated static let maxClarifications = 2

    @ObservationIgnored public let brain: GenieBrain
    @ObservationIgnored let voice: GenieVoice
    @ObservationIgnored let launch: @MainActor (GenieHandoff) async -> String
    @ObservationIgnored let settings: @MainActor () -> String
    @ObservationIgnored public var onFinish: @MainActor () -> Void = {}
    @ObservationIgnored public var onSpot: @MainActor (GenieSpot, Bool) -> Void = { _, _ in }
    @ObservationIgnored public var onReply: @MainActor (String) -> Void = { _ in }
    @ObservationIgnored public var canOverhear: @MainActor () -> Bool = { true }
    @ObservationIgnored public var greeter: @MainActor () -> GenieHint? = { nil }
    @ObservationIgnored private var ambientTask: Task<Void, Never>?
    @ObservationIgnored private var utterance = 0
    @ObservationIgnored private var spottedKey = ""
    @ObservationIgnored private var task: Task<Void, Never>?
    @ObservationIgnored private var confirmWaiter: CheckedContinuation<Bool, Never>?
    @ObservationIgnored private var nextLine = 0
    @ObservationIgnored private var clarifications = 0
    @ObservationIgnored private var run = 0

    public private(set) var phase: GeniePhase = .idle
    public private(set) var hearing = ""
    public private(set) var lines: [GenieLine] = []
    public private(set) var problem: String?
    public private(set) var alwaysListening = false
    public private(set) var overheard: GenieOverheard?
    public private(set) var offer: GenieHint?

    public init(
        brain: GenieBrain,
        voice: GenieVoice,
        settings: @escaping @MainActor () -> String = { "" },
        launch: @escaping @MainActor (GenieHandoff) async -> String
    ) {
        self.brain = brain
        self.voice = voice
        self.settings = settings
        self.launch = launch
    }

    public var isBusy: Bool {
        switch phase {
        case .idle, .finished: return false
        default: return true
        }
    }

    public var lastReply: GenieLine? { lines.last { !$0.fromUser } }

    public func open() {
        guard task == nil else { return }
        guard lines.isEmpty else {
            start { session in await session.listen(fresh: !session.brain.awaitingAnswer) }
            return
        }
        start { session in
            session.phase = .greeting
            let hint = session.greeter()
            session.offer = hint?.spot == nil ? nil : hint
            await session.speak(GenieHints.greeting(hint))
            await session.listen(fresh: true)
        }
    }

    public func perform(_ spot: GenieSpot, said: String? = nil) {
        offer = nil
        start { session in
            session.add(said ?? spot.title, fromUser: true)
            session.phase = .thinking
            session.clarifications = 0
            let outcome = await session.brain.perform(
                spot, confirm: session.confirmer, activity: session.activity)
            if Task.isCancelled { return }
            await session.respond(outcome)
        }
    }

    public func setAlwaysListening(_ on: Bool) {
        guard on != alwaysListening else { return }
        alwaysListening = on
        if on {
            problem = nil
            ambientTask?.cancel()
            ambientTask = Task { @MainActor [weak self] in
                while !Task.isCancelled {
                    guard let self, self.alwaysListening else { return }
                    await self.overhearOnce()
                }
            }
        } else {
            ambientTask?.cancel()
            ambientTask = nil
            if task == nil { voice.stopListening() }
            overheard = nil
        }
    }

    private func overhearOnce() async {
        guard task == nil, canOverhear() else {
            try? await Task.sleep(for: .milliseconds(700))
            return
        }
        let heard = await voice.listenAmbient { [weak self] words in self?.overhear(words, final: false) }
        if Task.isCancelled || !alwaysListening || task != nil { return }
        switch heard {
        case .words(let words) where !words.dartTrimmed.isEmpty:
            overhear(words, final: true)
        case .failed(let message):
            problem = message
            alwaysListening = false
            ambientTask = nil
        default:
            try? await Task.sleep(for: .milliseconds(250))
        }
    }

    func overhear(_ words: String, final: Bool) {
        let text = words.dartTrimmed
        guard !text.isEmpty, task == nil else { return }
        let spot = brain.toolbox.spot(text)
        overheard = GenieOverheard(id: utterance, text: text, spot: spot, final: final)
        if let spot { announce(spot, runs: spot.runsAlone) }
        guard final else { return }
        utterance += 1
        if let spot, spot.runsAlone {
            perform(spot, said: text)
        } else if spot == nil, GenieSpotter.calledByName(text) {
            let request = GenieSpotter.withoutWakeWord(text)
            if request.split(separator: " ").count < 2 {
                start { session in
                    await session.speak("Yes?")
                    if Task.isCancelled { return }
                    await session.listen(fresh: true)
                }
            } else {
                submit(request)
            }
        }
    }

    public func talk() {
        if phase == .listening {
            voice.stopListening()
            return
        }
        if case .confirming = phase { return }
        start { session in await session.listen(fresh: !session.brain.awaitingAnswer) }
    }

    public func submit(_ typed: String) {
        let text = typed.dartTrimmed
        guard !text.isEmpty else { return }
        if case .confirming = phase, let yes = GenieWords.isYes(text) {
            resolveConfirm(yes)
            return
        }
        start { session in await session.act(text, fresh: !session.brain.awaitingAnswer) }
    }

    public func confirm(_ allowed: Bool) {
        resolveConfirm(allowed)
    }

    public func close() {
        run += 1
        offer = nil
        task?.cancel()
        task = nil
        resolveConfirm(false)
        voice.stopListening()
        voice.stopSpeaking()
        phase = .finished
    }

    public func waitUntilIdle() async {
        await task?.value
    }

    private func start(_ work: @escaping @MainActor (GenieSession) async -> Void) {
        run += 1
        let token = run
        task?.cancel()
        resolveConfirm(false)
        voice.stopListening()
        voice.stopSpeaking()
        problem = nil
        task = Task { @MainActor [weak self] in
            guard let self else { return }
            await work(self)
            if token == self.run {
                self.task = nil
                if self.phase != .finished { self.phase = .idle }
            }
        }
    }

    private func announce(_ spot: GenieSpot, runs: Bool) {
        let key = "\(utterance):\(spot.tool):\(spot.arguments.sorted { $0.key < $1.key })"
        guard key != spottedKey else { return }
        spottedKey = key
        onSpot(spot, runs)
    }

    private func heardWhileListening(_ words: String) {
        hearing = words
        if let spot = brain.toolbox.spot(words) {
            announce(spot, runs: true)
        }
    }

    private func listen(fresh: Bool) async {
        phase = .listening
        hearing = ""
        utterance += 1
        let heard = await voice.listen { [weak self] words in self?.heardWhileListening(words) }
        if Task.isCancelled { return }
        hearing = ""
        switch heard {
        case .words(let words) where !words.dartTrimmed.isEmpty:
            await act(words, fresh: fresh)
        case .failed(let message):
            problem = message
            phase = .idle
        default:
            phase = .idle
        }
    }

    private var confirmer: @MainActor (String) async -> Bool {
        { [weak self] summary in await self?.askConfirmation(summary) ?? false }
    }

    private var activity: @MainActor (String) -> Void {
        { [weak self] tool in self?.phase = .working(tool) }
    }

    private func act(_ text: String, fresh: Bool) async {
        let offered = offer
        offer = nil
        if fresh, let spot = offered?.spot, let yes = GenieWords.isYes(text), GenieWords.words(text).count <= 4 {
            add(text, fromUser: true)
            phase = .thinking
            clarifications = 0
            let outcome: GenieOutcome = yes
                ? await brain.perform(spot, confirm: confirmer, activity: activity)
                : .reply("Okay.")
            if Task.isCancelled { return }
            await respond(outcome)
            return
        }
        add(text, fromUser: true)
        phase = .thinking
        if fresh { clarifications = 0 }
        let outcome = fresh
            ? await brain.begin(text, confirm: confirmer, activity: activity)
            : await brain.answer(text, confirm: confirmer, activity: activity)
        if Task.isCancelled { return }
        await respond(outcome)
    }

    private func respond(_ outcome: GenieOutcome) async {
        switch outcome {
        case .reply(let reply):
            if !reply.isEmpty {
                onReply(reply)
                await speak(reply)
            }
            if Task.isCancelled { return }
            if brain.toolbox.navigated {
                phase = .finished
                onFinish()
            } else {
                phase = .idle
            }
        case .clarify(let question):
            clarifications += 1
            if clarifications > GenieSession.maxClarifications {
                await handOff("Still unclear after \(GenieSession.maxClarifications) questions.")
                return
            }
            await speak(question)
            if Task.isCancelled { return }
            await listen(fresh: false)
        case .handOff(let reason):
            await handOff(reason)
        }
    }

    private func handOff(_ reason: String) async {
        phase = .handingOff
        await speak(GenieSession.handOffNotice)
        if Task.isCancelled { return }
        phase = .handingOff
        let message = await launch(brain.handoff(reason: reason, settings: settings()))
        if Task.isCancelled { return }
        await speak(message)
        if Task.isCancelled { return }
        phase = brain.toolbox.navigated ? .finished : .idle
        if phase == .finished { onFinish() }
    }

    private func askConfirmation(_ summary: String) async -> Bool {
        phase = .confirming(summary)
        await voice.say("\(summary). Shall I go ahead?")
        if Task.isCancelled { return false }
        let listener = Task { @MainActor [weak self] in
            guard let self else { return }
            let heard = await self.voice.listen { [weak self] words in self?.hearing = words }
            self.hearing = ""
            if case .words(let words) = heard, let yes = GenieWords.isYes(words) {
                self.resolveConfirm(yes)
            }
        }
        let allowed = await withCheckedContinuation { (continuation: CheckedContinuation<Bool, Never>) in
            confirmWaiter = continuation
        }
        listener.cancel()
        voice.stopListening()
        phase = .thinking
        return allowed
    }

    private func resolveConfirm(_ allowed: Bool) {
        let waiter = confirmWaiter
        confirmWaiter = nil
        waiter?.resume(returning: allowed)
    }

    private func speak(_ text: String) async {
        add(text, fromUser: false)
        if phase != .greeting { phase = .speaking }
        await voice.say(text)
    }

    private func add(_ text: String, fromUser: Bool) {
        nextLine += 1
        lines.append(GenieLine(id: nextLine, fromUser: fromUser, text: text))
    }
}
