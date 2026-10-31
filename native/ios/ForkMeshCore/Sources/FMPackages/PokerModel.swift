import FMApi
import FMBase
import Foundation
import Observation

public typealias PokerApiCall = @MainActor (_ method: String, _ suffix: String, _ body: JSONObject?) async throws -> JSONObject

public struct PokerAPI {
    public var call: PokerApiCall

    public init(call: @escaping PokerApiCall) {
        self.call = call
    }

    @MainActor
    public init(api: ApiService) {
        call = { method, suffix, body in try await api.pokerApi(method, suffix, body) }
    }
}

public struct PokerTableDraft: Equatable, Sendable {
    public static let seatChoices = Array(2...9)
    public static let turnChoices = [15, 30, 45, 60]

    public var name = ""
    public var stakes = "starter"
    public var seats = 6
    public var isPrivate = false
    public var turnSeconds = 30
    public var bots = 0
    public var buyIn = 0

    public init() {}

    public var trimmedName: String { String(name.trimmingCharacters(in: .whitespacesAndNewlines).prefix(40)) }

    public mutating func fit(_ stake: PokerStake?, wallet: Int) {
        seats = min(9, max(2, seats))
        bots = min(seats - 1, max(0, bots))
        if !PokerTableDraft.turnChoices.contains(turnSeconds) { turnSeconds = 30 }
        guard let stake else { return }
        guard buyIn > 0 else { return }
        if let range = PokerBuyIn.range(min: stake.minBuyIn, max: stake.maxBuyIn, wallet: wallet) {
            buyIn = min(range.upperBound, max(range.lowerBound, buyIn))
        } else {
            buyIn = 0
        }
    }

    public var body: JSONObject {
        [
            "name": .string(trimmedName), "stakes": .string(stakes), "seats": .int(Int64(seats)),
            "private": .bool(isPrivate), "turnSeconds": .int(Int64(turnSeconds)), "bots": .int(Int64(bots)),
            "buyIn": .int(Int64(max(0, buyIn))),
        ]
    }
}

public enum PokerBuyIn {
    public static func range(min low: Int, max high: Int, wallet: Int) -> ClosedRange<Int>? {
        let top = Swift.min(high, wallet)
        guard low > 0, top >= low else { return nil }
        return low...top
    }

    public static func range(_ table: PokerTable) -> ClosedRange<Int>? {
        range(min: table.minBuyIn, max: table.maxBuyIn, wallet: table.wallet)
    }

    public static func suggested(big: Int, range: ClosedRange<Int>) -> Int {
        Swift.min(range.upperBound, Swift.max(range.lowerBound, big * 100))
    }

    public static func rebuyRange(_ table: PokerTable) -> ClosedRange<Int>? {
        guard let me = table.me else { return nil }
        let room = table.maxBuyIn - me.stack
        let top = Swift.min(room, table.wallet)
        let low = Swift.max(1, Swift.min(table.big, top))
        guard top >= low, top > 0 else { return nil }
        return low...top
    }

    public static func step(_ range: ClosedRange<Int>) -> Int {
        let span = range.upperBound - range.lowerBound
        if span >= 20000 { return 500 }
        if span >= 4000 { return 100 }
        if span >= 800 { return 25 }
        return 5
    }
}

@MainActor
@Observable
public final class PokerModel {
    nonisolated public static let packageId = "poker"
    nonisolated public static let topic = "package:poker"
    nonisolated public static let title = "Poker"

    @ObservationIgnored public let api: PokerAPI
    @ObservationIgnored private let localNow: @MainActor () -> Int64
    @ObservationIgnored private let sleeper: @Sendable (Int64) async throws -> Void

    public private(set) var lobby: PokerLobby?
    public private(set) var table: PokerTable?
    public private(set) var tableId: String?
    public private(set) var loadingLobby = false
    public private(set) var loadingTable = false
    public private(set) var busy = false
    public private(set) var lobbyError = ""
    public private(set) var tableError = ""
    public private(set) var notInstalled = false
    public private(set) var animates = false
    public private(set) var eventBatch = 0
    public private(set) var lastEvents: [PokerEvent] = []
    public private(set) var serverClock = PokerServerClock()
    public var notice = ""
    public var raiseTo = 0

    @ObservationIgnored public var onEvents: (@MainActor ([PokerEvent], PokerTable) -> Void)?
    @ObservationIgnored public var onCue: (@MainActor (PokerCue) -> Void)?
    @ObservationIgnored public var onMyTurn: (@MainActor () -> Void)?

    @ObservationIgnored private var cursor = PokerEventCursor()
    @ObservationIgnored private var turnKey = ""
    @ObservationIgnored private var wakeTask: Task<Void, Never>?
    @ObservationIgnored private var wakeTarget: Int64 = 0
    @ObservationIgnored private var wakeRetries = 0
    @ObservationIgnored private var clockTask: Task<Void, Never>?
    @ObservationIgnored private var clockDeadline: Int64 = 0
    @ObservationIgnored private var claimTask: Task<Void, Never>?
    @ObservationIgnored private var claimTarget: Int64 = 0
    @ObservationIgnored private var claimFired: Int64 = 0
    @ObservationIgnored private var tableRefreshing = false
    @ObservationIgnored private var tableAgain = false
    @ObservationIgnored private var lobbyRefreshing = false
    @ObservationIgnored private var lobbyAgain = false

    public init(
        api: PokerAPI, localNow: (@MainActor () -> Int64)? = nil,
        sleeper: (@Sendable (Int64) async throws -> Void)? = nil
    ) {
        self.api = api
        self.localNow = localNow ?? { Int64(Date().timeIntervalSince1970 * 1000) }
        self.sleeper = sleeper ?? { (ms: Int64) in try await Task.sleep(nanoseconds: UInt64(max(0, ms)) * 1_000_000) }
    }

    public var actor: String { lobby?.actor ?? "" }
    public var wallet: PokerWallet { lobby?.wallet ?? PokerWallet() }
    public var stakes: [PokerStake] { lobby?.stakes ?? PokerStake.defaults }
    public var tableOpen: Bool { tableId != nil }
    public var serverNow: Int64 { serverClock.serverNow(localNow()) }
    public var claimRemainingMs: Int64 { max(0, wallet.nextClaimAt - serverNow) }
    public var claimDate: Date { Date(timeIntervalSince1970: Double(serverClock.localTime(wallet.nextClaimAt)) / 1000) }

    public func localDate(_ serverMs: Int64) -> Date {
        Date(timeIntervalSince1970: Double(serverClock.localTime(serverMs)) / 1000)
    }

    public func ring() {
        Task { [weak self] in
            guard let self else { return }
            if self.tableId != nil {
                await self.reloadTable()
            } else {
                await self.loadLobby()
            }
        }
    }

    public func catchUp() {
        Task { [weak self] in
            guard let self else { return }
            if self.tableId != nil { await self.reloadTable() }
            await self.loadLobby()
        }
    }

    public func loadLobby() async {
        if lobbyRefreshing {
            lobbyAgain = true
            return
        }
        lobbyRefreshing = true
        loadingLobby = lobby == nil
        defer {
            lobbyRefreshing = false
            loadingLobby = false
        }
        repeat {
            lobbyAgain = false
            do {
                let reply = try await api.call("GET", "", nil)
                guard let decoded = PokerJSON.decode(PokerLobby.self, reply) else {
                    lobbyError = PokerText.error("bad_reply")
                    continue
                }
                if decoded.now > 0 { serverClock.sync(serverNow: decoded.now, localNow: localNow()) }
                lobby = decoded
                lobbyError = ""
                notInstalled = false
                scheduleClaim()
            } catch {
                let code = Self.code(error)
                if Self.notInstalledCodes.contains(code) { notInstalled = true }
                lobbyError = PokerText.error(code)
            }
        } while lobbyAgain
    }

    public func openTable(_ id: String) async {
        guard PokerIds.isTableId(id) else {
            notice = PokerText.error("table_not_found")
            return
        }
        if tableId != id {
            cursor.reset()
            turnKey = ""
            table = nil
            raiseTo = 0
            cancelTimers()
        }
        tableId = id
        tableError = ""
        await reloadTable()
    }

    public func closeTable() {
        cancelTimers()
        tableId = nil
        table = nil
        tableError = ""
        cursor.reset()
        turnKey = ""
        Task { [weak self] in await self?.loadLobby() }
    }

    public func reloadTable() async {
        guard let id = tableId else { return }
        if tableRefreshing {
            tableAgain = true
            return
        }
        tableRefreshing = true
        loadingTable = table == nil
        defer {
            tableRefreshing = false
            loadingTable = false
        }
        repeat {
            tableAgain = false
            do {
                let reply = try await api.call("GET", "/tables/\(id)", nil)
                take(reply, for: id)
            } catch {
                failTable(error, for: id)
            }
        } while tableAgain && tableId == id
    }

    public func claim() async {
        guard !busy else { return }
        busy = true
        defer { busy = false }
        do {
            let reply = try await api.call("POST", "/claim", [:])
            if let wallet = PokerJSON.decode(PokerWallet.self, reply.obj("wallet")) {
                var lobby = self.lobby ?? PokerLobby()
                let gained = wallet.chips - lobby.wallet.chips
                lobby.wallet = wallet
                self.lobby = lobby
                notice = gained > 0 ? "\(PokerText.chips(gained)) free chips added." : "Free chips collected."
                onCue?(.chip)
                scheduleClaim()
            }
            await loadLobby()
        } catch {
            fail(error)
            await loadLobby()
        }
    }

    public func createTable(_ draft: PokerTableDraft) async -> Bool {
        guard !busy else { return false }
        busy = true
        defer { busy = false }
        var draft = draft
        draft.fit(lobby?.stake(draft.stakes), wallet: wallet.chips)
        do {
            let reply = try await api.call("POST", "/tables", draft.body)
            guard let table = PokerTable.from(reply.obj("table")) else {
                notice = PokerText.error("bad_reply")
                return false
            }
            cursor.reset()
            turnKey = ""
            cancelTimers()
            tableId = table.id
            tableError = ""
            apply(table)
            return true
        } catch {
            fail(error)
            return false
        }
    }

    public func lookUp(code: String) async -> String? {
        guard let normalized = PokerIds.normalizedCode(code) else {
            notice = PokerText.error("invalid_code")
            return nil
        }
        do {
            let reply = try await api.call("GET", "/codes/\(normalized)", nil)
            let id = reply.str("tableId")
            guard PokerIds.isTableId(id) else {
                notice = PokerText.error("table_not_found")
                return nil
            }
            return id
        } catch {
            fail(error)
            return nil
        }
    }

    public func join(code: String) async -> Bool {
        guard let id = await lookUp(code: code) else { return false }
        await openTable(id)
        return table != nil
    }

    public func sit(seat: Int, buyIn: Int) async -> Bool {
        await tableWrite("/sit", ["seat": .int(Int64(seat)), "buyIn": .int(Int64(buyIn))])
    }

    public func quickSit(_ id: String, buyIn: Int) async -> Bool {
        await openTable(id)
        guard let table, table.id == id else { return false }
        if table.seated { return true }
        guard let seat = table.openSeats.first else {
            notice = PokerText.error("table_full")
            return false
        }
        return await sit(seat: seat, buyIn: buyIn)
    }

    public func act(_ kind: PokerActionKind, amount: Int = 0) async {
        guard let table, table.myTurn else { return }
        var body: JSONObject = ["action": .string(kind.rawValue), "hand": .int(Int64(table.handNumber))]
        if kind == .bet || kind == .raise { body["amount"] = .int(Int64(amount)) }
        if await tableWrite("/act", body, quietCodes: ["hand_changed", "not_your_turn"]) {
            raiseTo = 0
        }
    }

    public func fold() async { await act(.fold) }

    public func checkOrCall() async {
        guard let legal = table?.legal else { return }
        await act(PokerActions.callKind(legal))
    }

    public func raise() async {
        guard let table, let legal = table.legal, legal.raise != nil else { return }
        let amount = PokerActions.clamp(raiseTo > 0 ? raiseTo : PokerActions.defaultRaise(table), legal)
        await act(PokerActions.raiseKind(table), amount: amount)
    }

    public func allIn() async { await act(.allin) }

    public func tick() async {
        guard let id = tableId else { return }
        do {
            let reply = try await api.call("POST", "/tables/\(id)/tick", [:])
            take(reply, for: id)
        } catch {
            failTable(error, for: id)
        }
    }

    public func setAway(_ away: Bool) async -> Bool {
        await tableWrite("/away", ["away": .bool(away)])
    }

    public func rebuy(_ amount: Int) async -> Bool {
        await tableWrite("/rebuy", ["amount": .int(Int64(amount))])
    }

    public func addBot() async -> Bool {
        await tableWrite("/bots", ["add": .bool(true)])
    }

    public func removeBot(seat: Int) async -> Bool {
        await tableWrite("/bots", ["remove": .int(Int64(seat))])
    }

    public func sendChat(_ text: String) async -> Bool {
        let trimmed = String(text.trimmingCharacters(in: .whitespacesAndNewlines).prefix(200))
        guard !trimmed.isEmpty else { return false }
        return await tableWrite("/chat", ["text": .string(trimmed)])
    }

    public func invite(_ text: String) async -> Bool {
        let names = PokerModel.inviteNames(text)
        guard !names.isEmpty, let id = tableId, !busy else { return false }
        busy = true
        defer { busy = false }
        do {
            let reply = try await api.call("POST", "/tables/\(id)/invite", ["names": .array(names.map { .string($0) })])
            take(reply, for: id)
            notice = PokerModel.inviteSummary(invited: reply.strings("invited"), skipped: reply.strings("skipped"))
            return true
        } catch {
            fail(error)
            return false
        }
    }

    public func leave() async -> Bool {
        await tableWrite("/leave", [:])
    }

    public func closeForEveryone() async -> Bool {
        guard let id = tableId, !busy else { return false }
        busy = true
        defer { busy = false }
        do {
            let reply = try await api.call("DELETE", "/tables/\(id)", nil)
            guard reply.bool("closed") || reply.optBool("ok") != false else { return false }
            notice = "Table closed. Every stack went back to its wallet."
            closeTable()
            return true
        } catch {
            fail(error)
            return false
        }
    }

    public func appeared() {
        if let table { schedule(table) }
    }

    public func disappeared() {
        cancelTimers()
    }

    private func tableWrite(_ suffix: String, _ body: JSONObject, quietCodes: Set<String> = []) async -> Bool {
        guard let id = tableId, !busy else { return false }
        busy = true
        defer { busy = false }
        do {
            let reply = try await api.call("POST", "/tables/\(id)\(suffix)", body)
            take(reply, for: id)
            return true
        } catch {
            let code = Self.code(error)
            notice = PokerText.error(code)
            if quietCodes.contains(code) || code == "hand_in_progress" || code == "seat_taken" {
                Task { [weak self] in await self?.reloadTable() }
            } else if code == "table_not_found" || code == "not_found" {
                tableError = PokerText.error(code)
                table = nil
            }
            return false
        }
    }

    private func take(_ reply: JSONObject, for id: String) {
        guard tableId == id else { return }
        let json = reply["table"]?.objectValue ?? reply
        guard let table = PokerTable.from(json), table.id == id else {
            tableError = PokerText.error("bad_reply")
            return
        }
        apply(table)
    }

    func apply(_ next: PokerTable) {
        if let current = table, current.id == next.id, next.version < current.version { return }
        serverClock.sync(serverNow: next.now, localNow: localNow())
        let events = cursor.take(next)
        let first = table == nil
        table = next
        tableError = ""
        animates = !events.isEmpty
        if !events.isEmpty {
            lastEvents = events
            eventBatch += 1
            onEvents?(events, next)
        }
        let key = next.myTurn ? "\(next.handNumber)-\(next.phase)-\(next.actStartedAt)-\(next.currentBet)" : ""
        if !key.isEmpty, key != turnKey, !first {
            onMyTurn?()
        }
        turnKey = key
        if let legal = next.legal, legal.raise != nil {
            if raiseTo == 0 || PokerActions.clamp(raiseTo, legal) != raiseTo { raiseTo = PokerActions.defaultRaise(next) }
        } else {
            raiseTo = 0
        }
        if let lobby, next.wallet != lobby.wallet.chips, next.wallet > 0 || lobby.wallet.chips > 0 {
            var updated = lobby
            updated.wallet.chips = next.wallet
            self.lobby = updated
        }
        schedule(next)
    }

    private func schedule(_ table: PokerTable) {
        scheduleWake(table)
        scheduleClock(table)
    }

    private func scheduleWake(_ table: PokerTable) {
        guard tableId == table.id, let delay = PokerClock.wakeDelayMs(wakeAt: table.wakeAt, serverNow: serverNow) else {
            wakeTask?.cancel()
            wakeTask = nil
            wakeTarget = 0
            wakeRetries = 0
            return
        }
        if wakeTarget == table.wakeAt, wakeTask != nil { return }
        var wait = delay
        if wakeTarget == table.wakeAt {
            guard wakeRetries < 3 else { return }
            wakeRetries += 1
            wait = max(delay, Int64(wakeRetries) * 1000)
        } else {
            wakeRetries = 0
        }
        wakeTask?.cancel()
        wakeTarget = table.wakeAt
        let sleeper = self.sleeper
        let target = table.wakeAt
        wakeTask = Task { [weak self] in
            do { try await sleeper(wait) } catch { return }
            guard !Task.isCancelled, let self, self.wakeTarget == target else { return }
            self.wakeTask = nil
            await self.tick()
        }
    }

    private func scheduleClock(_ table: PokerTable) {
        guard table.myTurn, table.actDeadline > 0 else {
            clockTask?.cancel()
            clockTask = nil
            clockDeadline = 0
            return
        }
        if clockDeadline == table.actDeadline, clockTask != nil { return }
        clockTask?.cancel()
        clockDeadline = table.actDeadline
        let deadline = table.actDeadline
        let sleeper = self.sleeper
        clockTask = Task { [weak self] in
            guard let start = self?.serverNow else { return }
            for at in PokerClock.tickTimes(deadline: deadline, serverNow: start) {
                guard let now = self?.serverNow else { return }
                do { try await sleeper(max(0, at - now)) } catch { return }
                guard !Task.isCancelled, let self, self.clockDeadline == deadline, self.table?.myTurn == true else { return }
                self.onCue?(.clock)
            }
        }
    }

    private func scheduleClaim() {
        let wallet = self.wallet
        guard !wallet.canClaim, wallet.nextClaimAt > 0, wallet.nextClaimAt != claimFired else {
            claimTask?.cancel()
            claimTask = nil
            claimTarget = 0
            return
        }
        if claimTarget == wallet.nextClaimAt, claimTask != nil { return }
        claimTask?.cancel()
        claimTarget = wallet.nextClaimAt
        let wait = max(0, wallet.nextClaimAt - serverNow) + 500
        let sleeper = self.sleeper
        let target = wallet.nextClaimAt
        claimTask = Task { [weak self] in
            do { try await sleeper(wait) } catch { return }
            guard !Task.isCancelled, let self, self.claimTarget == target else { return }
            self.claimTask = nil
            self.claimTarget = 0
            self.claimFired = target
            await self.loadLobby()
        }
    }

    private func cancelTimers() {
        wakeTask?.cancel()
        wakeTask = nil
        wakeTarget = 0
        wakeRetries = 0
        clockTask?.cancel()
        clockTask = nil
        clockDeadline = 0
    }

    private func failTable(_ error: Error, for id: String) {
        guard tableId == id else { return }
        let code = Self.code(error)
        if code == "table_not_found" || code == "not_found" {
            table = nil
            cancelTimers()
        }
        if Self.notInstalledCodes.contains(code) { notInstalled = true }
        tableError = PokerText.error(code)
    }

    private func fail(_ error: Error) {
        let code = Self.code(error)
        if Self.notInstalledCodes.contains(code) { notInstalled = true }
        notice = PokerText.error(code)
    }

    nonisolated static let notInstalledCodes: Set<String> = ["plugin_not_installed", "plugin_not_installed_for_account"]

    nonisolated static func code(_ error: Error) -> String {
        if let failure = error as? OrgApiException { return failure.code }
        if let failure = error as? PackagesError { return failure.code }
        return ""
    }

    nonisolated public static func inviteNames(_ text: String) -> [String] {
        var seen = Set<String>()
        var out: [String] = []
        for part in text.split(whereSeparator: { $0 == "," || $0 == " " || $0 == "\n" || $0 == ";" }) {
            var name = String(part).trimmingCharacters(in: .whitespaces)
            if name.hasPrefix("@") { name.removeFirst() }
            let lowered = name.lowercased()
            guard !lowered.isEmpty, lowered.count <= 64, seen.insert(lowered).inserted else { continue }
            out.append(lowered)
        }
        return Array(out.prefix(10))
    }

    nonisolated public static func inviteSummary(invited: [String], skipped: [String]) -> String {
        var parts: [String] = []
        if !invited.isEmpty { parts.append("Invited \(invited.joined(separator: ", ")).") }
        if !skipped.isEmpty { parts.append("Skipped \(skipped.joined(separator: ", ")).") }
        return parts.isEmpty ? "Nobody was invited." : parts.joined(separator: " ")
    }
}
