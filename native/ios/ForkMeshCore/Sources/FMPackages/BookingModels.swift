import FMBase
import Foundation

public struct BookingLocation: Equatable, Hashable, Sendable {
    public var kind: String
    public var detail: String
    public var room: Bool

    public init(kind: String = "video", detail: String = "", room: Bool = false) {
        self.kind = kind
        self.detail = detail
        self.room = room
    }

    public init(json: JSONObject) {
        let raw = json.str("kind").trimmingCharacters(in: .whitespaces).lowercased()
        kind = BookingText.locationKinds.contains(raw) ? raw : "video"
        detail = json.str("detail")
        room = json.bool("room")
    }

    public var body: JSONObject { ["kind": .string(kind), "detail": .string(detail.trimmingCharacters(in: .whitespacesAndNewlines))] }
}

public struct BookingGuest: Equatable, Hashable, Sendable {
    public var name: String
    public var email: String
    public var phone: String
    public var timezone: String
    public var notes: String

    public init(name: String = "", email: String = "", phone: String = "", timezone: String = "", notes: String = "") {
        self.name = name
        self.email = email
        self.phone = phone
        self.timezone = timezone
        self.notes = notes
    }

    public init(json: JSONObject) {
        self.init(
            name: json.str("name"), email: json.str("email"), phone: json.str("phone"), timezone: json.str("timezone"),
            notes: json.str("notes"))
    }
}

public struct Booking: Identifiable, Equatable, Hashable, Sendable {
    public var id: String
    public var status: String
    public var typeId: String
    public var typeTitle: String
    public var minutes: Int
    public var startAt: Int64
    public var endAt: Int64
    public var createdAt: Int64
    public var cancelledAt: Int64
    public var cancelledBy: String
    public var cancelReason: String
    public var guest: BookingGuest
    public var location: BookingLocation
    public var calendarEventId: String
    public var rescheduledFrom: Int64

    public init(json: JSONObject) {
        id = json.str("id")
        status = json.str("status", "confirmed").isEmpty ? "confirmed" : json.str("status", "confirmed")
        typeId = json.str("typeId")
        typeTitle = json.str("typeTitle")
        minutes = max(0, json.int("minutes"))
        startAt = json.int64("startAt")
        endAt = max(json.int64("endAt"), json.int64("startAt"))
        createdAt = json.int64("createdAt")
        cancelledAt = json.int64("cancelledAt")
        cancelledBy = json.str("cancelledBy")
        cancelReason = json.str("cancelReason")
        guest = BookingGuest(json: json.obj("guest"))
        let place = json.obj("location")
        location = place.isEmpty ? BookingLocation(kind: "none") : BookingLocation(json: place)
        calendarEventId = json.str("calendarEventId")
        rescheduledFrom = json.int64("rescheduledFrom")
    }

    public var isCancelled: Bool { status == "cancelled" }

    public func isPast(now: Int64) -> Bool { !isCancelled && endAt < now }

    public func canCancel(now: Int64) -> Bool { !isCancelled && endAt >= now }

    public func badges(now: Int64) -> [String] {
        var out: [String] = []
        if isCancelled {
            switch cancelledBy {
            case "owner": out.append("Cancelled by you")
            case "guest": out.append("Cancelled by guest")
            default: out.append("Cancelled")
            }
        } else if isPast(now: now) {
            out.append("Done")
        }
        if rescheduledFrom > 0 { out.append("Moved") }
        return out
    }

    public var joinURL: URL? {
        guard location.kind == "video", location.room, !isCancelled, location.detail.hasPrefix("https://") else { return nil }
        return URL(string: location.detail)
    }

    public var whereText: String {
        if joinURL != nil { return "ForkMesh video call, end-to-end encrypted" }
        switch location.kind {
        case "phone": return location.detail.isEmpty ? (guest.phone.isEmpty ? "Phone call" : guest.phone) : location.detail
        case "none": return ""
        default: return location.detail.isEmpty ? BookingText.locationLabel(location.kind) : location.detail
        }
    }
}

public struct BookingType: Identifiable, Equatable, Hashable, Sendable {
    public var id: String
    public var title: String
    public var minutes: Int
    public var description: String
    public var location: BookingLocation
    public var enabled: Bool

    public init(
        id: String = "", title: String = "New meeting", minutes: Int = 30, description: String = "",
        location: BookingLocation = BookingLocation(), enabled: Bool = true
    ) {
        self.id = id
        self.title = title
        self.minutes = minutes
        self.description = description
        self.location = location
        self.enabled = enabled
    }

    public init(json: JSONObject) {
        id = json.str("id")
        title = json.str("title")
        minutes = max(0, json.int("minutes", 30))
        description = json.str("description")
        location = BookingLocation(json: json.obj("location"))
        enabled = json.optBool("enabled") ?? true
    }

    public var body: JSONObject {
        [
            "id": .string(id), "title": .string(title.trimmingCharacters(in: .whitespacesAndNewlines)),
            "minutes": .int(Int64(minutes)), "description": .string(description), "location": .object(location.body),
            "enabled": .bool(enabled),
        ]
    }
}

public struct BookingWindow: Equatable, Hashable, Sendable {
    public var start: String
    public var end: String

    public init(start: String, end: String) {
        self.start = start
        self.end = end
    }
}

public struct BookingSettings: Equatable, Sendable {
    public var displayName = ""
    public var headline = ""
    public var welcome = ""
    public var timezone = "UTC"
    public var timeFormat = "12h"
    public var accent = "blue"
    public var availability: [String: [BookingWindow]] = [:]
    public var minNoticeMinutes = 240
    public var maxDaysAhead = 60
    public var bufferMinutes = 0
    public var slotStepMinutes = 0
    public var dailyLimit = 0
    public var replyTo = ""
    public var notifyEmail = ""
    public var emailOwner = true
    public var addToCalendar = true
    public var checkCalendar = true
    public var notifyVisits = true
    public var types: [BookingType] = []

    public init() {}

    public init(json: JSONObject) {
        displayName = json.str("displayName")
        headline = json.str("headline")
        welcome = json.str("welcome")
        timezone = json.str("timezone", "UTC").isEmpty ? "UTC" : json.str("timezone", "UTC")
        timeFormat = json.str("timeFormat") == "24h" ? "24h" : "12h"
        accent = json.str("accent", "blue").isEmpty ? "blue" : json.str("accent", "blue")
        let week = json.obj("availability")
        for weekday in BookingText.weekdays {
            let day = weekday.id
            availability[day] = week.list(day).compactMap { value -> BookingWindow? in
                let pair = value.strings
                guard pair.count == 2 else { return nil }
                return BookingWindow(start: pair[0], end: pair[1])
            }
        }
        minNoticeMinutes = json.int("minNoticeMinutes", 240)
        maxDaysAhead = json.int("maxDaysAhead", 60)
        bufferMinutes = json.int("bufferMinutes")
        slotStepMinutes = json.int("slotStepMinutes")
        dailyLimit = max(0, json.int("dailyLimit"))
        replyTo = json.str("replyTo")
        notifyEmail = json.str("notifyEmail")
        emailOwner = json.optBool("emailOwner") ?? true
        addToCalendar = json.optBool("addToCalendar") ?? true
        checkCalendar = json.optBool("checkCalendar") ?? true
        notifyVisits = json.optBool("notifyVisits") ?? true
        types = json.maps("types").map(BookingType.init(json:))
    }
}

public struct BookingAccent: Identifiable, Equatable, Hashable, Sendable {
    public var id: String
    public var hex: String

    public init(id: String, hex: String) {
        self.id = id
        self.hex = hex
    }

    public var rgb: (red: Double, green: Double, blue: Double)? {
        let digits = hex.hasPrefix("#") ? String(hex.dropFirst()) : hex
        guard digits.count == 6, let value = UInt32(digits, radix: 16) else { return nil }
        return (Double((value >> 16) & 0xFF) / 255, Double((value >> 8) & 0xFF) / 255, Double(value & 0xFF) / 255)
    }
}

public struct BookingOptions: Equatable, Sendable {
    public static let defaultAccentOrder = ["blue", "violet", "teal", "rose", "amber", "slate"]

    public var durations: [Int] = [10, 15, 20, 25, 30, 45, 50, 60, 75, 90, 120, 180, 240]
    public var steps: [Int] = [0, 5, 10, 15, 20, 30, 60]
    public var buffers: [Int] = [0, 5, 10, 15, 30, 45, 60]
    public var notices: [Int] = [0, 30, 60, 120, 240, 480, 720, 1440, 2880, 4320, 10080]
    public var horizons: [Int] = [7, 14, 21, 30, 45, 60, 90, 120, 180, 365]
    public var locations: [String] = BookingText.locationKinds
    public var accents: [BookingAccent] = [
        BookingAccent(id: "blue", hex: "#2563eb"), BookingAccent(id: "violet", hex: "#7c3aed"),
        BookingAccent(id: "teal", hex: "#0d9488"), BookingAccent(id: "rose", hex: "#e11d48"),
        BookingAccent(id: "amber", hex: "#d97706"), BookingAccent(id: "slate", hex: "#475569"),
    ]
    public var timeFormats: [String] = ["12h", "24h"]

    public init() {}

    public init(json: JSONObject) {
        func ints(_ key: String, _ fallback: [Int]) -> [Int] {
            let values = json.list(key).compactMap(\.intValue)
            return values.isEmpty ? fallback : values
        }
        let base = BookingOptions()
        durations = ints("durations", base.durations)
        steps = ints("steps", base.steps)
        buffers = ints("buffers", base.buffers)
        notices = ints("notices", base.notices)
        horizons = ints("horizons", base.horizons)
        let kinds = json.strings("locations").filter(BookingText.locationKinds.contains)
        locations = kinds.isEmpty ? base.locations : kinds
        let colours = json.obj("accents")
        if !colours.isEmpty {
            let named = colours.keys.sorted { lhs, rhs in
                let left = Self.defaultAccentOrder.firstIndex(of: lhs) ?? Int.max
                let right = Self.defaultAccentOrder.firstIndex(of: rhs) ?? Int.max
                return left == right ? lhs < rhs : left < right
            }
            accents = named.map { BookingAccent(id: $0, hex: colours.str($0)) }
        }
        let formats = json.strings("timeFormats")
        timeFormats = formats.isEmpty ? base.timeFormats : formats
    }

    public func accent(_ id: String) -> BookingAccent? { accents.first { $0.id == id } }
}

public struct BookingStats: Equatable, Sendable {
    public var upcoming = 0
    public var next7Days = 0
    public var booked30Days = 0
    public var cancelled30Days = 0
    public var bookedMinutes7Days = 0

    public init() {}

    public init(json: JSONObject) {
        upcoming = max(0, json.int("upcoming"))
        next7Days = max(0, json.int("next7Days"))
        booked30Days = max(0, json.int("booked30Days"))
        cancelled30Days = max(0, json.int("cancelled30Days"))
        bookedMinutes7Days = max(0, json.int("bookedMinutes7Days"))
    }

    public var hoursBooked: String {
        bookedMinutes7Days > 0 ? BookingText.minutesLabel(bookedMinutes7Days) + " booked" : ""
    }
}

public struct BookingOverview: Equatable, Sendable {
    public var installed: Bool
    public var actor: String
    public var suggestedSlug: String
    public var origin: String
    public var slug: String
    public var enabled: Bool
    public var link: String
    public var links: [String: String]
    public var settings: BookingSettings
    public var options: BookingOptions
    public var upcoming: [Booking]
    public var past: [Booking]
    public var stats: BookingStats
    public var activity: PageActivity

    public init(json: JSONObject) {
        installed = json.optBool("installed") ?? true
        actor = json.str("actor")
        suggestedSlug = json.str("suggestedSlug")
        origin = json.str("origin", "https://forkmesh.com").isEmpty ? "https://forkmesh.com" : json.str("origin", "https://forkmesh.com")
        slug = json.str("slug")
        enabled = json.optBool("enabled") ?? true
        link = json.str("link")
        var urls: [String: String] = [:]
        for (key, value) in json.obj("links") where !value.text.isEmpty { urls[key] = value.text }
        links = urls
        settings = BookingSettings(json: json.obj("settings"))
        options = BookingOptions(json: json.obj("options"))
        upcoming = json.maps("upcoming").map(Booking.init(json:)).filter { PackagesText.isHexId($0.id) }
        past = json.maps("past").map(Booking.init(json:)).filter { PackagesText.isHexId($0.id) }
        stats = BookingStats(json: json.obj("stats"))
        activity = PageActivity(json: json.obj("activity"))
    }

    public var displayLink: String { BookingText.displayLink(link) }

    public var linkPrefix: String { BookingText.displayLink(origin) + "/meet?" }

    public func link(for type: BookingType) -> String { links[type.id] ?? "" }

    public func find(_ id: String) -> Booking? { upcoming.first { $0.id == id } ?? past.first { $0.id == id } }
}

public struct BookingSlugCheck: Equatable, Sendable {
    public var slug: String
    public var available: Bool
    public var reason: String

    public init(slug: String, available: Bool, reason: String) {
        self.slug = slug
        self.available = available
        self.reason = reason
    }

    public init(json: JSONObject) {
        slug = json.str("slug")
        available = json.bool("available")
        reason = json.str("reason")
    }

    public var message: String {
        if available { return "Available" }
        let sentence = BookingText.errorSentence(reason)
        return sentence.isEmpty ? "Not available" : sentence
    }
}

public struct BookingExport: Sendable {
    public var filename: String
    public var json: String

    public init(filename: String, json: String) {
        self.filename = filename
        self.json = json
    }
}

public struct BookingWeekday: Identifiable, Equatable, Hashable, Sendable {
    public let id: String
    public let name: String
}

public struct BookingDay: Identifiable, Equatable, Sendable {
    public var id: String
    public var title: String
    public var bookings: [Booking]
}

public enum BookingText {
    public static let weekdays: [BookingWeekday] = [
        BookingWeekday(id: "mon", name: "Monday"), BookingWeekday(id: "tue", name: "Tuesday"),
        BookingWeekday(id: "wed", name: "Wednesday"), BookingWeekday(id: "thu", name: "Thursday"),
        BookingWeekday(id: "fri", name: "Friday"), BookingWeekday(id: "sat", name: "Saturday"),
        BookingWeekday(id: "sun", name: "Sunday"),
    ]

    public static let locationKinds = ["video", "phone", "place", "custom", "none"]

    public static let installTitle = "Create your booking page"

    public static let installMessage =
        "Pick a short link, set the hours you take meetings, and send the link to anyone. They choose a free time, the meeting goes on your ForkMesh calendar, and ForkMesh emails the details to both of you."

    public static let uninstallConfirm =
        "Uninstall Booking? Your export downloads first, then your booking page and every booking are deleted and the link stops working."

    public static let upcomingEmptyTitle = "No meetings booked yet"
    public static let upcomingEmptyMessage = "Send your link to someone. When they book, the meeting appears here and on your calendar."
    public static let pastEmptyTitle = "Nothing here yet"
    public static let pastEmptyMessage = "Meetings from the last 90 days and cancelled ones show up here."

    public static func displayLink(_ url: String) -> String {
        var text = url.trimmingCharacters(in: .whitespacesAndNewlines)
        for scheme in ["https://", "http://"] where text.lowercased().hasPrefix(scheme) {
            text = String(text.dropFirst(scheme.count))
        }
        return text
    }

    public static func minutesLabel(_ minutes: Int) -> String {
        if minutes < 60 { return "\(max(0, minutes)) min" }
        let hours = minutes / 60
        let rest = minutes % 60
        return rest == 0 ? "\(hours) h" : "\(hours) h \(rest) min"
    }

    public static func noticeLabel(_ minutes: Int) -> String {
        if minutes <= 0 { return "Any time" }
        if minutes < 60 { return "\(minutes) minutes before" }
        if minutes < 1440 {
            let hours = minutes / 60
            return hours == 1 ? "1 hour before" : "\(hours) hours before"
        }
        let days = minutes / 1440
        return days == 1 ? "1 day before" : "\(days) days before"
    }

    public static func horizonLabel(_ days: Int) -> String { "\(days) days ahead" }

    public static func bufferLabel(_ minutes: Int) -> String { minutes > 0 ? "\(minutes) minutes" : "No gap" }

    public static func stepLabel(_ minutes: Int) -> String { minutes > 0 ? "\(minutes) minutes" : "Meeting length" }

    public static func dailyLimitLabel(_ count: Int) -> String { count > 0 ? "\(count)" : "No limit" }

    public static func locationLabel(_ kind: String) -> String {
        switch kind {
        case "video": return "Video call"
        case "phone": return "Phone call"
        case "place": return "In person"
        case "custom": return "Other"
        case "none": return "No location"
        default: return kind
        }
    }

    public static func locationHint(_ kind: String) -> String {
        switch kind {
        case "video": return "Leave empty for a private ForkMesh video room"
        case "phone": return "Leave empty to ask the guest for their number"
        case "place": return "Address"
        case "custom": return "Shown after booking"
        default: return ""
        }
    }

    public static func accentName(_ id: String) -> String { id.prefix(1).uppercased() + id.dropFirst() }

    public static func errorSentence(_ code: String) -> String {
        switch code {
        case "slug_taken": return "That link is taken. Try another."
        case "invalid_slug": return "Links use lowercase letters, numbers and dashes."
        case "slug_reserved": return "That link is reserved. Try another."
        case "meeting_type_required": return "Keep at least one meeting type."
        case "invalid_meeting_url": return "A video call link must start with https://."
        case "invalid_email": return "Enter a valid email address."
        case "booking_locked": return "That meeting already happened or was cancelled."
        default: return ""
        }
    }

    public static func describe(_ error: Error) -> String {
        if let error = error as? PackagesError {
            let sentence = errorSentence(error.code)
            return sentence.isEmpty ? error.message : sentence
        }
        return error.localizedDescription
    }

    public static func isSlug(_ value: String) -> Bool {
        let chars = Array(value)
        guard (1...63).contains(chars.count) else { return false }
        let allowed = chars.allSatisfy { ($0.isASCII && ($0.isLowercase || $0.isNumber)) || $0 == "-" }
        return allowed && chars.first != "-" && chars.last != "-"
    }

    public static func clock(_ millis: Int64, timeFormat: String, timeZone: TimeZone, locale: Locale = .current) -> String {
        let formatter = DateFormatter()
        formatter.locale = locale
        formatter.timeZone = timeZone
        formatter.dateFormat = timeFormat == "24h" ? "HH:mm" : "h:mm a"
        return formatter.string(from: Date(timeIntervalSince1970: Double(millis) / 1000))
    }

    public static func timeRange(_ booking: Booking, timeFormat: String, timeZone: TimeZone, locale: Locale = .current) -> String {
        clock(booking.startAt, timeFormat: timeFormat, timeZone: timeZone, locale: locale) + " – "
            + clock(booking.endAt, timeFormat: timeFormat, timeZone: timeZone, locale: locale)
    }

    public static func dayTitle(_ millis: Int64, timeZone: TimeZone, locale: Locale = .current) -> String {
        let formatter = DateFormatter()
        formatter.locale = locale
        formatter.timeZone = timeZone
        formatter.dateFormat = "EEEE, MMMM d"
        return formatter.string(from: Date(timeIntervalSince1970: Double(millis) / 1000))
    }

    public static func days(_ bookings: [Booking], timeZone: TimeZone, locale: Locale = .current) -> [BookingDay] {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        var out: [BookingDay] = []
        for booking in bookings {
            let parts = calendar.dateComponents([.year, .month, .day], from: Date(timeIntervalSince1970: Double(booking.startAt) / 1000))
            let key = String(format: "%04d-%02d-%02d", parts.year ?? 0, parts.month ?? 0, parts.day ?? 0)
            if let last = out.indices.last, out[last].id == key {
                out[last].bookings.append(booking)
            } else {
                out.append(BookingDay(id: key, title: dayTitle(booking.startAt, timeZone: timeZone, locale: locale), bookings: [booking]))
            }
        }
        return out
    }
}

public struct BookingDraft: Equatable, Sendable {
    public var slug: String
    public var displayName: String
    public var headline: String
    public var welcome: String
    public var timezone: String
    public var timeFormat: String
    public var accent: String
    public var availability: [String: [BookingWindow]]
    public var minNoticeMinutes: Int
    public var maxDaysAhead: Int
    public var bufferMinutes: Int
    public var slotStepMinutes: Int
    public var dailyLimit: Int
    public var replyTo: String
    public var notifyEmail: String
    public var emailOwner: Bool
    public var addToCalendar: Bool
    public var checkCalendar: Bool
    public var notifyVisits: Bool
    public var types: [BookingType]

    public static let lastMinute = "23:59"

    public init(overview: BookingOverview) {
        let settings = overview.settings
        slug = overview.slug
        displayName = settings.displayName
        headline = settings.headline
        welcome = settings.welcome
        timezone = settings.timezone
        timeFormat = settings.timeFormat
        accent = settings.accent
        var week: [String: [BookingWindow]] = [:]
        for weekday in BookingText.weekdays {
            let day = weekday.id
            week[day] = (settings.availability[day] ?? []).map {
                BookingWindow(start: $0.start, end: $0.end == "24:00" ? Self.lastMinute : $0.end)
            }
        }
        availability = week
        minNoticeMinutes = settings.minNoticeMinutes
        maxDaysAhead = settings.maxDaysAhead
        bufferMinutes = settings.bufferMinutes
        slotStepMinutes = settings.slotStepMinutes
        dailyLimit = settings.dailyLimit
        replyTo = settings.replyTo
        notifyEmail = settings.notifyEmail
        emailOwner = settings.emailOwner
        addToCalendar = settings.addToCalendar
        checkCalendar = settings.checkCalendar
        notifyVisits = settings.notifyVisits
        types = settings.types
    }

    public func windows(_ day: String) -> [BookingWindow] { availability[day] ?? [] }

    public func isOn(_ day: String) -> Bool { !windows(day).isEmpty }

    public mutating func setDay(_ day: String, on: Bool) {
        availability[day] = on ? [BookingWindow(start: "09:00", end: "17:00")] : []
    }

    public mutating func addWindow(_ day: String) {
        availability[day, default: []].append(BookingWindow(start: "13:00", end: "17:00"))
    }

    public mutating func removeWindow(_ day: String, at index: Int) {
        guard var list = availability[day], list.indices.contains(index) else { return }
        list.remove(at: index)
        availability[day] = list
    }

    public mutating func setWindow(_ day: String, at index: Int, start: String? = nil, end: String? = nil) {
        guard var list = availability[day], list.indices.contains(index) else { return }
        if let start { list[index].start = start }
        if let end { list[index].end = end }
        availability[day] = list
    }

    public mutating func addType() {
        types.append(BookingType())
    }

    public mutating func removeType(at index: Int) {
        guard types.indices.contains(index) else { return }
        types.remove(at: index)
    }

    public static func minutes(_ clock: String) -> Int? {
        let parts = clock.split(separator: ":")
        guard parts.count == 2, let hours = Int(parts[0]), let minutes = Int(parts[1]),
            (0...24).contains(hours), (0...59).contains(minutes)
        else { return nil }
        return hours * 60 + minutes
    }

    public static func clock(_ minutes: Int) -> String {
        String(format: "%02d:%02d", minutes / 60, minutes % 60)
    }

    public var availabilityBody: JSONObject {
        var week = JSONObject()
        for weekday in BookingText.weekdays {
            let day = weekday.id
            let kept: [JSONValue] = windows(day).compactMap { window in
                let end = window.end == Self.lastMinute ? "24:00" : window.end
                guard let from = Self.minutes(window.start), let until = Self.minutes(end), until > from else { return nil }
                return .array([.string(window.start), .string(end)])
            }
            week[day] = .array(kept)
        }
        return week
    }

    public var body: JSONObject {
        let trim: (String) -> String = { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
        return [
            "slug": .string(trim(slug).lowercased()),
            "displayName": .string(trim(displayName)),
            "headline": .string(trim(headline)),
            "welcome": .string(welcome),
            "timezone": .string(timezone),
            "timeFormat": .string(timeFormat),
            "accent": .string(accent),
            "availability": .object(availabilityBody),
            "minNoticeMinutes": .int(Int64(minNoticeMinutes)),
            "maxDaysAhead": .int(Int64(maxDaysAhead)),
            "bufferMinutes": .int(Int64(bufferMinutes)),
            "slotStepMinutes": .int(Int64(slotStepMinutes)),
            "dailyLimit": .int(Int64(max(0, dailyLimit))),
            "replyTo": .string(trim(replyTo)),
            "notifyEmail": .string(trim(notifyEmail)),
            "emailOwner": .bool(emailOwner),
            "addToCalendar": .bool(addToCalendar),
            "checkCalendar": .bool(checkCalendar),
            "notifyVisits": .bool(notifyVisits),
            "types": .array(types.map { .object($0.body) }),
        ]
    }
}
