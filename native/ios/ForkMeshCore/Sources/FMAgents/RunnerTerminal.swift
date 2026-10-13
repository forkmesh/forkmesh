import FMApi
import FMBase
import FMNet
import Foundation
import Observation

@MainActor
@Observable
public final class RunnerTerminal: StreamedTerminal {
    nonisolated public static let pollSeconds: TimeInterval = 1
    nonisolated public static let resizeDelaySeconds: TimeInterval = 0.4

    public let runner: String
    public let provider: String
    public let accountLabel: String
    public private(set) var screen: TerminalScreen = TerminalScreen(40, 100)
    public private(set) var revision: Int = 0
    public private(set) var statusText: String = ""
    public private(set) var statusLevel: TerminalStatusLevel = .pending
    public private(set) var closed: Bool = false
    public private(set) var exitCode: Int?

    @ObservationIgnored public let session: RunnerLoginSession
    @ObservationIgnored private let api: ApiService
    @ObservationIgnored private let openId: String
    @ObservationIgnored private let sleeper: @Sendable (TimeInterval) async throws -> Void
    @ObservationIgnored private var cols: Int = 100
    @ObservationIgnored private var rows: Int = 40
    @ObservationIgnored private var sentCols: Int = 0
    @ObservationIgnored private var sentRows: Int = 0
    @ObservationIgnored private var started: Bool = false
    @ObservationIgnored private var loop: Task<Void, Never>?
    @ObservationIgnored private var resizeTask: Task<Void, Never>?
    @ObservationIgnored private var inputTask: Task<Void, Never>?
    @ObservationIgnored private var queued: [String] = []
    @ObservationIgnored private var sealed: Bool = false
    @ObservationIgnored private var inputError: String = ""
    @ObservationIgnored private var decoder: ChunkedUtf8Decoder = ChunkedUtf8Decoder()
    @ObservationIgnored private var notifyTask: Task<Void, Never>?

    public init(
        api: ApiService, runner: String, provider: String, accountLabel: String = "", openId: String = "",
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        }
    ) {
        self.api = api
        self.runner = runner.dartTrimmed.lowercased()
        self.provider = provider.dartTrimmed.lowercased()
        self.accountLabel = accountLabel
        self.openId = openId
        self.sleeper = sleep
        session = RunnerLoginSession(runner: runner, provider: provider, kind: "terminal")
        statusText = "Opening \(providerLabel) on \(self.runner)…"
    }

    public var providerLabel: String { RunnerSignIn.providerLong(provider) }

    public var open: Bool { session.open }

    public func start(cols: Int, rows: Int) {
        if started || closed { return }
        started = true
        self.cols = RunnerTerminal.clampCols(cols)
        self.rows = RunnerTerminal.clampRows(rows)
        screen = TerminalScreen(self.rows, self.cols)
        loop = Task { [weak self] in await self?.run() }
    }

    public func run() async {
        await openSession()
        while session.open && !closed && !Task.isCancelled {
            let more: Bool = await pollOnce()
            if !session.open || closed { break }
            if more { continue }
            do {
                try await sleeper(RunnerTerminal.pollSeconds)
            } catch {
                break
            }
        }
        updateStatus()
    }

    func openSession() async {
        guard api.hasSession else {
            session.markFailed("Sign in to your ForkMesh account on this phone first.")
            updateStatus()
            return
        }
        if RunnerSignIn.isLoginId(openId) {
            session.attach(openId)
            updateStatus()
            await sendResize()
            return
        }
        do {
            let body: Data = JSON.encodeData(.object([
                "provider": .string(provider),
                "kind": .string("terminal"),
                "cols": .int(Int64(cols)),
                "rows": .int(Int64(rows)),
            ]))
            let response: HTTPResponse = try await api.rawRequest(
                "POST", api.url(RunnerSignIn.startPath(runner)),
                headers: ["Accept": "application/json", "Content-Type": "application/json"], body: body)
            let result: RunnerLoginSession.Start = session.applyStartReply(status: response.status, body: response.jsonObject)
            if result == .started {
                sentCols = cols
                sentRows = rows
            } else if result == .attached {
                await sendResize()
            } else if response.status == 409 && response.jsonObject.str("error") == "terminal_unsupported" {
                session.markFailed("\(runner) needs an update before it can open a terminal.")
            }
        } catch {
            session.markFailed("The terminal could not be opened: \(dartErrorText(error))")
        }
        updateStatus()
    }

    func pollOnce() async -> Bool {
        let url: URL = api.url(RunnerSignIn.outputPath(runner, session.id), [("after", String(session.afterSeq))])
        do {
            let response: HTTPResponse = try await api.rawRequest("GET", url, headers: ["Accept": "application/json"])
            if [401, 403, 404].contains(response.status) {
                session.markFailed(
                    response.status == 404
                        ? "The terminal is no longer available."
                        : "This account may not use this runner's terminal.")
                updateStatus()
                return false
            }
            guard response.ok else { return false }
            let bytes: [UInt8] = session.applyOutputReply(response.jsonObject)
            if !bytes.isEmpty {
                screen.feed(decoder.take(Data(bytes)))
                changed()
            }
            exitCode = session.exitCode
            updateStatus()
            return session.more
        } catch {
            return false
        }
    }

    public func input(_ text: String) {
        if text.isEmpty || closed { return }
        if sealed || queued.isEmpty {
            queued.append(text)
            sealed = false
        } else {
            queued[queued.count - 1] += text
        }
        if inputTask == nil {
            inputTask = Task { [weak self] in await self?.flushInput() }
        }
    }

    public func paste(_ text: String) {
        if text.isEmpty { return }
        input(screen.bracketedPaste && text.utf16.count > 1 ? "\u{1b}[200~\(text)\u{1b}[201~" : text)
    }

    public func key(_ key: TerminalKey) {
        input(terminalKeyBytes(key, appCursorKeys: screen.appCursorKeys))
    }

    public func submit(_ text: String) {
        if !text.isEmpty {
            paste(text)
            sealed = true
        }
        key(.enter)
    }

    func flushInput() async {
        while !queued.isEmpty && !closed && !Task.isCancelled {
            if !session.open {
                if session.failed || !session.id.isEmpty && !RunnerSignIn.loginStateOpen(session.state) { break }
                try? await sleeper(0.2)
                continue
            }
            var head: String = queued[0]
            let chunk: String = RunnerTerminal.take(&head)
            if head.isEmpty {
                queued.removeFirst()
            } else {
                queued[0] = head
            }
            guard RunnerSignIn.terminalInputValid(chunk) else { continue }
            do {
                let body: Data = JSON.encodeData(.object(["data": .string(chunk)]))
                let response: HTTPResponse = try await api.rawRequest(
                    "POST", api.url(RunnerSignIn.inputPath(runner, session.id)),
                    headers: ["Accept": "application/json", "Content-Type": "application/json"], body: body)
                if response.ok {
                    inputError = ""
                } else {
                    let message: String = response.jsonObject.str("message").dartTrimmed
                    inputError = message.isEmpty ? "A key was not sent (HTTP \(response.status))." : message
                }
            } catch {
                inputError = "A key was not sent: \(dartErrorText(error))"
            }
            updateStatus()
        }
        queued = []
        sealed = false
        inputTask = nil
    }

    public func resize(cols: Int, rows: Int) {
        let cols: Int = RunnerTerminal.clampCols(cols)
        let rows: Int = RunnerTerminal.clampRows(rows)
        if cols == self.cols && rows == self.rows { return }
        self.cols = cols
        self.rows = rows
        if !started || closed { return }
        screen = screen.resized(rows, cols)
        changed()
        resizeTask?.cancel()
        resizeTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(RunnerTerminal.resizeDelaySeconds * 1_000_000_000))
            if Task.isCancelled { return }
            await self?.sendResize()
        }
    }

    func sendResize() async {
        guard session.open, cols != sentCols || rows != sentRows else { return }
        let wanted: (Int, Int) = (cols, rows)
        do {
            let body: Data = JSON.encodeData(.object(["cols": .int(Int64(wanted.0)), "rows": .int(Int64(wanted.1))]))
            let response: HTTPResponse = try await api.rawRequest(
                "POST", api.url(RunnerSignIn.resizePath(runner, session.id)),
                headers: ["Accept": "application/json", "Content-Type": "application/json"], body: body)
            if response.ok {
                sentCols = wanted.0
                sentRows = wanted.1
            }
        } catch {
            return
        }
    }

    public func close() async {
        if closed { return }
        closed = true
        loop?.cancel()
        resizeTask?.cancel()
        inputTask?.cancel()
        notifyTask?.cancel()
        guard session.needsCancelOnClose else { return }
        let response: HTTPResponse? = try? await api.rawRequest(
            "POST", api.url(RunnerSignIn.cancelPath(runner, session.id)),
            headers: ["Accept": "application/json", "Content-Type": "application/json"], body: Data("{}".utf8))
        if let response, response.ok {
            session.applyCancelReply(response.jsonObject)
        }
    }

    private func updateStatus() {
        let (text, level): (String, TerminalStatusLevel) = RunnerTerminal.status(
            session: session, runner: runner, provider: provider, accountLabel: accountLabel, inputError: inputError)
        if statusText == text && statusLevel == level { return }
        statusText = text
        statusLevel = level
    }

    private func changed() {
        if closed || notifyTask != nil { return }
        notifyTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 16_000_000)
            guard let self else { return }
            self.notifyTask = nil
            if self.closed { return }
            self.revision &+= 1
        }
    }

    nonisolated static func clampCols(_ cols: Int) -> Int { min(max(cols, 20), 300) }

    nonisolated static func clampRows(_ rows: Int) -> Int { min(max(rows, 5), 120) }

    nonisolated static func take(_ pending: inout String) -> String {
        var out: String = ""
        var bytes: Int = 0
        var index: String.Index = pending.startIndex
        while index < pending.endIndex {
            let next: String.Index = pending.index(after: index)
            let size: Int = pending[index..<next].utf8.count
            if bytes + size > RunnerSignIn.maxInputBytes { break }
            out += pending[index..<next]
            bytes += size
            index = next
        }
        pending = String(pending[index...])
        return out
    }

    nonisolated public static func status(
        session: RunnerLoginSession, runner: String, provider: String, accountLabel: String, inputError: String = ""
    ) -> (String, TerminalStatusLevel) {
        let label: String = RunnerSignIn.providerLong(provider)
        if session.failed { return (session.error, .error) }
        if !inputError.isEmpty && RunnerSignIn.loginStateOpen(session.state) { return (inputError, .error) }
        let account: String = accountLabel.isEmpty ? "" : " as \(accountLabel)"
        switch session.state {
        case "", "pending":
            return ("Waiting for \(runner) to open \(label)\(account). It picks this up on its next check-in.", .pending)
        case "running":
            return ("Live on \(runner)\(account). Keys reach it about once a second.", .ok)
        case "succeeded":
            return ("\(label) exited.", .ok)
        case "cancelled":
            return ("The terminal was closed.", .ok)
        default:
            if !session.message.isEmpty { return (session.message, .error) }
            if let code = session.exitCode { return ("\(label) exited with status \(code).", .error) }
            return ("The terminal ended (\(session.state)).", .error)
        }
    }
}
