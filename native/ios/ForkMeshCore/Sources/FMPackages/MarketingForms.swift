import FMBase
import Foundation

public enum MarketingFieldKind: Equatable, Sendable {
    case text
    case multiline
    case url
    case secret
    case choice([String])
    case channel([String])
    case channels([String])
    case date
    case dateTime
    case money
    case count
    case decimal
    case toggle
    case list
    case lines
    case tasks
}

public struct MarketingField: Equatable, Sendable, Identifiable {
    public var key: String
    public var label: String
    public var kind: MarketingFieldKind
    public var required: Bool
    public var optional: Bool
    public var hint: String

    public init(
        _ key: String, _ label: String, _ kind: MarketingFieldKind = .text, required: Bool = false, optional: Bool = false,
        hint: String = ""
    ) {
        self.key = key
        self.label = label
        self.kind = kind
        self.required = required
        self.optional = optional
        self.hint = hint
    }

    public var id: String { key }
}

public struct MarketingFormError: Error, Equatable, Sendable {
    public var message: String

    public init(_ message: String) {
        self.message = message
    }
}

public struct MarketingDraft: Equatable, Sendable {
    public var kind: MarketingKind
    public var id: String
    public var fields: [MarketingField]
    public var values: [String: String]
    public var original: JSONObject

    public init(kind: MarketingKind, id: String = "", overview: MarketingOverview, json: JSONObject = [:], zone: TimeZone = .current) {
        self.kind = kind
        self.id = id
        original = json
        fields = MarketingForms.fields(kind, overview: overview, json: json)
        values = [:]
        for field in fields { values[field.key] = MarketingForms.text(field, json: json, zone: zone) }
    }

    public var isNew: Bool { id.isEmpty }

    public subscript(key: String) -> String {
        get { values[key] ?? "" }
        set { values[key] = newValue }
    }

    public var title: String {
        switch kind {
        case .metric: return MarketingText.title(original.str("channel")) + " numbers"
        case .channel: return original.str("displayName", MarketingKind.channel.title)
        default: return (isNew ? "New " : "Edit ") + kind.title.lowercased()
        }
    }

    public func body(overview: MarketingOverview, now: Int64, zone: TimeZone = .current) throws -> JSONObject {
        var body = JSONObject()
        for field in fields {
            let raw = (values[field.key] ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            if field.required && raw.isEmpty { throw MarketingFormError("\(field.label) is required.") }
            if field.key == "currency" {
                if !raw.isEmpty { body["currency"] = .string(raw.uppercased()) }
                continue
            }
            if let value = try MarketingForms.value(field, raw, original: original, zone: zone) {
                if field.key.hasPrefix("snapshot.") {
                    var snapshot = body.obj("snapshot")
                    snapshot[String(field.key.dropFirst("snapshot.".count))] = value
                    body["snapshot"] = .object(snapshot)
                } else {
                    body[field.key] = value
                }
            }
        }
        return try MarketingForms.shape(kind, body: body, draft: self, overview: overview, now: now)
    }
}

public enum MarketingForms {
    public static let contentStatuses = MarketingContent.statuses
    public static let campaignObjectives = ["awareness", "traffic", "signups", "activation", "engagement", "launch", "community", "sales"]
    public static let adObjectives = ["awareness", "traffic", "engagement", "video-views", "leads", "app-installs", "sales"]
    public static let experimentStages = ["acquisition", "activation", "retention", "referral", "revenue", "measurement"]
    public static let experimentDecisions = ["", "keep", "iterate", "revert", "extend"]
    public static let keywordIntents = ["", "informational", "navigational", "commercial", "transactional"]
    public static let funnelRates = [
        ("signupRate", "Signup rate"), ("activationRate", "Activation rate"), ("retentionRate", "Retention rate"),
        ("referralRate", "Referral share"),
    ]
    public static let goalOperators = ["exact", "icontains"]

    public static func fields(_ kind: MarketingKind, overview: MarketingOverview, json: JSONObject = [:]) -> [MarketingField] {
        let configured = overview.channelSettings.filter(\.enabled).map(\.id)
        let channels = configured + overview.catalog.map(\.id).filter { !configured.contains($0) }
        switch kind {
        case .content:
            return [
                MarketingField("title", "Title", required: true),
                MarketingField("channel", "Channel", .channel(channels)),
                MarketingField("status", "Status", .choice(kind.statuses)),
                MarketingField("campaign", "Campaign", hint: "Campaign name"),
                MarketingField("scheduledAt", "Scheduled", .dateTime),
                MarketingField("owner", "Owner"),
                MarketingField("url", "Published link", .url),
                MarketingField("tags", "Tags", .list, hint: "Comma separated"),
                MarketingField("body", "Copy", .multiline),
                MarketingField("notes", "Notes", .multiline),
            ]
        case .campaign:
            return [
                MarketingField("name", "Name", required: true),
                MarketingField("status", "Status", .choice(kind.statuses)),
                MarketingField("objective", "Objective", .choice(campaignObjectives)),
                MarketingField("startsAt", "Starts", .date),
                MarketingField("endsAt", "Ends", .date),
                MarketingField("owner", "Owner"),
                MarketingField("audience", "Audience", .multiline),
                MarketingField("message", "Message", .multiline),
                MarketingField("offer", "Offer"),
                MarketingField("landingUrl", "Landing page", .url),
                MarketingField("slug", "Link tag (utm_campaign)", hint: "Made from the name when empty"),
                MarketingField("channels", "Channels", .channels(channels)),
                MarketingField("hashtags", "Hashtags", .list, hint: "Comma separated"),
                MarketingField("keywords", "Keywords", .list, hint: "Comma separated"),
                MarketingField("currency", "Currency", hint: "USD"),
                MarketingField("budgetCents", "Budget", .money),
                MarketingField("targetVisits", "Target visits", .count, optional: true),
                MarketingField("targetConversions", "Target conversions", .count, optional: true),
                MarketingField("tasks", "Tasks", .tasks, hint: "One per line; start a line with [x] when done"),
                MarketingField("brief", "Brief", .multiline),
                MarketingField("notes", "Notes", .multiline),
            ]
        case .ad:
            let platforms = overview.adPlatforms.map(\.id)
            return [
                MarketingField("name", "Name", required: true),
                MarketingField("channel", "Platform", .channel(platforms.isEmpty ? channels : platforms)),
                MarketingField("status", "Status", .choice(kind.statuses)),
                MarketingField("objective", "Objective", .choice(adObjectives)),
                MarketingField("campaign", "Campaign"),
                MarketingField("headline", "Headline"),
                MarketingField("body", "Ad copy", .multiline),
                MarketingField("audience", "Audience", .multiline),
                MarketingField("destinationUrl", "Destination link", .url),
                MarketingField("managerUrl", "Ads manager link", .url),
                MarketingField("budgetType", "Budget type", .choice(["lifetime", "daily"])),
                MarketingField("currency", "Currency", hint: "USD"),
                MarketingField("budgetCents", "Budget", .money),
                MarketingField("startsAt", "Starts", .date),
                MarketingField("endsAt", "Ends", .date),
                MarketingField("owner", "Owner"),
                MarketingField("tags", "Tags", .list, hint: "Comma separated"),
                MarketingField("notes", "Notes", .multiline),
            ]
        case .adResults:
            return [
                MarketingField("spendCents", "Spend", .money),
                MarketingField("impressions", "Impressions", .count),
                MarketingField("clicks", "Clicks", .count),
                MarketingField("conversions", "Conversions", .count),
                MarketingField("revenueCents", "Revenue", .money),
            ]
        case .experiment:
            let scores = ["", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10"]
            return [
                MarketingField("title", "Title", required: true),
                MarketingField("status", "Status", .choice(kind.statuses)),
                MarketingField("stage", "Funnel stage", .choice(experimentStages)),
                MarketingField("hypothesis", "Hypothesis", .multiline, hint: "If we … then … because …"),
                MarketingField("audience", "Audience"),
                MarketingField("metric", "Metric"),
                MarketingField("baseline", "Baseline"),
                MarketingField("target", "Target"),
                MarketingField("impact", "Impact", .choice(scores)),
                MarketingField("confidence", "Confidence", .choice(scores)),
                MarketingField("ease", "Ease", .choice(scores)),
                MarketingField("startsAt", "Starts", .date),
                MarketingField("endsAt", "Ends", .date),
                MarketingField("reviewAt", "Review on", .date),
                MarketingField("owner", "Owner"),
                MarketingField("campaign", "Campaign"),
                MarketingField("result", "Result", .multiline),
                MarketingField("decision", "Decision", .choice(experimentDecisions)),
                MarketingField("notes", "Notes", .multiline),
            ]
        case .funnelWeek:
            return [
                MarketingField("weekStart", "Week of", .date, required: true),
                MarketingField("visits", "Visits", .count, optional: true),
                MarketingField("signups", "Signups", .count, optional: true),
                MarketingField("activated", "Activated", .count, optional: true),
                MarketingField("retainedBase", "Retention base", .count, optional: true, hint: "Signups old enough to retain"),
                MarketingField("retained", "Retained", .count, optional: true),
                MarketingField("referrals", "Referrals", .count, optional: true),
                MarketingField("northStar", "North star", .count, optional: true),
                MarketingField("revenueCents", "Revenue", .money, optional: true),
                MarketingField("currency", "Currency", hint: "USD"),
                MarketingField("source", "Source", hint: "Where the numbers came from"),
                MarketingField("note", "Note", .multiline),
            ]
        case .keyword:
            return [
                MarketingField("term", "Search phrase", required: true),
                MarketingField("intent", "Intent", .choice(keywordIntents)),
                MarketingField("priority", "Priority", .choice(["0", "1", "2", "3"])),
                MarketingField("targetUrl", "Page that should rank", .url),
                MarketingField("campaign", "Campaign"),
                MarketingField("notes", "Notes", .multiline),
                MarketingField("snapshot.position", "New reading: position", .decimal, optional: true),
                MarketingField("snapshot.impressions", "New reading: impressions", .count, optional: true),
                MarketingField("snapshot.clicks", "New reading: clicks", .count, optional: true),
            ]
        case .redditThread:
            return [
                MarketingField("url", "Thread link", .url, required: true, hint: "https://www.reddit.com/r/…/comments/…"),
                MarketingField("title", "Title"),
                MarketingField("status", "Status", .choice(kind.statuses)),
                MarketingField("landingPath", "Landing path", hint: "/ or /pricing"),
                MarketingField("rulesChecked", "I read the subreddit rules", .toggle),
                MarketingField("disclosed", "My reply discloses who I work for", .toggle),
                MarketingField("replyUrl", "My reply link", .url),
                MarketingField("monitored", "Monitor this thread", .toggle),
                MarketingField("commentUrl", "Comment to monitor", .url),
                MarketingField("note", "Note", .multiline),
            ]
        case .post:
            return [
                MarketingField("url", "Link", .url, required: true),
                MarketingField("channel", "Channel", .channel([""] + channels), hint: "Found from the link when empty"),
                MarketingField("title", "Title"),
                MarketingField("postedAt", "Posted", .date),
                MarketingField("views", "Views", .count, optional: true),
                MarketingField("likes", "Likes", .count, optional: true),
                MarketingField("replies", "Replies", .count, optional: true),
                MarketingField("reposts", "Reposts", .count, optional: true),
                MarketingField("note", "Note", .multiline),
            ]
        case .metric:
            let channel = json.str("channel")
            var result: [MarketingField] = []
            switch channel {
            case "google-search":
                result = [MarketingField("visits", "Visits from Google (30 days)", .count),
                          MarketingField("indexedPages", "Pages on Google", .count)]
            case "email":
                result = [MarketingField("outbound", "Emails sent (30 days)", .count),
                          MarketingField("replies", "Replies", .count),
                          MarketingField("closedDeals", "Closed deals", .count),
                          MarketingField("closedValue", "Closed value", .count)]
            case "github":
                result = [MarketingField("stars", "Stars", .count), MarketingField("forks", "Forks", .count),
                          MarketingField("followers", "Followers", .count)]
            default:
                result = [MarketingField("followers", "Followers", .count), MarketingField("posts", "Posts", .count)]
            }
            return result + [
                MarketingField("reach", "Reach", .count),
                MarketingField("impressions", "Impressions", .count),
                MarketingField("engagements", "Engagements", .count),
                MarketingField("clicks", "Clicks", .count),
                MarketingField("views", "Views", .count),
                MarketingField("rating", "Rating (0–5)", .decimal, optional: true),
                MarketingField("note", "Note"),
            ]
        case .channel:
            var result = [
                MarketingField("enabled", "Track this channel", .toggle),
                MarketingField("displayName", "Name"),
                MarketingField("profileUrl", "Channel link", .url),
            ]
            if MarketingChannelSetting.tokenChannels.contains(json.str("id")) {
                result.append(MarketingField(
                    "accessToken", json.str("id") == "x" ? "X API bearer token" : "GitHub access token", .secret,
                    hint: json.bool("hasAccessToken") ? "Saved; type a new one to replace it" : "Write-only"))
                if json.bool("hasAccessToken") { result.append(MarketingField("clearAccessToken", "Remove the saved token", .toggle)) }
            }
            return result
        case .attribution:
            return [
                MarketingField("landingUrl", "Default landing page", .url),
                MarketingField("sources", "Allowed utm_source values", .list),
                MarketingField("mediums", "Allowed utm_medium values", .list),
                MarketingField("campaigns", "Allowed utm_campaign values", .list),
                MarketingField("domains", "Your domains", .list, hint: "Links elsewhere are not checked"),
                MarketingField("untrackedPaths", "Paths without analytics", .list, hint: "/docs, /app"),
            ]
        case .targets:
            return funnelRates.map { MarketingField($0.0, $0.1 + " target (%)", .decimal, optional: true) }
        case .goals:
            return [
                MarketingField("project", "Analytics project id", hint: "Empty uses the organization's default"),
                MarketingField("host", "Site host", hint: "forkmesh.com"),
                MarketingField("goals", "Goals", .lines, hint: "One per line: label | event | property | exact or icontains | value"),
            ]
        case .redditWatch:
            return [
                MarketingField("subreddits", "Subreddits", .list, hint: "Comma separated, without r/"),
                MarketingField("queries", "Searches", .lines, hint: "One per line"),
            ]
        case .xConnection:
            return [
                MarketingField("apiKey", "API key", .secret, required: true),
                MarketingField("apiSecret", "API key secret", .secret, required: true),
                MarketingField("accessToken", "Access token", .secret, required: true),
                MarketingField("accessSecret", "Access token secret", .secret, required: true),
            ]
        case .xPost:
            return [MarketingField("text", "Post", .multiline, required: true, hint: "Up to 280 characters on most accounts")]
        case .link:
            return [
                MarketingField("url", "Page", .url, hint: "Empty uses the default landing page"),
                MarketingField("source", "Source", hint: "x, reddit, newsletter"),
                MarketingField("medium", "Medium", hint: "social, email, cpc"),
                MarketingField("campaign", "Campaign"),
                MarketingField("content", "Content"),
                MarketingField("term", "Term"),
            ]
        }
    }

    public static func text(_ field: MarketingField, json: JSONObject, zone: TimeZone) -> String {
        if field.key.hasPrefix("snapshot.") { return "" }
        let value = json[field.key]
        switch field.kind {
        case .secret:
            return ""
        case .toggle:
            if field.key == "enabled" { return (value?.boolValue ?? true) ? "true" : "" }
            return value?.boolValue == true ? "true" : ""
        case .channels, .list:
            return (value?.strings ?? []).joined(separator: ", ")
        case .lines:
            if field.key == "goals" {
                return json.maps("goals").map { goal in
                    var parts = [goal.str("label"), goal.str("event")]
                    if !goal.str("property").isEmpty { parts += [goal.str("property"), goal.str("operator", "exact"), goal.str("value")] }
                    return parts.joined(separator: " | ")
                }.joined(separator: "\n")
            }
            return (value?.strings ?? []).joined(separator: "\n")
        case .tasks:
            return json.maps(field.key).map { ($0.bool("done") ? "[x] " : "") + $0.str("title") }.joined(separator: "\n")
        case .date, .dateTime:
            let ms = value?.int64Value ?? 0
            return ms > 0 ? MarketingDates.text(ms, time: field.kind == .dateTime, zone: zone) : ""
        case .money:
            guard let cents = value?.int64Value, cents != 0 else { return "" }
            return MoneyText.cents(cents, "")
        case .count:
            guard let number = value?.int64Value else { return "" }
            return field.optional || number != 0 ? String(number) : ""
        case .decimal:
            guard let number = value?.doubleValue, number != 0 else { return "" }
            return MarketingText.trimmed(number)
        case .choice(let options):
            let current = value?.text ?? ""
            if field.key == "priority" || ["impact", "confidence", "ease"].contains(field.key) {
                let number = value?.int64Value ?? 0
                return number == 0 && options.contains("") ? "" : String(number)
            }
            return current.isEmpty ? defaultChoice(field, options) : current
        case .channel(let options):
            let current = value?.text ?? ""
            return current.isEmpty ? (options.contains("") ? "" : options.first ?? "") : current
        case .text, .multiline, .url:
            return value?.text ?? ""
        }
    }

    static func defaultChoice(_ field: MarketingField, _ options: [String]) -> String {
        switch field.key {
        case "status": return options.first ?? ""
        case "budgetType": return "lifetime"
        case "objective": return "awareness"
        case "stage": return "acquisition"
        default: return options.first ?? ""
        }
    }

    static func value(_ field: MarketingField, _ raw: String, original: JSONObject, zone: TimeZone) throws -> JSONValue? {
        switch field.kind {
        case .text, .multiline:
            return .string(raw)
        case .url:
            if !raw.isEmpty && !raw.lowercased().hasPrefix("https://") {
                throw MarketingFormError("\(field.label) must start with https://.")
            }
            return .string(raw)
        case .secret:
            return raw.isEmpty ? nil : .string(raw)
        case .choice:
            if field.key == "priority" || ["impact", "confidence", "ease"].contains(field.key) { return .int(Int64(raw) ?? 0) }
            return .string(raw)
        case .channel:
            return .string(raw)
        case .channels, .list:
            return .array(splitList(raw).map { .string($0) })
        case .lines:
            if field.key == "goals" { return .array(try goals(raw)) }
            return .array(raw.split(whereSeparator: \.isNewline).map { $0.trimmingCharacters(in: .whitespaces) }
                .filter { !$0.isEmpty }.map { .string($0) })
        case .tasks:
            let previous = original.maps(field.key)
            return .array(tasks(raw, previous: previous).map(\.json))
        case .date, .dateTime:
            if raw.isEmpty { return field.optional ? .null : .int(0) }
            guard let ms = MarketingDates.parse(raw, zone: zone) else {
                throw MarketingFormError("Write \(field.label.lowercased()) as YYYY-MM-DD" + (field.kind == .dateTime ? " or YYYY-MM-DD HH:MM." : "."))
            }
            return .int(ms)
        case .money:
            if raw.isEmpty { return field.optional ? .null : .int(0) }
            guard let cents = MarketingMoney.cents(raw) else { throw MarketingFormError("Write \(field.label.lowercased()) as an amount like 12.50.") }
            return .int(cents)
        case .count:
            if raw.isEmpty { return field.optional ? .null : .int(0) }
            guard let number = Int64(raw.replacingOccurrences(of: ",", with: "").replacingOccurrences(of: " ", with: "")), number >= 0 else {
                throw MarketingFormError("\(field.label) must be a whole number.")
            }
            return .int(number)
        case .decimal:
            if raw.isEmpty { return field.optional ? .null : .int(0) }
            guard let number = Double(raw.replacingOccurrences(of: ",", with: ".")), number >= 0 else {
                throw MarketingFormError("\(field.label) must be a number.")
            }
            return .double(number)
        case .toggle:
            return .bool(raw == "true")
        }
    }

    public static func splitList(_ raw: String) -> [String] {
        var seen: [String] = []
        for part in raw.split(whereSeparator: { $0 == "," || $0 == ";" || $0.isNewline }) {
            let item = part.trimmingCharacters(in: .whitespaces)
            if !item.isEmpty && !seen.contains(item) { seen.append(item) }
        }
        return seen
    }

    public static func tasks(_ raw: String, previous: [JSONObject] = []) -> [MarketingTask] {
        raw.split(whereSeparator: \.isNewline).compactMap { line in
            var text = line.trimmingCharacters(in: .whitespaces)
            var done = false
            if text.lowercased().hasPrefix("[x]") {
                done = true
                text = String(text.dropFirst(3)).trimmingCharacters(in: .whitespaces)
            } else if text.hasPrefix("[ ]") {
                text = String(text.dropFirst(3)).trimmingCharacters(in: .whitespaces)
            }
            if text.isEmpty { return nil }
            let owner = previous.first { $0.str("title") == text }?.str("owner") ?? ""
            return MarketingTask(title: text, done: done, owner: owner)
        }
    }

    static func goals(_ raw: String) throws -> [JSONValue] {
        var result: [JSONValue] = []
        for line in raw.split(whereSeparator: \.isNewline) {
            let parts = line.split(separator: "|", omittingEmptySubsequences: false).map { $0.trimmingCharacters(in: .whitespaces) }
            if parts.allSatisfy(\.isEmpty) { continue }
            let label = parts[0]
            let event = parts.count > 1 ? parts[1] : parts[0]
            if event.isEmpty { throw MarketingFormError("Each goal needs an event name.") }
            var goal: JSONObject = ["label": .string(label.isEmpty ? event : label), "event": .string(event)]
            if parts.count > 2 && !parts[2].isEmpty {
                let op = parts.count > 3 && goalOperators.contains(parts[3]) ? parts[3] : "exact"
                goal["property"] = .string(parts[2])
                goal["operator"] = .string(op)
                goal["value"] = .string(parts.count > 4 ? parts[4] : "")
            }
            result.append(.object(goal))
        }
        if result.count > 5 { throw MarketingFormError("Up to 5 goals.") }
        return result
    }

    static func shape(_ kind: MarketingKind, body: JSONObject, draft: MarketingDraft, overview: MarketingOverview, now: Int64)
        throws -> JSONObject
    {
        var body = body
        switch kind {
        case .content:
            if body.str("status") == "published" && draft.original.int64("publishedAt") == 0 { body["publishedAt"] = .int(now) }
            return body
        case .keyword:
            if body.obj("snapshot").values.allSatisfy({ $0 == .null }) {
                body["snapshot"] = nil
            } else {
                var snapshot = body.obj("snapshot")
                snapshot["at"] = .int(now)
                snapshot["source"] = "mobile"
                body["snapshot"] = .object(snapshot)
            }
            return body
        case .post:
            for key in ["views", "likes", "replies", "reposts"] where body[key] == .null { body[key] = nil }
            if body.str("channel").isEmpty { body["channel"] = nil }
            if body.int64("postedAt") == 0 && draft.original.int64("postedAt") == 0 { body["postedAt"] = nil }
            return body
        case .metric:
            body["channel"] = .string(draft.original.str("channel"))
            if body["rating"] == .null { body["rating"] = .int(0) }
            return body
        case .channel:
            let id = draft.original.str("id")
            var entry: JSONObject = ["id": .string(id), "enabled": body["enabled"] ?? true,
                                     "displayName": body["displayName"] ?? "", "profileUrl": body["profileUrl"] ?? ""]
            if let token = body["accessToken"] { entry["accessToken"] = token }
            if body["clearAccessToken"]?.boolValue == true { entry["clearAccessToken"] = true }
            return MarketingForms.channelsBody(overview, replacing: entry)
        case .attribution:
            return ["attribution": .object(body)]
        case .targets:
            var targets = JSONObject()
            for (key, value) in body where value != .null && (value.doubleValue ?? 0) > 0 {
                if (value.doubleValue ?? 0) > 100 { throw MarketingFormError("Targets are percentages up to 100.") }
                targets[key] = value
            }
            return ["targets": .object(targets)]
        case .goals:
            return ["analytics": .object(body)]
        case .redditWatch:
            return ["reddit": .object(body)]
        case .xPost:
            if let content = draft.original["contentId"] { body["contentId"] = content }
            return body
        default:
            return body
        }
    }

    public static func channelsBody(_ overview: MarketingOverview, replacing entry: JSONObject? = nil, adding: [String] = [])
        -> JSONObject
    {
        var channels = overview.channelSettings.map(\.payload)
        if let entry, let index = channels.firstIndex(where: { $0.str("id") == entry.str("id") }) {
            channels[index] = entry
        } else if let entry {
            channels.append(entry)
        }
        for id in adding where !channels.contains(where: { $0.str("id") == id }) {
            channels.append(["id": .string(id), "enabled": true, "displayName": .string(overview.channelName(id)), "profileUrl": ""])
        }
        return ["channels": .array(channels.map { .object($0) })]
    }

    public static func contentDraft(_ overview: MarketingOverview, campaign: String = "", channel: String = "", title: String = "",
                                    body: String = "", scheduledAt: Int64 = 0, zone: TimeZone = .current) -> MarketingDraft
    {
        var json: JSONObject = ["title": .string(title), "body": .string(body), "campaign": .string(campaign)]
        if !channel.isEmpty { json["channel"] = .string(channel) }
        if scheduledAt > 0 { json["scheduledAt"] = .int(scheduledAt) }
        return MarketingDraft(kind: .content, overview: overview, json: json, zone: zone)
    }

    public static func templateDraft(_ template: JSONObject, overview: MarketingOverview, zone: TimeZone = .current) -> MarketingDraft {
        var json = template
        json["key"] = nil
        json["label"] = nil
        return MarketingDraft(kind: .campaign, overview: overview, json: json, zone: zone)
    }

    public static func metricDraft(_ overview: MarketingOverview, channel: String, zone: TimeZone = .current) -> MarketingDraft {
        var json: JSONObject = ["channel": .string(channel)]
        if let stats = overview.stats(channel) {
            for (key, value) in [("followers", stats.followers), ("reach", stats.reach), ("impressions", stats.impressions),
                                 ("engagements", stats.engagements), ("clicks", stats.clicks), ("views", stats.views),
                                 ("posts", stats.posts), ("visits", stats.visits), ("indexedPages", stats.indexedPages),
                                 ("outbound", stats.outbound), ("replies", stats.replies), ("closedDeals", stats.closedDeals),
                                 ("closedValue", stats.closedValue), ("stars", stats.stars), ("forks", stats.forks)]
            {
                json[key] = .int(value)
            }
            if stats.rating > 0 { json["rating"] = .double(stats.rating) }
        }
        return MarketingDraft(kind: .metric, overview: overview, json: json, zone: zone)
    }

    public static func channelDraft(_ overview: MarketingOverview, channel: String, zone: TimeZone = .current) -> MarketingDraft {
        let setting = overview.setting(channel)
        let json: JSONObject = [
            "id": .string(channel), "enabled": .bool(setting?.enabled ?? true),
            "displayName": .string(setting?.displayName ?? overview.channelName(channel)),
            "profileUrl": .string(setting?.profileUrl ?? ""), "hasAccessToken": .bool(setting?.hasAccessToken ?? false),
        ]
        return MarketingDraft(kind: .channel, overview: overview, json: json, zone: zone)
    }

    public static func settingsDraft(_ kind: MarketingKind, overview: MarketingOverview, zone: TimeZone = .current) -> MarketingDraft {
        switch kind {
        case .attribution: return MarketingDraft(kind: kind, overview: overview, json: overview.settings.obj("attribution"), zone: zone)
        case .targets: return MarketingDraft(kind: kind, overview: overview, json: overview.settings.obj("targets"), zone: zone)
        case .goals: return MarketingDraft(kind: kind, overview: overview, json: overview.settings.obj("analytics"), zone: zone)
        case .redditWatch:
            return MarketingDraft(kind: kind, overview: overview, json: [
                "subreddits": .array(overview.redditSubreddits.map { .string($0.name) }),
                "queries": .array(overview.redditQueries.map { .string($0.name) }),
            ], zone: zone)
        default: return MarketingDraft(kind: kind, overview: overview, zone: zone)
        }
    }

    public static func xPostDraft(_ overview: MarketingOverview, content: MarketingContent? = nil) -> MarketingDraft {
        var json: JSONObject = [:]
        if let content {
            json["text"] = .string(content.body.isEmpty ? content.title : content.body)
            json["contentId"] = .string(content.id)
        }
        return MarketingDraft(kind: .xPost, overview: overview, json: json)
    }

    public static func ideaDraft(_ idea: MarketingIdea, overview: MarketingOverview, campaign: MarketingRecord?, now: Int64,
                                 zone: TimeZone = .current) -> MarketingDraft?
    {
        switch idea.kind {
        case "content":
            return contentDraft(overview, campaign: campaign?.title ?? "", channel: idea.channel, title: idea.title,
                                body: idea.copy.isEmpty ? idea.detail : idea.copy,
                                scheduledAt: idea.inDays > 0 ? now + Int64(idea.inDays) * 86_400_000 : 0, zone: zone)
        case "keyword":
            return MarketingDraft(kind: .keyword, overview: overview, json: [
                "term": .string(idea.title.lowercased()), "notes": .string(idea.detail), "campaign": .string(campaign?.title ?? ""),
            ], zone: zone)
        case "campaign":
            return MarketingDraft(kind: .campaign, overview: overview, json: [
                "name": .string(idea.title), "brief": .string(idea.detail), "status": "idea",
            ], zone: zone)
        default:
            return nil
        }
    }

    public static func suggestionDraft(_ suggestion: MarketingSuggestion, overview: MarketingOverview, zone: TimeZone = .current)
        -> MarketingDraft?
    {
        let action = suggestion.action
        switch suggestion.actionType {
        case "campaign": return MarketingDraft(kind: .campaign, overview: overview, zone: zone)
        case "campaign-edit":
            guard let campaign = overview.campaign(action.str("id")) else { return nil }
            return MarketingDraft(kind: .campaign, id: campaign.id, overview: overview, json: campaign.json, zone: zone)
        case "content":
            return contentDraft(overview, campaign: action.str("campaign"), channel: action.str("channel"), zone: zone)
        case "keyword": return MarketingDraft(kind: .keyword, overview: overview, zone: zone)
        case "channel": return metricDraft(overview, channel: action.str("channel"), zone: zone)
        default: return nil
        }
    }

    public static func taskToggleBody(_ campaign: MarketingRecord, index: Int, done: Bool) -> JSONObject {
        var tasks = campaign.tasks
        if tasks.indices.contains(index) { tasks[index].done = done }
        return ["tasks": .array(tasks.map(\.json))]
    }

    public static func addTaskBody(_ campaign: MarketingRecord, title: String) -> JSONObject {
        var tasks = campaign.tasks
        tasks.append(MarketingTask(title: String(title.prefix(200))))
        return ["tasks": .array(tasks.prefix(40).map(\.json))]
    }
}

public enum MarketingDates {
    public static func text(_ ms: Int64, time: Bool, zone: TimeZone) -> String {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = zone
        let parts = calendar.dateComponents([.year, .month, .day, .hour, .minute], from: Date(timeIntervalSince1970: Double(ms) / 1000))
        let day = String(format: "%04d-%02d-%02d", parts.year ?? 1970, parts.month ?? 1, parts.day ?? 1)
        if !time || (parts.hour == 0 && parts.minute == 0) { return day }
        return day + String(format: " %02d:%02d", parts.hour ?? 0, parts.minute ?? 0)
    }

    public static func parse(_ raw: String, zone: TimeZone) -> Int64? {
        let pieces = raw.trimmingCharacters(in: .whitespaces).split(whereSeparator: { $0 == " " || $0 == "T" })
        guard let dayPart = pieces.first, pieces.count <= 2 else { return nil }
        let day = dayPart.split(separator: "-").compactMap { Int($0) }
        guard day.count == 3, (1...12).contains(day[1]), (1...31).contains(day[2]), day[0] >= 1970 else { return nil }
        var components = DateComponents(year: day[0], month: day[1], day: day[2], hour: 0, minute: 0)
        if pieces.count == 2 {
            let clock = pieces[1].split(separator: ":").compactMap { Int($0) }
            guard clock.count == 2, (0...23).contains(clock[0]), (0...59).contains(clock[1]) else { return nil }
            components.hour = clock[0]
            components.minute = clock[1]
        }
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = zone
        guard let date = calendar.date(from: components), calendar.component(.day, from: date) == day[2] else { return nil }
        return Int64((date.timeIntervalSince1970 * 1000).rounded())
    }
}

public enum MarketingMoney {
    public static func cents(_ raw: String) -> Int64? {
        let clean = raw.replacingOccurrences(of: " ", with: "").replacingOccurrences(of: "$", with: "")
        let normalized = clean.contains(".") ? clean.replacingOccurrences(of: ",", with: "") : clean.replacingOccurrences(of: ",", with: ".")
        let parts = normalized.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count <= 2, let whole = Int64(parts[0].isEmpty ? "0" : String(parts[0])), whole >= 0 else { return nil }
        var fraction: Int64 = 0
        if parts.count == 2 {
            let digits = String(parts[1])
            guard digits.count <= 2, digits.allSatisfy(\.isNumber) else { return nil }
            fraction = Int64(digits.isEmpty ? "0" : digits.count == 1 ? digits + "0" : digits) ?? 0
        }
        return whole * 100 + fraction
    }
}
