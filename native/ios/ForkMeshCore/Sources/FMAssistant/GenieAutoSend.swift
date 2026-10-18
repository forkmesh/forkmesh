import FMBase
import FMLocalAI
import FMPackages
import Foundation

public struct GenieAutoSendReceipt: Equatable, Sendable, Identifiable {
    public var id: String
    public var organization: String
    public var contactName: String
    public var to: String
    public var fromEmail: String
    public var subject: String
    public var text: String
    public var via: String
    public var model: String
    public var sentToday: Int
    public var dailyLimit: Int

    public init(json: JSONObject, pick: GenieOutreachPick) {
        let email = json.obj("email")
        id = email.str("id")
        organization = pick.organization
        contactName = pick.contact.displayName
        to = email.str("to", pick.contact.email)
        fromEmail = json.str("fromEmail", email.str("fromEmail"))
        subject = json.str("subject", email.str("subject"))
        text = json.str("text", email.str("text"))
        via = json.str("via")
        model = json.str("model")
        sentToday = json.int("sentToday")
        dailyLimit = json.int("dailyLimit")
    }

    public var viaLabel: String { via == "zoho" ? "Zoho Mail" : via == "mailtrap" ? "ForkMesh mail" : via }
}

public enum GenieAutoSendStage: Equatable, Sendable {
    case finding
    case writing
    case sending
    case sent
}

public enum GenieAutoSendOutcome: Equatable, Sendable {
    case sent(GenieAutoSendReceipt)
    case skip(String)
    case pause(String)
}

public final class GenieAutoSendPace {
    public nonisolated static let onKey = "shell/auto_send"
    public nonisolated static let dayKey = "shell/auto_send_day"
    public nonisolated static let countKey = "shell/auto_send_count"
    public nonisolated static let lastKey = "shell/auto_send_last"
    public nonisolated static let perDay = 100
    public nonisolated static let every: TimeInterval = 24 * 60 * 60 / Double(perDay)

    private let store: KeyValueStore?
    private let calendar: Calendar
    public private(set) var on: Bool
    public private(set) var last: Date
    private var day: String
    private var count: Int

    public init(store: KeyValueStore?, calendar: Calendar = .current) {
        self.store = store
        self.calendar = calendar
        on = store?.bool(forKey: GenieAutoSendPace.onKey) ?? false
        last = Date(timeIntervalSince1970: store?.double(forKey: GenieAutoSendPace.lastKey) ?? 0)
        day = store?.string(forKey: GenieAutoSendPace.dayKey) ?? ""
        count = store?.int(forKey: GenieAutoSendPace.countKey) ?? 0
    }

    public func setOn(_ value: Bool) {
        on = value
        store?.set(value, forKey: GenieAutoSendPace.onKey)
    }

    public func sentToday(now: Date) -> Int { day == key(now) ? count : 0 }

    public func due(now: Date) -> Bool {
        on && sentToday(now: now) < GenieAutoSendPace.perDay && now.timeIntervalSince(last) >= GenieAutoSendPace.every
    }

    public func wait(now: Date) -> TimeInterval {
        if sentToday(now: now) >= GenieAutoSendPace.perDay, let tomorrow = calendar.date(byAdding: .day, value: 1, to: calendar.startOfDay(for: now)) {
            return tomorrow.timeIntervalSince(now)
        }
        return max(0, GenieAutoSendPace.every - now.timeIntervalSince(last))
    }

    public func countdown(now: Date) -> DateInterval? {
        let wait = wait(now: now)
        guard on, wait > 0 else { return nil }
        let start = min(last, now)
        return DateInterval(start: start, end: now.addingTimeInterval(wait))
    }

    public func sent(at now: Date) {
        let today = key(now)
        count = (day == today ? count : 0) + 1
        day = today
        held(at: now)
        store?.set(today, forKey: GenieAutoSendPace.dayKey)
        store?.set(count, forKey: GenieAutoSendPace.countKey)
    }

    public func held(at now: Date) {
        last = now
        store?.set(now.timeIntervalSince1970, forKey: GenieAutoSendPace.lastKey)
    }

    private func key(_ date: Date) -> String {
        let parts = calendar.dateComponents([.year, .month, .day], from: date)
        return "\(parts.year ?? 0)-\(parts.month ?? 0)-\(parts.day ?? 0)"
    }
}

public enum GenieAutoSend {
    public static let skipped: Set<String> = ["contact_has_no_email", "contact_closed", "recently_emailed", "contact_not_found"]

    public static func outcome(_ reply: PackagesReply, pick: GenieOutreachPick) -> GenieAutoSendOutcome {
        if reply.ok { return .sent(GenieAutoSendReceipt(json: reply.json, pick: pick)) }
        let code = reply.json.str("error")
        let detail = reply.json.str("detail")
        if skipped.contains(code) { return .skip(code) }
        switch code {
        case "daily_limit_reached":
            return .pause("The CRM's limit of \(reply.json.int("dailyLimit", GenieAutoSendPace.perDay)) emails a day is reached.")
        case "outreach_forbidden":
            return .pause("Only owners, admins and the outreach team of \(pick.organization) can send outreach.")
        case "ai_unavailable", "draft_unusable":
            return .pause("Nothing could write the email: the server has no AI model and no writer on this phone gave a usable draft.")
        case "rate_limited":
            return .pause("The AI model is busy. Trying again in a while.")
        default:
            return .pause(detail.isEmpty ? PackagesError.sentence(code, status: reply.status) : detail)
        }
    }

    public static func parseReply(_ text: String) -> (subject: String, body: String)? {
        let marks = CharacterSet.whitespaces.union(CharacterSet(charactersIn: "*#_\""))
        let lines = text.trimmingCharacters(in: .whitespacesAndNewlines).components(separatedBy: "\n")
        guard let index = lines.firstIndex(where: { $0.trimmingCharacters(in: marks).lowercased().hasPrefix("subject:") }) else {
            return nil
        }
        let subject = String(lines[index].trimmingCharacters(in: marks).dropFirst(8)).trimmingCharacters(in: marks)
        let body = lines[(index + 1)...].joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
        guard !subject.isEmpty, !body.isEmpty else { return nil }
        return (subject, body)
    }

    @MainActor
    static func write(_ prompt: String, writers: [GenieOutreachWriter]) async -> (subject: String, body: String, writer: String)? {
        let messages = [LocalChatMessage(role: .user, content: prompt)]
        for writer in writers {
            guard let text = await GenieOutreach.attempt(writer, messages), let draft = parseReply(text) else { continue }
            return (draft.subject, draft.body, writer.name)
        }
        return nil
    }
}

extension ForkMeshToolbox {
    public func outreachAutoSend(
        _ pick: GenieOutreachPick, writers: [GenieOutreachWriter], stage: (GenieAutoSendStage) -> Void = { _ in }
    ) async -> GenieAutoSendOutcome {
        guard let client = sources.crm() else { return .pause("The CRM is not available in this build.") }
        var body: JSONObject = ["contactId": .string(pick.contact.id)]
        do {
            stage(.writing)
            var reply = try await client.autoCrmEmail(pick.organization, body: body)
            let code = reply.json.str("error")
            let prompt = reply.json.str("prompt")
            if code == "ai_unavailable" || code == "draft_unusable", !prompt.isEmpty,
               let draft = await GenieAutoSend.write(prompt, writers: writers) {
                body["subject"] = .string(draft.subject)
                body["text"] = .string(draft.body)
                body["model"] = .string(draft.writer)
                stage(.sending)
                reply = try await client.autoCrmEmail(pick.organization, body: body)
            }
            let outcome = GenieAutoSend.outcome(reply, pick: pick)
            if case .sent = outcome { stage(.sent) }
            return outcome
        } catch {
            return .pause(PackagesError.describe(error))
        }
    }
}
