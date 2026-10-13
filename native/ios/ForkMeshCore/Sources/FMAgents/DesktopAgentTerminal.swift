import FMBase
import FMChat
import FMControl
import Foundation
import Observation

@MainActor
@Observable
public final class DesktopAgentTerminal: StreamedTerminal {
    public let owner: String
    public let name: String
    public let provider: String
    public let session: Int
    public let mode: String
    public let account: String
    public private(set) var terminalId: String
    public private(set) var info: DesktopTerminalInfo?

    public private(set) var screen = TerminalScreen(24, 80)
    public private(set) var revision = 0
    public private(set) var statusText = ""
    public private(set) var statusLevel = TerminalStatusLevel.pending
    public private(set) var live = false
    public private(set) var exitCode: Int?
    public private(set) var closed = false
    public private(set) var ending = false
    public private(set) var errorCode = ""

    @ObservationIgnored private let channel: any DesktopTerminalChannel
    @ObservationIgnored private let inputFlush: TimeInterval
    @ObservationIgnored private let sleep: @Sendable (TimeInterval) async throws -> Void
    @ObservationIgnored private var cols = 80
    @ObservationIgnored private var rows = 24
    @ObservationIgnored private var started = false
    @ObservationIgnored private var detached = false
    @ObservationIgnored private var awaiting = false
    @ObservationIgnored private var relinkAfter = false
    @ObservationIgnored private var generation = 0
    @ObservationIgnored private var early: [(String, JSONObject)] = []
    @ObservationIgnored private var attachedLink: ObjectIdentifier?
    @ObservationIgnored private var tapId: UUID?
    @ObservationIgnored private var linkId: UUID?
    @ObservationIgnored private var retry: Task<Void, Never>?
    @ObservationIgnored private var retryDelay: TimeInterval = 1
    @ObservationIgnored private var inputTimer: Task<Void, Never>?
    @ObservationIgnored private var notifyTimer: Task<Void, Never>?
    @ObservationIgnored private var pendingInput = ""
    @ObservationIgnored private var outbox: [(String, JSONObject)] = []
    @ObservationIgnored private var draining = false
    @ObservationIgnored private var decoder = ChunkedUtf8Decoder()

    public static let transient: Set<String> = ["", "not_connected", "closed", "timeout", "relay_unavailable"]

    private init(
        channel: any DesktopTerminalChannel, terminal: String, owner: String, name: String, provider: String,
        session: Int = 0, mode: String = "", account: String = "", inputFlush: TimeInterval,
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void
    ) {
        self.channel = channel
        self.terminalId = terminal
        self.owner = owner
        self.name = name
        self.provider = provider
        self.session = session
        self.mode = mode
        self.account = account
        self.inputFlush = inputFlush
        self.sleep = sleep
        statusText = session > 0 ? "Opening the agent in a terminal on the desktop…"
            : terminal.isEmpty ? "Starting \(DesktopAgentTerminal.taskLabel(agentProviderLabel(provider), mode)) on the desktop…"
            : "Connecting to the terminal on the desktop…"
    }

    public convenience init(
        channel: any DesktopTerminalChannel, session: Int, owner: String = "", name: String = "",
        provider: String = "", inputFlush: TimeInterval = AgentTerminalConfig.inputFlush,
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = DesktopAgentTerminal.defaultSleep
    ) {
        self.init(
            channel: channel, terminal: "", owner: owner, name: name, provider: provider, session: session,
            inputFlush: inputFlush, sleep: sleep)
    }

    public convenience init(
        channel: any DesktopTerminalChannel, owner: String, name: String, provider: String, mode: String = "",
        account: String = "", inputFlush: TimeInterval = AgentTerminalConfig.inputFlush,
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = DesktopAgentTerminal.defaultSleep
    ) {
        self.init(
            channel: channel, terminal: "", owner: owner, name: name, provider: provider, mode: mode,
            account: account, inputFlush: inputFlush, sleep: sleep)
    }

    public convenience init(
        channel: any DesktopTerminalChannel, terminal: String,
        inputFlush: TimeInterval = AgentTerminalConfig.inputFlush,
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = DesktopAgentTerminal.defaultSleep
    ) {
        self.init(
            channel: channel, terminal: terminal, owner: "", name: "", provider: "", inputFlush: inputFlush,
            sleep: sleep)
    }

    public nonisolated static let defaultSleep: @Sendable (TimeInterval) async throws -> Void = { seconds in
        try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
    }

    public var providerLabel: String {
        if let label = info?.label, !label.isEmpty { return label }
        let id = info?.provider ?? provider
        return id.isEmpty ? "The agent" : agentProviderLabel(id)
    }

    public var isSignIn: Bool { mode == "login" || mode == "logout" }

    public var taskLabel: String { DesktopAgentTerminal.taskLabel(providerLabel, mode) }

    public nonisolated static func taskLabel(_ provider: String, _ mode: String) -> String {
        switch mode {
        case "login": return "\(provider) sign-in"
        case "logout": return "\(provider) sign-out"
        default: return provider
        }
    }

    public var repoLabel: String {
        if let info, !info.repositoryName.isEmpty { return info.repoLabel }
        return owner.isEmpty ? name : "\(owner)/\(name)"
    }

    public var isStarting: Bool { terminalId.isEmpty && !closed }

    public func start(cols: Int = 80, rows: Int = 24) {
        if started || closed { return }
        started = true
        self.cols = min(max(cols, 20), 500)
        self.rows = min(max(rows, 5), 200)
        screen = TerminalScreen(self.rows, self.cols)
        listen()
        Task { await self.open() }
    }

    public func detach() {
        if closed || detached { return }
        flushInput()
        detached = true
        live = false
        retry?.cancel()
        retry = nil
        inputTimer?.cancel()
        inputTimer = nil
        unlisten()
        if !terminalId.isEmpty {
            enqueue("terminal.detach", ["terminal": .string(terminalId)])
        }
    }

    public func reattach() {
        if !detached || closed { return }
        detached = false
        if !started { return }
        listen()
        if terminalId.isEmpty { return }
        Task { await self.open() }
    }

    public func end() async {
        if closed || ending { return }
        if terminalId.isEmpty {
            closed = true
            setStatus("Ended.", .ok)
            teardown()
            return
        }
        ending = true
        defer { ending = false }
        pendingInput = ""
        do {
            _ = try await channel.request("terminal.close", ["terminal": .string(terminalId)], timeout: 15)
        } catch let refusal as LocalControlException where refusal.code == "unknown_terminal" {
        } catch {
            setStatus(ControlErrorText.describe(error), .error)
            return
        }
        live = false
        setStatus("You ended \(providerLabel).", .ok)
        finish()
    }

    public func input(_ text: String) {
        if text.isEmpty || closed { return }
        pendingInput += text
        if pendingInput.utf8.count > 4096 {
            inputTimer?.cancel()
            flushInput()
            return
        }
        if inputTimer == nil {
            let delay = inputFlush
            inputTimer = Task { [weak self] in
                try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
                guard let self, !Task.isCancelled else { return }
                self.flushInput()
            }
        }
    }

    public func paste(_ text: String) {
        if text.isEmpty { return }
        input(screen.bracketedPaste && text.utf16.count > 1 ? "\u{1b}[200~\(text)\u{1b}[201~" : text)
    }

    public func key(_ key: TerminalKey) {
        input(terminalKeyBytes(key, appCursorKeys: screen.appCursorKeys))
    }

    public func resize(cols: Int, rows: Int) {
        let cols = min(max(cols, 20), 500)
        let rows = min(max(rows, 5), 200)
        if cols == self.cols && rows == self.rows { return }
        self.cols = cols
        self.rows = rows
        if !started || closed { return }
        screen = screen.resized(rows, cols)
        changed()
        if live && !terminalId.isEmpty { sendSize() }
    }

    private func sendSize() {
        enqueue(
            "terminal.resize",
            ["terminal": .string(terminalId), "cols": .int(Int64(cols)), "rows": .int(Int64(rows))])
    }

    private func listen() {
        if tapId == nil {
            tapId = channel.addEventTap { [weak self] name, data in self?.onEvent(name, data) }
        }
        if linkId == nil {
            linkId = channel.addLinkListener { [weak self] in self?.linkChanged() }
        }
    }

    private func unlisten() {
        if let tapId { channel.removeEventTap(tapId) }
        if let linkId { channel.removeLinkListener(linkId) }
        tapId = nil
        linkId = nil
    }

    private func open() async {
        if closed || detached { return }
        retry?.cancel()
        retry = nil
        generation += 1
        let attempt = generation
        let starting = terminalId.isEmpty
        awaiting = true
        relinkAfter = false
        early = []
        defer {
            if attempt == generation {
                awaiting = false
                early = []
            }
        }
        setStatus(
            !starting ? "Connecting to the terminal on the desktop…"
                : session > 0 ? "Opening the agent in a terminal on the desktop…"
                : "Starting \(taskLabel) on the desktop…",
            .pending)
        let answer: JSONObject
        do {
            if starting && session > 0 {
                answer = try await channel.request(
                    "terminal.resume",
                    [
                        "id": .int(Int64(session)), "cols": .int(Int64(cols)), "rows": .int(Int64(rows)),
                    ], timeout: 30)
            } else if starting {
                var params: JSONObject = [
                    "owner": .string(owner), "name": .string(name), "provider": .string(provider),
                    "cols": .int(Int64(cols)), "rows": .int(Int64(rows)),
                ]
                if !mode.isEmpty { params["mode"] = .string(mode) }
                if !account.isEmpty { params["account"] = .string(account) }
                answer = try await channel.request("terminal.start", params, timeout: 30)
            } else {
                answer = try await channel.request(
                    "terminal.attach",
                    ["terminal": .string(terminalId), "cols": .int(Int64(cols)), "rows": .int(Int64(rows))],
                    timeout: 20)
            }
        } catch {
            if attempt != generation || closed || detached { return }
            let code = (error as? LocalControlException)?.code ?? ""
            if starting {
                errorCode = code.isEmpty ? "terminal_failed" : code
                setStatus(ControlErrorText.describe(error), .error)
                return
            }
            if code == "unknown_terminal" {
                gone()
                return
            }
            if DesktopAgentTerminal.transient.contains(code) {
                setStatus("Reconnecting to the desktop…", .pending)
                scheduleRetry()
                return
            }
            errorCode = code
            setStatus(ControlErrorText.describe(error), .error)
            return
        }
        let parsed = DesktopTerminalInfo(json: answer)
        if starting && terminalId.isEmpty && !parsed.terminal.isEmpty { terminalId = parsed.terminal }
        if closed {
            if starting && !terminalId.isEmpty {
                let id = terminalId
                Task { [channel] in _ = try? await channel.request("terminal.close", ["terminal": .string(id)], timeout: 15) }
            }
            return
        }
        if detached {
            if !terminalId.isEmpty { enqueue("terminal.detach", ["terminal": .string(terminalId)]) }
            return
        }
        if attempt != generation { return }
        if terminalId.isEmpty {
            errorCode = "terminal_failed"
            setStatus(ControlErrorText.describe(code: "terminal_failed"), .error)
            return
        }
        info = parsed
        attachedLink = channel.linkIdentity
        retryDelay = 1
        errorCode = ""
        decoder = ChunkedUtf8Decoder()
        screen = TerminalScreen(rows, cols)
        feed(answer.str("replay"))
        live = true
        exitCode = nil
        setStatus(liveStatus, .ok)
        let queued = early
        early = []
        awaiting = false
        for (event, data) in queued { onEvent(event, data) }
        if closed { return }
        if parsed.cols > 0 && parsed.rows > 0 && (parsed.cols != cols || parsed.rows != rows) { sendSize() }
        flushInput()
        if relinkAfter {
            relinkAfter = false
            linkChanged()
        }
    }

    private var liveStatus: String {
        let repo = repoLabel
        let place = repo.isEmpty ? "on the desktop" : "in \(repo) on the desktop"
        return "\(taskLabel) is running \(place). What you type here goes to its terminal."
    }

    private func onEvent(_ name: String, _ data: JSONObject) {
        if name != "terminal.output" && name != "terminal.exit" { return }
        if closed || detached { return }
        if awaiting {
            early.append((name, data))
            return
        }
        if terminalId.isEmpty || data.str("terminal") != terminalId { return }
        if name == "terminal.exit" {
            exited(data.int("code", -1))
            return
        }
        if !live { return }
        if data.bool("reset") {
            decoder = ChunkedUtf8Decoder()
            screen = TerminalScreen(rows, cols)
            changed()
        }
        feed(data.str("data"))
    }

    private func feed(_ base64: String) {
        if base64.isEmpty { return }
        guard let bytes = try? B64.decode(base64) else { return }
        screen.feed(decoder.take(bytes))
        changed()
    }

    private func exited(_ code: Int) {
        live = false
        exitCode = code
        pendingInput = ""
        outbox.removeAll()
        let label = taskLabel
        if code == 0 {
            setStatus("\(label) ended.", .ok)
        } else if code < 0 {
            setStatus("\(label) was stopped.", .error)
        } else {
            setStatus("\(label) exited with status \(code).", .error)
        }
        finish()
    }

    private func gone() {
        live = false
        errorCode = "unknown_terminal"
        setStatus(ControlErrorText.describe(code: "unknown_terminal"), .error)
        finish()
    }

    private func finish() {
        if closed { return }
        closed = true
        teardown()
        revision &+= 1
    }

    private func teardown() {
        retry?.cancel()
        retry = nil
        inputTimer?.cancel()
        inputTimer = nil
        notifyTimer?.cancel()
        notifyTimer = nil
        unlisten()
    }

    private func linkChanged() {
        if closed || detached || !started || terminalId.isEmpty { return }
        if awaiting {
            relinkAfter = true
            return
        }
        if let now = channel.linkIdentity, now == attachedLink { return }
        live = false
        attachedLink = nil
        if channel.linkIdentity != nil {
            Task { await self.open() }
        } else {
            setStatus("Reconnecting to the desktop…", .pending)
            scheduleRetry()
        }
    }

    private func scheduleRetry() {
        if closed || detached || retry != nil { return }
        let delay = retryDelay
        retryDelay = min(retryDelay * 2, 15)
        let sleep = self.sleep
        retry = Task { [weak self] in
            do { try await sleep(delay) } catch { return }
            guard let self, !Task.isCancelled, !self.closed, !self.detached else { return }
            self.retry = nil
            await self.open()
        }
    }

    private func flushInput() {
        inputTimer = nil
        if pendingInput.isEmpty || !live || terminalId.isEmpty || closed { return }
        let bytes = Array(pendingInput.utf8)
        pendingInput = ""
        var at = 0
        while at < bytes.count {
            let end = min(bytes.count, at + AgentTerminalConfig.maxInputBytesPerFrame)
            enqueue("terminal.input", ["terminal": .string(terminalId), "data": .string(B64.encode(bytes[at..<end]))])
            at = end
        }
    }

    private func enqueue(_ method: String, _ params: JSONObject) {
        outbox.append((method, params))
        if draining { return }
        draining = true
        Task { await self.drain() }
    }

    private func drain() async {
        while !outbox.isEmpty {
            let (method, params) = outbox.removeFirst()
            do {
                _ = try await channel.request(method, params, timeout: 10)
            } catch {
                if method == "terminal.detach" { continue }
                outbox.removeAll { $0.0 == "terminal.input" }
                let code = (error as? LocalControlException)?.code ?? ""
                if code == "unknown_terminal" {
                    gone()
                } else if !closed && !detached && !DesktopAgentTerminal.transient.contains(code) {
                    setStatus(ControlErrorText.describe(error), .error)
                }
            }
        }
        draining = false
    }

    private func setStatus(_ text: String, _ level: TerminalStatusLevel) {
        if statusText == text && statusLevel == level { return }
        statusText = text
        statusLevel = level
    }

    private func changed() {
        if closed || notifyTimer != nil { return }
        notifyTimer = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 16_000_000)
            guard let self, !Task.isCancelled else { return }
            self.notifyTimer = nil
            if self.closed { return }
            self.revision &+= 1
        }
    }
}
