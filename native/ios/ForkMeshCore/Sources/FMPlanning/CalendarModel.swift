import FMApi
import FMBase
import Foundation
import Observation

@MainActor
@Observable
public final class CalendarModel {
    public private(set) var events: [CalendarEvent] = []
    public private(set) var calendars: [CalendarInfo] = [CalendarInfo(id: "personal", name: "Personal", scope: "personal")]
    public private(set) var actor = ""
    public private(set) var loading = false
    public private(set) var busy = false
    public private(set) var error: String?
    public var mode: CalendarMode
    public private(set) var anchor: Int64
    public var hiddenCalendars: Set<String> = []

    @ObservationIgnored private let client: PlanningClient
    @ObservationIgnored public let timeZone: TimeZone
    @ObservationIgnored public let firstWeekday: Int
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored private var loadedRange: (start: Int64, end: Int64)?
    @ObservationIgnored private var loadToken = 0
    @ObservationIgnored private var revision = 0
    @ObservationIgnored private var localChanges: [(revision: Int, id: String, event: CalendarEvent?)] = []

    public init(client: PlanningClient, mode: CalendarMode = .month, anchorDay: String? = nil, timeZone: TimeZone = .current,
                firstWeekday: Int = Calendar.current.firstWeekday, clock: Clock = SystemClock()) {
        self.client = client
        self.mode = mode
        self.timeZone = timeZone
        self.firstWeekday = firstWeekday
        self.clock = clock
        anchor = anchorDay.flatMap { CalendarMath.parseDayKey($0, timeZone: timeZone) }
            ?? CalendarMath.startOfDay(clock.nowMillis, timeZone: timeZone)
    }

    public var range: (start: Int64, end: Int64) {
        CalendarMath.range(for: mode, anchor: anchor, firstWeekday: firstWeekday, timeZone: timeZone)
    }

    public var visibleEvents: [CalendarEvent] { events.filter { !hiddenCalendars.contains($0.calendar) } }

    public var occurrences: [CalendarOccurrence] {
        CalendarMath.occurrences(visibleEvents, from: range.start, to: range.end, timeZone: timeZone)
    }

    public func occurrences(onDay dayStart: Int64) -> [CalendarOccurrence] {
        occurrences.filter { CalendarMath.overlaps($0, dayStart: dayStart, timeZone: timeZone) }
    }

    public var days: [Int64] {
        switch mode {
        case .month: return CalendarMath.monthGrid(anchor, firstWeekday: firstWeekday, timeZone: timeZone)
        case .week: return CalendarMath.weekDays(anchor, firstWeekday: firstWeekday, timeZone: timeZone)
        case .day: return [CalendarMath.startOfDay(anchor, timeZone: timeZone)]
        case .agenda: return []
        }
    }

    public var agenda: [CalendarAgendaDay] { CalendarMath.agenda(occurrences, timeZone: timeZone) }

    public var today: Int64 { CalendarMath.startOfDay(clock.nowMillis, timeZone: timeZone) }

    public var anchorKey: String { CalendarMath.dayKey(anchor, timeZone: timeZone) }

    public func isInAnchorMonth(_ day: Int64) -> Bool {
        CalendarMath.startOfMonth(day, timeZone: timeZone) == CalendarMath.startOfMonth(anchor, timeZone: timeZone)
    }

    public func step(_ direction: Int) async {
        switch mode {
        case .day: anchor = CalendarMath.addDays(anchor, direction, timeZone: timeZone)
        case .week: anchor = CalendarMath.addDays(anchor, 7 * direction, timeZone: timeZone)
        case .month: anchor = CalendarMath.addMonths(CalendarMath.startOfMonth(anchor, timeZone: timeZone), direction, timeZone: timeZone)
        case .agenda: anchor = CalendarMath.addDays(anchor, 30 * direction, timeZone: timeZone)
        }
        await loadIfNeeded()
    }

    public func goToToday() async {
        anchor = today
        await loadIfNeeded()
    }

    public func select(day: Int64) { anchor = CalendarMath.startOfDay(day, timeZone: timeZone) }

    public func setMode(_ mode: CalendarMode) async {
        self.mode = mode
        await loadIfNeeded()
    }

    func loadIfNeeded() async {
        let wanted = range
        if let loaded = loadedRange, loaded.start <= wanted.start, loaded.end >= wanted.end { return }
        await load()
    }

    public func load() async {
        let wanted = range
        let start = CalendarMath.addDays(wanted.start, -31, timeZone: timeZone)
        let end = CalendarMath.addDays(wanted.end, 31, timeZone: timeZone)
        loadToken += 1
        let token = loadToken
        let seen = revision
        loading = true
        defer { if token == loadToken { loading = false } }
        do {
            let data = try await client.accountApiCall("GET", "/api/calendar?start=\(start)&end=\(end)", nil)
            guard token == loadToken else { return }
            events = merged(data.maps("events").prefix(500).compactMap(CalendarEvent.init(json:)), since: seen)
            let infos = data.maps("calendars").compactMap { map -> CalendarInfo? in
                let id = map.str("id", map.str("name"))
                return id.isEmpty ? nil : CalendarInfo(id: id, name: map.str("name", id), scope: map.str("scope"), role: map.str("role"))
            }
            if !infos.isEmpty { calendars = infos }
            actor = data.str("actor").lowercased()
            loadedRange = (start, end)
            error = nil
        } catch {
            guard token == loadToken else { return }
            self.error = PlanningErrors.text(error, "The calendar could not be loaded.")
        }
    }

    private func merged(_ server: [CalendarEvent], since seen: Int) -> [CalendarEvent] {
        localChanges.removeAll { $0.revision <= seen }
        var result = server
        for change in localChanges {
            result.removeAll { $0.id == change.id }
            if let event = change.event { result.append(event) }
        }
        return result
    }

    private func recordChange(_ id: String, _ event: CalendarEvent?) {
        revision += 1
        localChanges.append((revision, id, event))
    }

    public func clearError() { error = nil }

    public func event(_ id: String) -> CalendarEvent? { events.first { $0.id == id } }

    public func loadEvent(_ id: String) async {
        guard CalendarModel.validId(id) else {
            error = "That event link is not valid."
            return
        }
        loading = true
        defer { loading = false }
        do {
            let data = try await client.accountApiCall("GET", "/api/calendar/\(id)", nil)
            _ = upsert(data, local: false)
            if actor.isEmpty { actor = data.str("actor").lowercased() }
            error = nil
        } catch {
            self.error = PlanningErrors.text(error, "The event could not be opened.")
        }
    }

    public func canEdit(_ event: CalendarEvent) -> Bool {
        if event.isOwned(by: actor) { return true }
        let role = calendars.first { $0.name == event.calendar || $0.id == event.calendar }?.role ?? ""
        return event.scope == "organization" && (role == "owner" || role == "admin")
    }

    public func newDraft(on day: Int64? = nil) -> CalendarEventDraft {
        let base = day ?? anchor
        let now = clock.nowMillis
        let hour: Int64 = 3_600_000
        let start: Int64
        if CalendarMath.startOfDay(now, timeZone: timeZone) == CalendarMath.startOfDay(base, timeZone: timeZone) {
            start = (now / hour + 1) * hour
        } else {
            start = CalendarMath.startOfDay(base, timeZone: timeZone) + 9 * hour
        }
        return CalendarEventDraft(startAt: start, endAt: start + hour, timezone: timeZone.identifier)
    }

    private func mutate(_ method: String, _ path: String, _ body: JSONObject?, _ fallback: String) async -> JSONObject? {
        if busy { return nil }
        busy = true
        defer { busy = false }
        do {
            let data = try await client.accountApiCall(method, path, body)
            error = nil
            return data
        } catch {
            self.error = PlanningErrors.text(error, fallback)
            return nil
        }
    }

    private func upsert(_ data: JSONObject, local: Bool = true) -> CalendarEvent? {
        guard let event = data["event"]?.objectValue.flatMap(CalendarEvent.init(json:)) else { return nil }
        events.removeAll { $0.id == event.id }
        events.append(event)
        if local { recordChange(event.id, event) }
        return event
    }

    public func create(_ draft: CalendarEventDraft) async -> CalendarEvent? {
        if let problem = draft.validationError {
            error = problem
            return nil
        }
        guard let data = await mutate("POST", "/api/calendar", draft.body, "The event was not saved.") else { return nil }
        return upsert(data)
    }

    public func update(_ id: String, _ draft: CalendarEventDraft) async -> CalendarEvent? {
        if let problem = draft.validationError {
            error = problem
            return nil
        }
        guard CalendarModel.validId(id),
              let data = await mutate("PATCH", "/api/calendar/\(id)", draft.body, "The event was not saved.") else { return nil }
        return upsert(data)
    }

    @discardableResult
    public func delete(_ id: String) async -> Bool {
        guard CalendarModel.validId(id), await mutate("DELETE", "/api/calendar/\(id)", nil, "The event was not deleted.") != nil else {
            return false
        }
        events.removeAll { $0.id == id }
        recordChange(id, nil)
        return true
    }

    @discardableResult
    public func rsvp(_ id: String, status: String) async -> Bool {
        guard CalendarModel.validId(id), ["accepted", "declined", "tentative"].contains(status),
              let data = await mutate("POST", "/api/calendar/\(id)/rsvp", ["status": .string(status)], "Your answer was not sent.") else {
            return false
        }
        if upsert(data) == nil, let index = events.firstIndex(where: { $0.id == id }),
           let attendee = events[index].attendees.firstIndex(where: { $0.name.lowercased() == actor }) {
            events[index].attendees[attendee].status = status
            recordChange(id, events[index])
        }
        return true
    }

    public static func validId(_ id: String) -> Bool {
        id.count == 32 && id.allSatisfy { $0.isHexDigit && ($0.isNumber || $0.isLowercase) }
    }
}
