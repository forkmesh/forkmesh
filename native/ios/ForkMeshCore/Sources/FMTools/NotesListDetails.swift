import FMBase
import Foundation

public enum NoteEditTime {
    static func formatter(_ format: String, _ calendar: Calendar) -> DateFormatter {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.calendar = calendar
        formatter.timeZone = calendar.timeZone
        formatter.dateFormat = format
        return formatter
    }

    public static func label(_ millis: Int64, nowMillis: Int64, calendar: Calendar = .current) -> String {
        guard millis > 0 else { return "" }
        let date = Date(epochMillis: millis)
        let now = Date(epochMillis: nowMillis)
        let time = formatter("h:mm a", calendar).string(from: date)
        let day = calendar.startOfDay(for: date)
        let today = calendar.startOfDay(for: now)
        if day == today { return "Today \(time)" }
        if let yesterday = calendar.date(byAdding: .day, value: -1, to: today), day == yesterday {
            return "Yesterday \(time)"
        }
        if calendar.component(.year, from: date) == calendar.component(.year, from: now) {
            return formatter("MMM d, h:mm a", calendar).string(from: date)
        }
        return formatter("MMM d, yyyy", calendar).string(from: date)
    }

    public static func line(_ millis: Int64, editedBy: String, me: String, nowMillis: Int64, calendar: Calendar = .current) -> String {
        let when = label(millis, nowMillis: nowMillis, calendar: calendar)
        let who = editedBy.dartTrimmed
        guard !who.isEmpty, who.lowercased() != me.dartTrimmed.lowercased() else { return when }
        return when.isEmpty ? "by \(who)" : "\(when) · by \(who)"
    }
}

public enum NoteReachBadge: String, Equatable, Sendable {
    case privateNote = "private"
    case shared
    case publicNote = "public"

    public var label: String {
        switch self {
        case .privateNote: return "Private"
        case .shared: return "Shared"
        case .publicNote: return "Public"
        }
    }

    public static func of(_ note: JSONObject) -> NoteReachBadge {
        if note["visibility"]?.stringValue == "public" { return .publicNote }
        return ToolsJSON.maps(note["shares"]).isEmpty ? .privateNote : .shared
    }
}

@MainActor
public final class NoteViewsSeen {
    public static let shared = NoteViewsSeen()

    private var seen: [String: Int64] = [:]

    public init() {}

    public func record(_ note: JSONObject) {
        let id = ToolsJSON.text(note["id"], "")
        guard !id.isEmpty else { return }
        record(id, updatedAt: ToolsJSON.epoch(note["updatedAt"]))
    }

    public func record(_ id: String, updatedAt: Int64) {
        seen[id] = max(seen[id] ?? 0, updatedAt)
    }

    public func unread(_ note: JSONObject) -> Bool {
        guard note["unread"]?.boolValue == true else { return false }
        guard let at = seen[ToolsJSON.text(note["id"], "")] else { return true }
        return ToolsJSON.epoch(note["updatedAt"]) > at
    }
}

public struct NoteAvatar: Equatable, Identifiable, Sendable {
    public let name: String
    public let organization: Bool

    public init(name: String, organization: Bool = false) {
        self.name = name
        self.organization = organization
    }

    public var id: String { "\(organization ? "organization" : "user"):\(name.lowercased())" }
    public var initials: String { NoteAvatar.initials(name) }

    public static func initials(_ name: String) -> String {
        let parts = name.split(whereSeparator: { !($0.isLetter || $0.isNumber) })
        let first = parts.first?.first.map(String.init) ?? "?"
        let second = parts.dropFirst().first?.first.map(String.init) ?? ""
        return (first + second).uppercased()
    }

    public static let maxShown = 3

    public static func of(_ note: JSONObject) -> (shown: [NoteAvatar], more: Int) {
        var seen = Set<String>()
        let all = ToolsJSON.maps(note["shares"]).compactMap { (share: JSONObject) -> NoteAvatar? in
            let name = ToolsJSON.text(share["name"], "").dartTrimmed
            guard !name.isEmpty else { return nil }
            let avatar = NoteAvatar(name: name, organization: share["type"]?.stringValue == "organization")
            return seen.insert(avatar.id).inserted ? avatar : nil
        }
        return (Array(all.prefix(maxShown)), max(0, all.count - maxShown))
    }
}

public enum NoteAccessHeader {
    public static func roleLine(_ role: String) -> String {
        switch role {
        case "owner", "": return "You own this note"
        case "editor": return "You can edit"
        default: return "View only"
        }
    }

    public static func shareLines(_ shares: [JSONObject]) -> [String] {
        let lines = shares.compactMap { (share: JSONObject) -> String? in
            let name = ToolsJSON.text(share["name"], "").dartTrimmed
            guard !name.isEmpty else { return nil }
            let who = share["type"]?.stringValue == "organization" ? "\(name) (organization)" : name
            return "\(who) · \(share["role"]?.stringValue == "editor" ? "Can edit" : "Can view")"
        }
        return lines.isEmpty ? ["Not shared"] : lines
    }

    public static func fullTime(_ millis: Int64, calendar: Calendar = .current) -> String {
        guard millis > 0 else { return "" }
        return NoteEditTime.formatter("EEEE, MMMM d, yyyy 'at' h:mm a", calendar).string(from: Date(epochMillis: millis))
    }

    public static func editLine(_ millis: Int64, editedBy: String, me: String, calendar: Calendar = .current) -> String {
        let when = fullTime(millis, calendar: calendar)
        guard !when.isEmpty else { return "" }
        let who = editedBy.dartTrimmed
        if who.isEmpty { return "Last edited \(when)" }
        return who.lowercased() == me.dartTrimmed.lowercased() ? "Last edited \(when) by you" : "Last edited \(when) by \(who)"
    }

    public static func lines(_ note: JSONObject, me: String, calendar: Calendar = .current) -> [String] {
        var out = [roleLine(ToolsJSON.text(note["role"], "owner"))]
        out += shareLines(ToolsJSON.maps(note["shares"]))
        out.append(NoteReachBadge.of(note).label)
        let edited = editLine(
            ToolsJSON.epoch(note["updatedAt"]), editedBy: ToolsJSON.text(note["lastEditedBy"], ""), me: me, calendar: calendar)
        if !edited.isEmpty { out.append(edited) }
        return out
    }
}
