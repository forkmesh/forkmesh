import FMApi
import FMBase
import Foundation
import Observation

public enum AppStoreTone: String, Equatable, Sendable {
    case action
    case pending
    case good
    case neutral

    public init(raw: String) {
        self = AppStoreTone(rawValue: raw.trimmingCharacters(in: .whitespaces).lowercased()) ?? .neutral
    }
}

public struct AppStoreState: Decodable, Equatable, Sendable {
    public var state: String
    public var label: String
    public var tone: AppStoreTone

    public init(state: String = "", label: String = "", tone: AppStoreTone = .neutral) {
        self.state = state
        self.label = label
        self.tone = tone
    }

    enum CodingKeys: String, CodingKey { case state, label, tone }

    public init(from decoder: Decoder) throws {
        if let raw = try? decoder.singleValueContainer().decode(String.self) {
            state = raw
            label = ""
            tone = .neutral
            return
        }
        let c = try decoder.container(keyedBy: CodingKeys.self)
        state = c.lenientString(.state)
        label = c.lenientString(.label)
        tone = AppStoreTone(raw: c.lenientString(.tone))
    }

    public var text: String {
        if !label.isEmpty { return label }
        if !state.isEmpty { return AppStoreStatusText.humanize(state) }
        return "Unknown"
    }

    public var isEmpty: Bool { state.isEmpty && label.isEmpty }
}

public struct AppStoreVersion: Decodable, Equatable, Sendable {
    public var version: String
    public var build: String
    public var releaseType: String
    public var createdAt: String
    public var status: AppStoreState

    enum CodingKeys: String, CodingKey { case version, build, releaseType, createdAt, status }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        version = c.lenientString(.version)
        build = c.lenientString(.build)
        releaseType = c.lenientString(.releaseType)
        createdAt = c.lenientString(.createdAt)
        status = c.lenientState(.status)
    }
}

public struct AppStoreSubmissionItem: Decodable, Equatable, Sendable {
    public var name: String
    public var status: AppStoreState

    enum CodingKeys: String, CodingKey { case name, status }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = c.lenientString(.name)
        status = c.lenientState(.status)
    }
}

public struct AppStoreSubmission: Decodable, Equatable, Sendable {
    public var id: String
    public var submittedAt: String
    public var status: AppStoreState
    public var items: [AppStoreSubmissionItem]

    enum CodingKeys: String, CodingKey { case id, submittedAt, status, items }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.lenientString(.id)
        submittedAt = c.lenientString(.submittedAt)
        status = c.lenientState(.status)
        items = c.lenientList(.items)
    }
}

public struct AppStoreDownloadFigures: Decodable, Equatable, Sendable {
    public var days: Int
    public var downloads: Int
    public var redownloads: Int
    public var updates: Int

    public init(days: Int = 0, downloads: Int = 0, redownloads: Int = 0, updates: Int = 0) {
        self.days = days
        self.downloads = downloads
        self.redownloads = redownloads
        self.updates = updates
    }

    enum CodingKeys: String, CodingKey { case days, downloads, redownloads, updates }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        days = c.lenientInt(.days)
        downloads = c.lenientInt(.downloads)
        redownloads = c.lenientInt(.redownloads)
        updates = c.lenientInt(.updates)
    }
}

public struct AppStoreDownloads: Decodable, Equatable, Sendable {
    public var available: Bool
    public var note: String
    public var since: String
    public var total: AppStoreDownloadFigures
    public var recent: [AppStoreDownloadFigures]

    public init(
        available: Bool = false, note: String = "", since: String = "", total: AppStoreDownloadFigures = .init(),
        recent: [AppStoreDownloadFigures] = []
    ) {
        self.available = available
        self.note = note
        self.since = since
        self.total = total
        self.recent = recent
    }

    enum CodingKeys: String, CodingKey { case available, note, since, total, recent }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        available = c.lenientBool(.available)
        note = c.lenientString(.note)
        since = c.lenientString(.since)
        total = (try? c.decodeIfPresent(AppStoreDownloadFigures.self, forKey: .total)) ?? AppStoreDownloadFigures()
        recent = c.lenientList(.recent)
    }
}

public struct AppStoreBuild: Decodable, Equatable, Sendable {
    public var version: String
    public var build: String
    public var uploadedAt: String
    public var expired: Bool
    public var processing: AppStoreState
    public var internalTesting: AppStoreState
    public var externalTesting: AppStoreState
    public var betaReview: AppStoreState
    public var groups: [String]

    enum CodingKeys: String, CodingKey {
        case version, build, uploadedAt, expired, processing, betaReview, groups
        case internalTesting = "internal"
        case externalTesting = "external"
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        version = c.lenientString(.version)
        build = c.lenientString(.build)
        uploadedAt = c.lenientString(.uploadedAt)
        expired = c.lenientBool(.expired)
        processing = c.lenientState(.processing)
        internalTesting = c.lenientState(.internalTesting)
        externalTesting = c.lenientState(.externalTesting)
        betaReview = c.lenientState(.betaReview)
        groups = c.lenientStrings(.groups)
    }
}

public struct AppStoreGroup: Decodable, Equatable, Sendable {
    public var name: String
    public var isInternal: Bool
    public var testers: Int
    public var allBuilds: Bool
    public var publicLinkEnabled: Bool
    public var publicLink: String
    public var publicLinkLimit: Int
    public var feedbackEnabled: Bool

    enum CodingKeys: String, CodingKey {
        case name, testers, allBuilds, publicLinkEnabled, publicLink, publicLinkLimit, feedbackEnabled
        case isInternal = "internal"
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = c.lenientString(.name)
        isInternal = c.lenientBool(.isInternal)
        testers = c.lenientInt(.testers)
        allBuilds = c.lenientBool(.allBuilds)
        publicLinkEnabled = c.lenientBool(.publicLinkEnabled)
        publicLink = c.lenientString(.publicLink)
        publicLinkLimit = c.lenientInt(.publicLinkLimit)
        feedbackEnabled = c.lenientBool(.feedbackEnabled)
    }

    public var publicURL: URL? {
        guard publicLinkEnabled, let url = URL(string: publicLink), url.scheme?.lowercased() == "https" else { return nil }
        return url
    }
}

public struct AppStoreScreenshot: Decodable, Equatable, Hashable, Sendable, Identifiable {
    public static let maxPerFeedback = 4

    public var id: String
    public var width: Int
    public var height: Int
    public var url: String

    public init(id: String, width: Int = 0, height: Int = 0, url: String = "") {
        self.id = id
        self.width = width
        self.height = height
        self.url = url
    }

    enum CodingKeys: String, CodingKey { case id, width, height, url }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.lenientString(.id)
        width = min(max(c.lenientInt(.width), 0), 20000)
        height = min(max(c.lenientInt(.height), 0), 20000)
        url = c.lenientString(.url)
    }

    public static func isValidId(_ id: String) -> Bool {
        id.count == 24 && id.allSatisfy { "0123456789abcdef".contains($0) }
    }

    public var directURL: URL? {
        guard let url = URL(string: url), url.scheme?.lowercased() == "https", url.host != nil else { return nil }
        return url
    }

    public var aspectRatio: Double? {
        guard width > 0, height > 0 else { return nil }
        return Double(width) / Double(height)
    }
}

public struct AppStoreFeedback: Decodable, Equatable, Sendable {
    public var kind: String
    public var createdAt: String
    public var from: String
    public var device: String
    public var comment: String
    public var screenshots: [AppStoreScreenshot]

    enum CodingKeys: String, CodingKey { case kind, createdAt, from, device, comment, screenshots }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        kind = c.lenientString(.kind)
        createdAt = c.lenientString(.createdAt)
        from = c.lenientString(.from)
        device = c.lenientString(.device)
        comment = c.lenientString(.comment)
        let shots: [AppStoreScreenshot] = c.lenientList(.screenshots)
        screenshots = Array(shots.filter { AppStoreScreenshot.isValidId($0.id) }.prefix(AppStoreScreenshot.maxPerFeedback))
    }
}

public struct AppStoreReview: Decodable, Equatable, Sendable {
    public var createdAt: String
    public var rating: Int
    public var title: String
    public var territory: String
    public var body: String

    enum CodingKeys: String, CodingKey { case createdAt, rating, title, territory, body }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        createdAt = c.lenientString(.createdAt)
        rating = min(max(c.lenientInt(.rating), 0), 5)
        title = c.lenientString(.title)
        territory = c.lenientString(.territory)
        body = c.lenientString(.body)
    }

    public var stars: String { AppStoreStatusText.stars(rating) }
}

public enum AppStoreSection: String, CaseIterable, Sendable {
    case versions, submissions, downloads, builds, groups, feedback, reviews

    public var title: String {
        switch self {
        case .versions: return "App Store versions"
        case .submissions: return "App Review submissions"
        case .downloads: return "Downloads"
        case .builds: return "TestFlight builds"
        case .groups: return "TestFlight groups"
        case .feedback: return "Tester feedback"
        case .reviews: return "Customer reviews"
        }
    }

    public var emptyText: String {
        switch self {
        case .versions: return "No App Store versions yet."
        case .submissions: return "No App Review submissions."
        case .downloads: return "No download figures yet."
        case .builds: return "No TestFlight builds."
        case .groups: return "No TestFlight groups."
        case .feedback: return "No tester feedback."
        case .reviews: return "No customer reviews yet."
        }
    }
}

public struct AppStoreSnapshot: Decodable, Equatable, Sendable {
    public static let format = "forkmesh-app-store-status-v1"

    public var format: String
    public var generatedAt: Int64
    public var appId: String
    public var bundleId: String
    public var attention: [String]
    public var versions: [AppStoreVersion]
    public var submissions: [AppStoreSubmission]
    public var downloads: AppStoreDownloads
    public var builds: [AppStoreBuild]
    public var groups: [AppStoreGroup]
    public var feedback: [AppStoreFeedback]
    public var reviews: [AppStoreReview]
    public var unavailable: [String: String]

    enum CodingKeys: String, CodingKey {
        case format, generatedAt, app, attention, versions, submissions, downloads, builds, groups, feedback, reviews
        case unavailable
    }

    enum AppKeys: String, CodingKey { case id, bundleId }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        format = c.lenientString(.format)
        generatedAt = c.lenientStamp(.generatedAt)
        if let app = try? c.nestedContainer(keyedBy: AppKeys.self, forKey: .app) {
            appId = app.lenientString(.id)
            bundleId = app.lenientString(.bundleId)
        } else {
            appId = ""
            bundleId = ""
        }
        attention = c.lenientStrings(.attention).filter { !$0.trimmingCharacters(in: .whitespaces).isEmpty }
        versions = c.lenientList(.versions)
        submissions = c.lenientList(.submissions)
        downloads = (try? c.decodeIfPresent(AppStoreDownloads.self, forKey: .downloads)) ?? AppStoreDownloads()
        builds = c.lenientList(.builds)
        groups = c.lenientList(.groups)
        feedback = c.lenientList(.feedback)
        reviews = c.lenientList(.reviews)
        let reasons = (try? c.decodeIfPresent([String: AppStoreLenient<String>].self, forKey: .unavailable)) ?? [:]
        unavailable = reasons.compactMapValues { entry in
            guard let text = entry.value, !text.isEmpty else { return nil }
            return text
        }
    }

    public func unavailable(_ section: AppStoreSection) -> String? { unavailable[section.rawValue] }

    public var lacksScreenshots: Bool {
        let reports = feedback.filter { $0.kind == "screenshot" }
        return !reports.isEmpty && reports.allSatisfy { $0.screenshots.isEmpty }
    }

    public var sortedGroups: [AppStoreGroup] {
        groups.enumerated().sorted { lhs, rhs in
            if lhs.element.isInternal != rhs.element.isInternal { return lhs.element.isInternal }
            return lhs.offset < rhs.offset
        }.map(\.element)
    }

    public static func decode(_ data: Data) throws -> AppStoreSnapshot {
        try JSONDecoder().decode(AppStoreSnapshot.self, from: data)
    }
}

public struct AppStoreStatusResponse: Decodable, Equatable, Sendable {
    public var published: Bool
    public var publishedAt: Int64
    public var snapshot: AppStoreSnapshot?
    public var fromDesktop: Bool

    public init(
        published: Bool = false, publishedAt: Int64 = 0, snapshot: AppStoreSnapshot? = nil, fromDesktop: Bool = false
    ) {
        self.published = published
        self.publishedAt = publishedAt
        self.snapshot = snapshot
        self.fromDesktop = fromDesktop
    }

    enum CodingKeys: String, CodingKey { case published, publishedAt, snapshot, source }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        snapshot = try? c.decodeIfPresent(AppStoreSnapshot.self, forKey: .snapshot)
        let stamp = c.lenientStamp(.publishedAt)
        publishedAt = stamp > 0 ? stamp : (snapshot?.generatedAt ?? 0)
        published = snapshot != nil && (c.optionalBool(.published) ?? true)
        fromDesktop = c.lenientString(.source) == "desktop"
    }

    public static func decode(_ data: Data) throws -> AppStoreStatusResponse {
        try JSONDecoder().decode(AppStoreStatusResponse.self, from: data)
    }

    public static func decode(_ object: JSONObject) throws -> AppStoreStatusResponse {
        try decode(JSON.encodeData(.object(object)))
    }
}

public enum AppStoreStatusText {
    public static let title = "App Store Status"
    public static let packageId = "app-store-status"
    public static let route = "/dashboard/app-store-status"
    public static let apiPath = "/api/app-store-status"
    public static let shortcutFile = "ios-app-store-status.sh"
    public static let shortcutName = "iOS App Store status"
    public static let openTitle = "Open App Store Status"
    public static let unavailableTitle = "App Store Status is unavailable"
    public static let emptyTitle = "Nothing published yet"
    public static let emptyMessage =
        "Your desktop publishes this page when the \(shortcutName) shortcut runs. Run it from the desktop's Shortcuts page, or from Shortcuts on this iPhone, then pull down to refresh."
    public static let notInstalled =
        "App Store Status is not installed for your account. Install it from the Market, then run the \(shortcutName) shortcut on your desktop."
    public static let genericError = "App Store Status is unavailable right now."
    public static let notOffered = "This server does not offer App Store Status yet."
    public static let desktopMethod = "stats.appStoreStatus"
    public static let desktopTimeout: TimeInterval = 120

    public static func isStatusShortcut(file: String) -> Bool {
        let name = file.split(separator: "/").last.map(String.init) ?? file
        return name.lowercased() == shortcutFile
    }

    public static func publishedLine(_ publishedAt: Int64, nowMillis: Int64 = SystemClock().nowMillis) -> String {
        guard publishedAt > 0 else { return "" }
        return "Published \(Format.relativeTimeFrom(publishedAt, nowMillis: nowMillis))"
    }

    public static func sourceLine(
        _ response: AppStoreStatusResponse, nowMillis: Int64 = SystemClock().nowMillis
    ) -> String {
        guard response.fromDesktop else { return publishedLine(response.publishedAt, nowMillis: nowMillis) }
        guard response.publishedAt > 0 else { return "From your desktop" }
        return "From your desktop \(Format.relativeTimeFrom(response.publishedAt, nowMillis: nowMillis))"
    }

    public static func isMissing(_ error: Error) -> Bool {
        guard let failure = error as? OrgApiException else { return false }
        return missingCodes.contains(failure.code)
    }

    static let missingCodes: Set<String> = [
        "http_404", "not_found", "http_405", "method_not_allowed",
        "plugin_not_installed", "package_not_installed", "not_installed",
    ]

    public static func desktopMessage(for error: Error) -> String {
        let reason = (error as? OrgApiException).map { $0.message.isEmpty ? $0.code : $0.message } ?? ""
        return reason.isEmpty
            ? "Your desktop could not fetch it either."
            : "Your desktop could not fetch it either: \(reason)"
    }

    public static func screenshotPath(_ id: String) -> String { "\(apiPath)/screenshots/\(id)" }

    public static let screenshotMissing =
        "This screenshot has not been uploaded yet. Run the \(shortcutName) shortcut on your desktop again."

    public static let screenshotsNotPublished =
        "Tester screenshots appear once the \(shortcutName) shortcut runs again on your desktop."

    static let screenshotMissingCodes: Set<String> = ["screenshot_not_found", "http_404", "screenshot_not_in_snapshot"]

    public static func screenshotTitle(index: Int, count: Int) -> String {
        count > 1 ? "Screenshot \(index + 1) of \(count)" : "Screenshot"
    }

    public static func screenshotData(_ object: JSONObject) -> Data? {
        guard let mime = object["mime"]?.stringValue, let text = object["data"]?.stringValue,
              let data = Data(base64Encoded: text), isPicture(data, mime: mime) else { return nil }
        return data
    }

    public static func isPicture(_ data: Data, mime: String? = nil) -> Bool {
        let jpeg = data.starts(with: [0xFF, 0xD8, 0xFF])
        let png = data.starts(with: [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])
        switch mime {
        case "image/jpeg": return jpeg
        case "image/png": return png
        case nil: return jpeg || png
        default: return false
        }
    }

    public static func stars(_ rating: Int) -> String {
        let filled = min(max(rating, 0), 5)
        return String(repeating: "\u{2605}", count: filled) + String(repeating: "\u{2606}", count: 5 - filled)
    }

    public static func humanize(_ state: String) -> String {
        let words = state.split(whereSeparator: { $0 == "_" || $0 == " " }).map { $0.lowercased() }
        guard let first = words.first else { return "" }
        return ([first.prefix(1).uppercased() + first.dropFirst()] + words.dropFirst()).joined(separator: " ")
    }

    public static func parseDate(_ text: String) -> Date? {
        let value = text.trimmingCharacters(in: .whitespaces)
        if value.isEmpty { return nil }
        let plain = ISO8601DateFormatter()
        plain.formatOptions = [.withInternetDateTime]
        if let date = plain.date(from: value) { return date }
        let fractional = ISO8601DateFormatter()
        fractional.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return fractional.date(from: value)
    }

    public static func date(_ text: String, timeZone: TimeZone = .current, locale: Locale = .current) -> String {
        guard let date = parseDate(text) else { return text }
        let formatter = DateFormatter()
        formatter.locale = locale
        formatter.timeZone = timeZone
        formatter.dateStyle = .medium
        formatter.timeStyle = .short
        return formatter.string(from: date)
    }

    public static func epochMillis(_ text: String) -> Int64 {
        let value = text.trimmingCharacters(in: .whitespaces)
        if let number = Double(value) { return millis(number) }
        return parseDate(value).map { Int64(($0.timeIntervalSince1970 * 1000).rounded()) } ?? 0
    }

    static func millis(_ number: Double) -> Int64 {
        guard number.isFinite, number > 0 else { return 0 }
        return number < 100_000_000_000 ? Int64(number * 1000) : Int64(number)
    }

    public static func spanTitle(_ figures: AppStoreDownloadFigures) -> String {
        figures.days == 1 ? "Last day" : "Last \(figures.days) days"
    }

    public static func totalTitle(_ downloads: AppStoreDownloads) -> String {
        downloads.since.isEmpty ? "Total" : "Total since \(downloads.since)"
    }

    public static func versionTitle(version: String, build: String) -> String {
        let name = version.isEmpty ? "Version" : version
        return build.isEmpty ? name : "\(name) (\(build))"
    }

    public static func releaseType(_ raw: String) -> String {
        switch raw.uppercased() {
        case "": return ""
        case "AFTER_APPROVAL": return "Releases after approval"
        case "MANUAL": return "Manual release"
        case "SCHEDULED": return "Scheduled release"
        default: return humanize(raw)
        }
    }

    public static func testers(_ count: Int) -> String { count == 1 ? "1 tester" : "\(count) testers" }

    public static func groupBuilds(_ group: AppStoreGroup) -> String { group.allBuilds ? "Every build" : "Chosen builds" }

    public static func groupKind(_ group: AppStoreGroup) -> String { group.isInternal ? "Internal" : "External" }

    public static func groupFeedback(_ group: AppStoreGroup) -> String { group.feedbackEnabled ? "Feedback on" : "Feedback off" }

    public static func publicLinkLimit(_ group: AppStoreGroup) -> String {
        group.publicLinkLimit > 0 ? "Up to \(group.publicLinkLimit) testers" : ""
    }

    public static func message(for error: Error) -> String {
        guard let failure = error as? OrgApiException else { return genericError }
        switch failure.code {
        case "plugin_not_installed", "package_not_installed", "not_installed": return notInstalled
        case "http_404", "not_found", "http_405", "method_not_allowed": return notOffered
        default: return failure.message.isEmpty ? genericError : failure.message
        }
    }
}

public struct AppStoreStatusAPI {
    public var fetch: () async throws -> JSONObject
    public var desktop: (() async throws -> JSONObject)?
    public var screenshot: ((String) async throws -> JSONObject)?
    public var direct: (@Sendable (URL) async throws -> Data)?

    public init(
        fetch: @escaping () async throws -> JSONObject, desktop: (() async throws -> JSONObject)? = nil,
        screenshot: ((String) async throws -> JSONObject)? = nil, direct: (@Sendable (URL) async throws -> Data)? = nil
    ) {
        self.fetch = fetch
        self.desktop = desktop
        self.screenshot = screenshot
        self.direct = direct
    }

    @MainActor
    public init(
        api: ApiService, desktop: (() async throws -> JSONObject)? = nil,
        direct: (@Sendable (URL) async throws -> Data)? = nil
    ) {
        fetch = { try await api.accountApiCall("GET", AppStoreStatusText.apiPath) }
        self.desktop = desktop
        screenshot = { id in try await api.accountApiCall("GET", AppStoreStatusText.screenshotPath(id)) }
        self.direct = direct
    }
}

public struct AppStoreScreenshotError: Error, Equatable {
    public var message: String

    public init(_ message: String) { self.message = message }
}

@MainActor
public final class AppStoreScreenshotLoader {
    public typealias Direct = @Sendable (URL) async throws -> Data

    private let server: ((String) async throws -> JSONObject)?
    private let direct: Direct?
    private var cache: [String: Data] = [:]

    public init(server: ((String) async throws -> JSONObject)?, direct: Direct? = nil) {
        self.server = server
        self.direct = direct
    }

    public func cached(_ shot: AppStoreScreenshot) -> Data? { cache[shot.id] }

    public func load(_ shot: AppStoreScreenshot) async throws -> Data {
        if let data = cache[shot.id] { return data }
        let data = try await fetch(shot)
        cache[shot.id] = data
        return data
    }

    private func fetch(_ shot: AppStoreScreenshot) async throws -> Data {
        var failure: Error?
        if let direct, let url = shot.directURL {
            do {
                let data = try await direct(url)
                if AppStoreStatusText.isPicture(data) { return data }
            } catch {
                failure = error
            }
        }
        guard let server else {
            throw AppStoreScreenshotError(
                failure == nil ? AppStoreStatusText.screenshotMissing : "This screenshot could not be loaded.")
        }
        do {
            let object = try await server(shot.id)
            if let data = AppStoreStatusText.screenshotData(object) { return data }
            throw AppStoreScreenshotError("This screenshot is not a picture.")
        } catch let error as OrgApiException {
            if AppStoreStatusText.screenshotMissingCodes.contains(error.code) {
                throw AppStoreScreenshotError(AppStoreStatusText.screenshotMissing)
            }
            throw AppStoreScreenshotError(error.message.isEmpty ? "This screenshot could not be loaded." : error.message)
        }
    }
}

@MainActor
@Observable
public final class AppStoreStatusModel {
    @ObservationIgnored public let api: AppStoreStatusAPI
    @ObservationIgnored public let screenshots: AppStoreScreenshotLoader

    public private(set) var response: AppStoreStatusResponse?
    public private(set) var loading = false
    public private(set) var error = ""

    public init(api: AppStoreStatusAPI) {
        self.api = api
        screenshots = AppStoreScreenshotLoader(server: api.screenshot, direct: api.direct)
    }

    public var snapshot: AppStoreSnapshot? { response?.snapshot }
    public var showsSpinner: Bool { response == nil && error.isEmpty }
    public var showsError: Bool { response == nil && !error.isEmpty }
    public var showsEmpty: Bool { response != nil && snapshot == nil }
    public var refreshError: String { response != nil ? error : "" }

    public func load() async {
        loading = true
        defer { loading = false }
        var server: AppStoreStatusResponse?
        var serverError: Error?
        do {
            server = try AppStoreStatusResponse.decode(try await api.fetch())
        } catch {
            serverError = error
        }
        let missing = server.map { $0.snapshot == nil } ?? serverError.map(AppStoreStatusText.isMissing) ?? false
        let stale = server?.snapshot?.lacksScreenshots ?? false
        var desktopNote = ""
        if missing || stale, let desktop = api.desktop {
            do {
                let reply = try AppStoreStatusResponse.decode(try await desktop())
                if let snapshot = reply.snapshot, !stale || !snapshot.lacksScreenshots {
                    response = reply
                    error = ""
                    return
                }
            } catch {
                if missing { desktopNote = AppStoreStatusText.desktopMessage(for: error) }
            }
        }
        if let server {
            response = server
            error = desktopNote
        } else if let serverError {
            let message = AppStoreStatusText.message(for: serverError)
            error = desktopNote.isEmpty ? message : message + " " + desktopNote
        }
    }

    public func dismissError() { error = "" }
}

extension KeyedDecodingContainer {
    func lenientStamp(_ key: Key) -> Int64 {
        if let number = try? decodeIfPresent(Double.self, forKey: key) { return AppStoreStatusText.millis(number) }
        if let text = try? decodeIfPresent(String.self, forKey: key) { return AppStoreStatusText.epochMillis(text) }
        return 0
    }

    func lenientString(_ key: Key) -> String {
        if let text = try? decodeIfPresent(String.self, forKey: key) { return text }
        if let number = try? decodeIfPresent(Int64.self, forKey: key) { return String(number) }
        if let number = try? decodeIfPresent(Double.self, forKey: key) { return String(number) }
        return ""
    }

    func lenientInt(_ key: Key) -> Int {
        if let number = try? decodeIfPresent(Int.self, forKey: key) { return number }
        if let number = try? decodeIfPresent(Double.self, forKey: key), number.isFinite,
           abs(number) < Double(Int32.max) { return Int(number) }
        if let text = try? decodeIfPresent(String.self, forKey: key) { return Int(text.trimmingCharacters(in: .whitespaces)) ?? 0 }
        return 0
    }

    func optionalBool(_ key: Key) -> Bool? {
        if let flag = try? decodeIfPresent(Bool.self, forKey: key) { return flag }
        if let number = try? decodeIfPresent(Int.self, forKey: key) { return number != 0 }
        if let text = try? decodeIfPresent(String.self, forKey: key) {
            switch text.lowercased() {
            case "true", "1", "yes": return true
            case "false", "0", "no": return false
            default: return nil
            }
        }
        return nil
    }

    func lenientBool(_ key: Key) -> Bool { optionalBool(key) ?? false }

    func lenientState(_ key: Key) -> AppStoreState {
        (try? decodeIfPresent(AppStoreState.self, forKey: key)) ?? AppStoreState()
    }

    func lenientStrings(_ key: Key) -> [String] {
        guard let wrapped = try? decodeIfPresent([AppStoreLenient<String>].self, forKey: key) else { return [] }
        return wrapped.compactMap(\.value)
    }

    func lenientList<T: Decodable>(_ key: Key) -> [T] {
        guard let wrapped = try? decodeIfPresent([AppStoreLenient<T>].self, forKey: key) else { return [] }
        return wrapped.compactMap(\.value)
    }
}

struct AppStoreLenient<T: Decodable>: Decodable {
    let value: T?

    init(from decoder: Decoder) throws {
        value = try? T(from: decoder)
    }
}
