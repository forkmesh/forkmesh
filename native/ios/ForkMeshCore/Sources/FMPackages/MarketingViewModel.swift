import FMBase
import Foundation
import Observation

@MainActor
@Observable
public final class MarketingModel {
    public let packages: OrgPackageModel<MarketingOverview>
    public private(set) var working: Set<String> = []
    public var notice = ""
    public private(set) var advice: MarketingAdvice?
    public private(set) var asking = false
    public private(set) var performance: MarketingPerformance?
    public private(set) var loadingPerformance = false
    public var performanceDays = 28
    public var performanceProject = ""
    public private(set) var report: MarketingReport?
    public private(set) var readyPost: MarketingReadyState?

    @ObservationIgnored private let client: PackagesClient
    @ObservationIgnored private let clock: @MainActor () -> Int64
    @ObservationIgnored private let zone: TimeZone

    public init(
        client: PackagesClient, organization: String = "", zone: TimeZone = .current,
        clock: @escaping @MainActor () -> Int64 = { Int64(Date().timeIntervalSince1970 * 1000) }
    ) {
        self.client = client
        self.clock = clock
        self.zone = zone
        packages = OrgPackageModel<MarketingOverview>(
            organization: organization, organizations: { try await client.marketingOrganizations() },
            load: { try await client.marketing($0) })
    }

    public var overview: MarketingOverview? { packages.payload }
    public var organization: String { packages.organization }
    public var now: Int64 { clock() }
    public var timeZone: TimeZone { zone }

    public var errorText: String {
        get { packages.errorText }
        set { packages.errorText = newValue }
    }

    public func refresh() async { await packages.refresh() }

    public func reload() async { await packages.load() }

    public func isWorking(_ id: String) -> Bool { working.contains(id) }

    public func draft(_ kind: MarketingKind, record: MarketingRecord) -> MarketingDraft? {
        guard let overview else { return nil }
        return MarketingDraft(kind: kind, id: record.id, overview: overview, json: record.json, zone: zone)
    }

    public func newDraft(_ kind: MarketingKind) -> MarketingDraft? {
        guard let overview else { return nil }
        switch kind {
        case .attribution, .targets, .goals, .redditWatch: return MarketingForms.settingsDraft(kind, overview: overview, zone: zone)
        case .content: return MarketingForms.contentDraft(overview, zone: zone)
        default: return MarketingDraft(kind: kind, overview: overview, zone: zone)
        }
    }

    @discardableResult
    public func save(_ draft: MarketingDraft) async -> String? {
        guard let overview else { return "Nothing is loaded yet." }
        let key = draft.id.isEmpty ? draft.kind.rawValue : draft.id
        working.insert(key)
        defer { working.remove(key) }
        do {
            let reply = try await client.saveMarketing(draft, organization: organization, overview: overview, now: now, zone: zone)
            notice = Self.savedText(draft, reply: reply)
            await packages.load()
            return nil
        } catch {
            return PackagesError.describe(error)
        }
    }

    static func savedText(_ draft: MarketingDraft, reply: JSONObject) -> String {
        switch draft.kind {
        case .xPost: return reply.str("url").isEmpty ? "Posted to X." : "Posted to X: " + reply.str("url")
        case .xConnection: return "Connected X as @" + reply.obj("x").str("username") + "."
        case .metric: return "Numbers logged."
        case .funnelWeek: return "Week saved."
        case .channel, .attribution, .targets, .goals, .redditWatch: return "Settings saved."
        default: return draft.isNew ? draft.kind.title + " added." : "Saved."
        }
    }

    public func setStatus(_ kind: MarketingKind, id: String, status: String) async {
        working.insert(id)
        defer { working.remove(id) }
        do {
            var body: JSONObject = ["status": .string(status)]
            if kind == .content, status == "published",
                overview?.content.first(where: { $0.id == id })?.publishedAt == 0
            {
                body["publishedAt"] = .int(now)
            }
            guard kind.statuses.contains(status) else { throw PackagesError("invalid_status") }
            try await client.sendMarketing(kind, id: id, organization: organization, body: body)
            notice = "Moved to \(MarketingText.title(status))."
            await packages.load()
        } catch {
            errorText = PackagesError.describe(error)
        }
    }

    public func delete(_ kind: MarketingKind, id: String) async -> Bool {
        working.insert(id)
        defer { working.remove(id) }
        do {
            try await client.deleteMarketing(kind, id: id, organization: organization)
            notice = kind.title + " deleted."
            await packages.load()
            return true
        } catch {
            errorText = PackagesError.describe(error)
            return false
        }
    }

    public func toggleTask(_ campaign: MarketingRecord, index: Int, done: Bool) async {
        await patchCampaign(campaign, body: MarketingForms.taskToggleBody(campaign, index: index, done: done))
    }

    public func addTask(_ campaign: MarketingRecord, title: String) async {
        let clean = title.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !clean.isEmpty else { return }
        await patchCampaign(campaign, body: MarketingForms.addTaskBody(campaign, title: clean))
    }

    private func patchCampaign(_ campaign: MarketingRecord, body: JSONObject) async {
        working.insert(campaign.id)
        defer { working.remove(campaign.id) }
        do {
            try await client.sendMarketing(.campaign, id: campaign.id, organization: organization, body: body)
            await packages.load()
        } catch {
            errorText = PackagesError.describe(error)
        }
    }

    public func refreshChannel(_ channel: String) async {
        working.insert("refresh:" + channel)
        defer { working.remove("refresh:" + channel) }
        do {
            let (fresh, message) = try await client.refreshMarketingChannel(channel, organization: organization)
            packages.update { $0 = fresh }
            notice = (overview?.channelName(channel) ?? channel) + " refreshed · " + message
        } catch {
            let name = overview?.channelName(channel) ?? channel
            errorText = name + " could not be refreshed: " + PackagesError.describe(error)
        }
    }

    public func refreshStale() async {
        let stale = overview?.staleChannels ?? []
        if stale.isEmpty {
            notice = "Every channel that refreshes itself is up to date."
            return
        }
        for channel in stale { await refreshChannel(channel) }
    }

    public func addChannels(_ ids: [String]) async {
        guard let overview, !ids.isEmpty else { return }
        working.insert("channels")
        defer { working.remove("channels") }
        do {
            try await client.addMarketingChannels(ids, organization: organization, overview: overview)
            notice = MarketingText.plural(ids.count, "channel", "channels") + " added."
            await packages.load()
        } catch {
            errorText = PackagesError.describe(error)
        }
    }

    public func ask(campaignId: String = "", question: String = "") async {
        asking = true
        defer { asking = false }
        do {
            advice = try await client.marketingAdvice(organization: organization, campaignId: campaignId, question: question)
        } catch {
            errorText = PackagesError.describe(error)
        }
    }

    public func clearAdvice() { advice = nil }

    public func apply(_ idea: MarketingIdea) async -> MarketingDraft? {
        guard let overview else { return nil }
        let campaign = advice.flatMap { overview.campaign($0.campaignId) }
        if idea.kind == "task" {
            guard let campaign else {
                errorText = "Pick a campaign to add tasks to."
                return nil
            }
            await addTask(campaign, title: idea.title)
            notice = "Task added to \(campaign.title)."
            return nil
        }
        return MarketingForms.ideaDraft(idea, overview: overview, campaign: campaign, now: now, zone: zone)
    }

    public func loadReport() async {
        working.insert("report")
        defer { working.remove("report") }
        do {
            report = try await client.marketingReport(organization: organization)
        } catch {
            errorText = PackagesError.describe(error)
        }
    }

    public func loadPerformance() async {
        loadingPerformance = true
        defer { loadingPerformance = false }
        do {
            performance = try await client.marketingPerformance(
                organization: organization, days: performanceDays, project: performanceProject, now: now)
        } catch {
            errorText = PackagesError.describe(error)
        }
    }

    public func buildLink(_ draft: MarketingDraft) async -> (MarketingLink?, String?) {
        guard let overview else { return (nil, "Nothing is loaded yet.") }
        do {
            return (try await client.buildMarketingLink(draft, organization: organization, overview: overview, now: now), nil)
        } catch {
            return (nil, PackagesError.describe(error))
        }
    }

    public func importKeywords(_ text: String, source: String, limit: Int, observedAt: Int64) async -> String? {
        let rows = MarketingKeywordImport.top(MarketingKeywordImport.rows(text), limit: limit)
        if rows.isEmpty { return "No keywords found in that text." }
        working.insert("keywords")
        defer { working.remove("keywords") }
        do {
            let result = try await client.importMarketingKeywords(rows, organization: organization, source: source, observedAt: observedAt)
            notice = "\(result.created) added · \(result.updated) updated" + (result.skipped > 0 ? " · \(result.skipped) skipped" : "")
            await packages.load()
            return nil
        } catch {
            return PackagesError.describe(error)
        }
    }

    public var readyX: MarketingReadyState { (overview?.readyX ?? MarketingReadyState(json: [:])).merged(with: readyPost) }

    public func loadReadyPost() async {
        guard let state = overview?.readyX, state.needsImage(loaded: readyPost) else { return }
        working.insert("ready-post")
        defer { working.remove("ready-post") }
        do {
            readyPost = try await client.marketingReadyPost(organization: organization, channel: state.channel)
        } catch {
            errorText = PackagesError.describe(error)
        }
    }

    public func closeReadyPost(id: String, status: String, url: String = "") async {
        guard overview?.canCloseReadyPost == true else { return }
        working.insert("ready-post")
        defer { working.remove("ready-post") }
        do {
            readyPost = try await client.closeMarketingReadyPost(
                organization: organization, channel: readyX.channel, id: id, status: status, url: url)
            notice = (status == "posted" ? "Marked posted." : "Skipped.") + " Herald writes the next one."
            await packages.load()
        } catch let error as PackagesError where ["ready_post_changed", "ready_post_not_found"].contains(error.code) {
            notice = error.message
            readyPost = nil
            await packages.load()
            await loadReadyPost()
        } catch {
            errorText = PackagesError.describe(error)
        }
    }

    public func sharedReadyPost(id: String, activity: String?, completed: Bool) async {
        guard MarketingReadyPost.sharePosted(activity: activity, completed: completed) else { return }
        await closeReadyPost(id: id, status: "posted")
    }

    public func disconnectX() async {
        working.insert("x")
        defer { working.remove("x") }
        do {
            try await client.disconnectX(organization: organization)
            notice = "X disconnected."
            await packages.load()
        } catch {
            errorText = PackagesError.describe(error)
        }
    }
}
