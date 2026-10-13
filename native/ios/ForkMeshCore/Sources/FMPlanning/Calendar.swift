import FMApi
import FMBase
import Foundation

public struct CalendarAttendee: Equatable, Hashable, Sendable {
    public var name: String
    public var status: String

    public init(name: String, status: String = "invited") {
        self.name = name
        self.status = status
    }
}

public enum CalendarRecurrence: String, CaseIterable, Sendable {
    case none, daily, weekly, monthly, yearly

    public var label: String {
        switch self {
        case .none: return "Does not repeat"
        case .daily: return "Every day"
        case .weekly: return "Every week"
        case .monthly: return "Every month"
        case .yearly: return "Every year"
        }
    }

    var component: Calendar.Component? {
        switch self {
        case .none: return nil
        case .daily: return .day
        case .weekly: return .weekOfYear
        case .monthly: return .month
        case .yearly: return .year
        }
    }
}

public struct CalendarEvent: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var owner: String
    public var calendar: String
    public var scope: String
    public var startAt: Int64
    public var endAt: Int64
    public var allDay: Bool
    public var timezone: String
    public var title: String
    public var description: String
    public var location: String
    public var meetingUrl: String
    public var color: String
    public var reminders: [Int]
    public var recurrence: CalendarRecurrence
    public var recurrenceUntil: Int64
    public var attendees: [CalendarAttendee]
    public var updatedAt: Int64

    public init(
        id: String, owner: String = "", calendar: String = "personal", scope: String = "personal", startAt: Int64,
        endAt: Int64, allDay: Bool = false, timezone: String = "", title: String, description: String = "",
        location: String = "", meetingUrl: String = "", color: String = "", reminders: [Int] = [],
        recurrence: CalendarRecurrence = .none, recurrenceUntil: Int64 = 0, attendees: [CalendarAttendee] = [],
        updatedAt: Int64 = 0
    ) {
        self.id = id
        self.owner = owner
        self.calendar = calendar
        self.scope = scope
        self.startAt = startAt
        self.endAt = endAt
        self.allDay = allDay
        self.timezone = timezone
        self.title = title
        self.description = description
        self.location = location
        self.meetingUrl = meetingUrl
        self.color = color
        self.reminders = reminders
        self.recurrence = recurrence
        self.recurrenceUntil = recurrenceUntil
        self.attendees = attendees
        self.updatedAt = updatedAt
    }

    public init?(json: JSONObject) {
        let id = json.str("id")
        guard !id.isEmpty else { return nil }
        self.init(
            id: id, owner: json.str("owner"), calendar: json.str("calendar", "personal"), scope: json.str("scope", "personal"),
            startAt: json.int64("startAt"), endAt: json.int64("endAt"), allDay: json.bool("allDay"),
            timezone: json.str("timezone"), title: json.str("title"), description: json.str("description"),
            location: json.str("location"), meetingUrl: json.str("meetingUrl"), color: json.str("color"),
            reminders: json.list("reminders").compactMap(\.intValue),
            recurrence: CalendarRecurrence(rawValue: json.str("recurrence")) ?? .none,
            recurrenceUntil: json.int64("recurrenceUntil"),
            attendees: json.maps("attendees").map { CalendarAttendee(name: $0.str("name"), status: $0.str("status", "invited")) },
            updatedAt: json.int64("updatedAt"))
    }

    public var durationMs: Int64 { max(0, endAt - startAt) }

    public func attendeeStatus(_ account: String) -> String? {
        attendees.first { $0.name.lowercased() == account.lowercased() }?.status
    }

    public func isOwned(by account: String) -> Bool { owner.lowercased() == account.lowercased() }
}

public struct CalendarInfo: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var scope: String
    public var role: String

    public init(id: String, name: String, scope: String, role: String = "") {
        self.id = id
        self.name = name
        self.scope = scope
        self.role = role
    }
}

public struct CalendarOccurrence: Equatable, Hashable, Sendable, Identifiable {
    public var event: CalendarEvent
    public var startAt: Int64
    public var endAt: Int64
    public var id: String { "\(event.id)@\(startAt)" }
}

public struct CalendarAgendaDay: Equatable, Sendable, Identifiable {
    public var day: String
    public var items: [CalendarOccurrence]
    public var id: String { day }
}

public enum CalendarMath {
    public static let maxOccurrencesPerEvent = 1_000

    public static func calendar(_ timeZone: TimeZone) -> Calendar {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        return calendar
    }

    public static func occurrences(
        of event: CalendarEvent, from rangeStart: Int64, to rangeEnd: Int64, timeZone: TimeZone
    ) -> [CalendarOccurrence] {
        guard rangeEnd > rangeStart else { return [] }
        let duration = event.durationMs
        guard let component = event.recurrence.component else {
            let inside = duration == 0
                ? event.startAt >= rangeStart && event.startAt < rangeEnd
                : event.endAt > rangeStart && event.startAt < rangeEnd
            return inside ? [CalendarOccurrence(event: event, startAt: event.startAt, endAt: event.endAt)] : []
        }
        let zone = TimeZone(identifier: event.timezone) ?? timeZone
        let cal = calendar(zone)
        let base = Date(epochMillis: event.startAt)
        let until = event.recurrenceUntil > 0 ? min(event.recurrenceUntil, rangeEnd) : rangeEnd
        var out: [CalendarOccurrence] = []
        var index = 0
        if duration > 0 || rangeStart > event.startAt {
            let span = Double(max(0, rangeStart - duration - event.startAt)) / 1_000
            let unit: Double
            switch component {
            case .day: unit = 86_400
            case .weekOfYear: unit = 7 * 86_400
            case .month: unit = 31 * 86_400
            default: unit = 366 * 86_400
            }
            index = max(0, Int(span / unit) - 2)
        }
        while out.count < maxOccurrencesPerEvent {
            guard let date = cal.date(byAdding: component, value: index, to: base) else { break }
            let start = date.epochMillis
            if start >= until { break }
            let end = start + duration
            if end > rangeStart || (duration == 0 && start >= rangeStart) {
                out.append(CalendarOccurrence(event: event, startAt: start, endAt: end))
            }
            index += 1
        }
        return out
    }

    public static func occurrences(
        _ events: [CalendarEvent], from rangeStart: Int64, to rangeEnd: Int64, timeZone: TimeZone
    ) -> [CalendarOccurrence] {
        events.flatMap { occurrences(of: $0, from: rangeStart, to: rangeEnd, timeZone: timeZone) }.sorted {
            $0.startAt != $1.startAt ? $0.startAt < $1.startAt : $0.event.title < $1.event.title
        }
    }

    public static func dayKey(_ ms: Int64, timeZone: TimeZone) -> String {
        let parts = calendar(timeZone).dateComponents([.year, .month, .day], from: Date(epochMillis: ms))
        return String(format: "%04d-%02d-%02d", parts.year ?? 0, parts.month ?? 0, parts.day ?? 0)
    }

    public static func parseDayKey(_ key: String, timeZone: TimeZone) -> Int64? {
        let parts = key.split(separator: "-").compactMap { Int($0) }
        guard parts.count == 3, (1...12).contains(parts[1]), (1...31).contains(parts[2]) else { return nil }
        let cal = calendar(timeZone)
        guard let date = cal.date(from: DateComponents(year: parts[0], month: parts[1], day: parts[2])),
              cal.component(.day, from: date) == parts[2] else { return nil }
        return date.epochMillis
    }

    public static func startOfDay(_ ms: Int64, timeZone: TimeZone) -> Int64 {
        calendar(timeZone).startOfDay(for: Date(epochMillis: ms)).epochMillis
    }

    public static func addDays(_ ms: Int64, _ days: Int, timeZone: TimeZone) -> Int64 {
        (calendar(timeZone).date(byAdding: .day, value: days, to: Date(epochMillis: ms)) ?? Date(epochMillis: ms)).epochMillis
    }

    public static func addMonths(_ ms: Int64, _ months: Int, timeZone: TimeZone) -> Int64 {
        (calendar(timeZone).date(byAdding: .month, value: months, to: Date(epochMillis: ms)) ?? Date(epochMillis: ms)).epochMillis
    }

    public static func startOfWeek(_ ms: Int64, firstWeekday: Int, timeZone: TimeZone) -> Int64 {
        let day = startOfDay(ms, timeZone: timeZone)
        let weekday = calendar(timeZone).component(.weekday, from: Date(epochMillis: day))
        let back = (weekday - firstWeekday + 7) % 7
        return addDays(day, -back, timeZone: timeZone)
    }

    public static func startOfMonth(_ ms: Int64, timeZone: TimeZone) -> Int64 {
        let cal = calendar(timeZone)
        let parts = cal.dateComponents([.year, .month], from: Date(epochMillis: ms))
        return (cal.date(from: parts) ?? Date(epochMillis: ms)).epochMillis
    }

    public static func monthGrid(_ ms: Int64, firstWeekday: Int, timeZone: TimeZone) -> [Int64] {
        let first = startOfMonth(ms, timeZone: timeZone)
        let start = startOfWeek(first, firstWeekday: firstWeekday, timeZone: timeZone)
        return (0..<42).map { addDays(start, $0, timeZone: timeZone) }
    }

    public static func weekDays(_ ms: Int64, firstWeekday: Int, timeZone: TimeZone) -> [Int64] {
        let start = startOfWeek(ms, firstWeekday: firstWeekday, timeZone: timeZone)
        return (0..<7).map { addDays(start, $0, timeZone: timeZone) }
    }

    public static func range(for mode: CalendarMode, anchor: Int64, firstWeekday: Int, timeZone: TimeZone) -> (start: Int64, end: Int64) {
        switch mode {
        case .day:
            let start = startOfDay(anchor, timeZone: timeZone)
            return (start, addDays(start, 1, timeZone: timeZone))
        case .week:
            let start = startOfWeek(anchor, firstWeekday: firstWeekday, timeZone: timeZone)
            return (start, addDays(start, 7, timeZone: timeZone))
        case .month:
            let grid = monthGrid(anchor, firstWeekday: firstWeekday, timeZone: timeZone)
            return (grid[0], addDays(grid[41], 1, timeZone: timeZone))
        case .agenda:
            let start = startOfDay(anchor, timeZone: timeZone)
            return (start, addDays(start, 30, timeZone: timeZone))
        }
    }

    public static func agenda(_ occurrences: [CalendarOccurrence], timeZone: TimeZone) -> [CalendarAgendaDay] {
        var order: [String] = []
        var groups: [String: [CalendarOccurrence]] = [:]
        for occurrence in occurrences {
            let key = dayKey(occurrence.startAt, timeZone: timeZone)
            if groups[key] == nil { order.append(key) }
            groups[key, default: []].append(occurrence)
        }
        return order.map { CalendarAgendaDay(day: $0, items: groups[$0] ?? []) }
    }

    public static func overlaps(_ occurrence: CalendarOccurrence, dayStart: Int64, timeZone: TimeZone) -> Bool {
        let dayEnd = addDays(dayStart, 1, timeZone: timeZone)
        if occurrence.endAt == occurrence.startAt { return occurrence.startAt >= dayStart && occurrence.startAt < dayEnd }
        return occurrence.startAt < dayEnd && occurrence.endAt > dayStart
    }
}

extension CalendarMode {
    public var label: String { rawValue.capitalized }
}

public struct CalendarEventDraft: Equatable, Sendable {
    public static let allowedReminders = [0, 5, 10, 15, 30, 60, 120, 1_440, 2_880, 10_080]
    public static let defaultReminders = [1_440, 60, 10]
    public static let titleMaxLength = 200

    public var title: String
    public var startAt: Int64
    public var endAt: Int64
    public var allDay: Bool
    public var timezone: String
    public var calendar: String
    public var description: String
    public var location: String
    public var meetingUrl: String
    public var reminders: [Int]
    public var recurrence: CalendarRecurrence
    public var recurrenceUntil: Int64
    public var attendees: [String]

    public init(startAt: Int64, endAt: Int64, timezone: String, calendar: String = "personal") {
        title = ""
        self.startAt = startAt
        self.endAt = endAt
        allDay = false
        self.timezone = timezone
        self.calendar = calendar
        description = ""
        location = ""
        meetingUrl = ""
        reminders = CalendarEventDraft.defaultReminders
        recurrence = .none
        recurrenceUntil = 0
        attendees = []
    }

    public init(event: CalendarEvent) {
        title = event.title
        startAt = event.startAt
        endAt = event.endAt
        allDay = event.allDay
        timezone = event.timezone
        calendar = event.calendar
        description = event.description
        location = event.location
        meetingUrl = event.meetingUrl
        reminders = event.reminders
        recurrence = event.recurrence
        recurrenceUntil = event.recurrenceUntil
        attendees = event.attendees.map(\.name)
    }

    public var validationError: String? {
        let trimmed = title.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { return "Give the event a title." }
        if trimmed.count > CalendarEventDraft.titleMaxLength { return "Keep the title under 200 characters." }
        if endAt <= startAt { return "The event must end after it starts." }
        if reminders.count > 5 { return "At most five reminders." }
        if reminders.contains(where: { !CalendarEventDraft.allowedReminders.contains($0) }) { return "Choose reminders from the list." }
        let link = meetingUrl.trimmingCharacters(in: .whitespaces)
        if !link.isEmpty, URL(string: link)?.scheme?.lowercased() != "https" { return "Meeting links must start with https://." }
        if recurrence != .none && recurrenceUntil > 0 && recurrenceUntil < startAt { return "The repeat must end after the first event." }
        if attendees.count > 50 { return "Invite at most 50 people." }
        return nil
    }

    public var body: JSONObject {
        var body: JSONObject = [
            "title": .string(title.trimmingCharacters(in: .whitespacesAndNewlines)),
            "startAt": .int(startAt),
            "endAt": .int(endAt),
            "allDay": .bool(allDay),
            "description": .string(description),
            "location": .string(location.trimmingCharacters(in: .whitespaces)),
            "meetingUrl": .string(meetingUrl.trimmingCharacters(in: .whitespaces)),
            "reminders": .array(Array(Set(reminders)).sorted(by: >).map { .int(Int64($0)) }),
            "recurrence": .string(recurrence.rawValue),
            "recurrenceUntil": .int(recurrence == .none ? 0 : recurrenceUntil),
            "attendees": .array(attendees.map { $0.trimmingCharacters(in: .whitespaces).lowercased() }.filter { !$0.isEmpty }.map { .string($0) }),
        ]
        if !timezone.isEmpty { body["timezone"] = .string(timezone) }
        if calendar == "personal" || calendar.isEmpty {
            body["scope"] = "personal"
        } else {
            body["scope"] = "organization"
            body["calendar"] = .string(calendar)
        }
        return body
    }

    public static func reminderLabel(_ minutes: Int) -> String {
        switch minutes {
        case 0: return "At the start"
        case 1_440: return "1 day before"
        case 2_880: return "2 days before"
        case 10_080: return "1 week before"
        default: return minutes >= 60 ? "\(minutes / 60) hour\(minutes == 60 ? "" : "s") before" : "\(minutes) minutes before"
        }
    }
}
