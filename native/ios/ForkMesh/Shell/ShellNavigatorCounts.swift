import FMApi
import FMBase
import FMChat
import FMCoves
import FMInsights
import FMLocalAI
import FMPackages
import FMPlanning
import FMTeammates
import FMTools
import FMVault
import Foundation
import Observation

@MainActor
@Observable
final class ShellNavigatorCounts {
    static let shared = ShellNavigatorCounts()

    private(set) var fetched: [Route.Kind: Int] = [:]
    @ObservationIgnored private var loadedAt: Date?
    private(set) var loading = false

    func reset() {
        fetched = [:]
        loadedAt = nil
    }

    func count(_ kind: Route.Kind) -> Int? {
        if let local = local(kind) { return local }
        return fetched[kind]
    }

    private func local(_ kind: Route.Kind) -> Int? {
        switch kind {
        case .desktops:
            return HostsFeature.hub?.candidates.count
        case .teammates:
            return TeammatesFeature.teammates?.snapshot?.teammates.count
        case .shortcuts:
            return TeammatesFeature.shortcuts?.catalog?.shortcuts.count
        case .timers:
            return PlanningTimersRuntime.shared()?.store.timers.count
        case .coves:
            guard let model = ShellNavigatorCounts.coves(), model.phase == .loaded else { return nil }
            return model.entries.count
        case .activity:
            return Services.get(RelayService.self)?.log.count
        case .tour:
            return TourPage.pages.count
        case .market:
            return HubPackageState.shared.installed.count
        case .vault:
            guard let vault = Services.get(VaultService.self), vault.phase == .unlocked else { return nil }
            return vault.items.count
        case .localAI:
            return Services.get(LocalModelManager.self)?.downloadedIds.count
        default:
            return nil
        }
    }

    func active(_ kind: Route.Kind) -> Bool {
        switch kind {
        case .timers:
            return ShellCounts.shared.timers > 0
        case .shortcuts:
            guard let store = TeammatesFeature.shortcuts else { return false }
            return store.anyRunning || store.catalog?.entries.contains(where: \.running) == true
        case .teammates:
            return TeammatesFeature.teammates?.snapshot?.teammates.contains { $0.status == .working } == true
        case .localAI:
            return !(Services.get(LocalModelManager.self)?.downloadProgress.isEmpty ?? true)
        default:
            return false
        }
    }

    static let fetchable: [Route.Kind] = [
        .repos, .notes, .tasks, .calendar, .orgs, .calories, .steps, .water, .sleep, .sugarBreak, .stretch, .admin,
        .crm, .marketing, .analytics,
    ]

    static var heat: DeviceHeat {
        switch ProcessInfo.processInfo.thermalState {
        case .nominal: return .nominal
        case .fair: return .fair
        case .serious: return .serious
        case .critical: return .critical
        @unknown default: return .serious
        }
    }

    func refresh(guest: Bool, force: Bool = false) async {
        if loading { return }
        let lowPower: Bool = ProcessInfo.processInfo.isLowPowerModeEnabled
        guard NavigatorCountPlan.due(
            loadedAt: loadedAt, now: Date(), heat: ShellNavigatorCounts.heat, lowPower: lowPower, force: force)
        else { return }
        guard let api = Services.get(ApiService.self) else { return }
        let packages: PackagesClient? = PackagesFeature.client
        let shown: Set<Route.Kind> = NavigatorCountPlan.shownKinds(HubGroups.current(guest: guest))
        let kinds: [Route.Kind] = NavigatorCountPlan.wanted(ShellNavigatorCounts.fetchable, shown: shown)
        loading = true
        defer { loading = false }
        let counts: [Route.Kind: Int] = await NavigatorCountPlan.run(kinds) { (kind: Route.Kind) -> Int? in
            await ShellNavigatorCounts.load(kind, api: api, packages: packages)
        }
        var next: [Route.Kind: Int] = fetched.merging(counts) { _, new in new }
        if shown.contains(.ledger) {
            next[.ledger] = PackagesFeature.personalLedger?.load().count ?? next[.ledger]
        }
        if shown.contains(.stats) {
            next[.stats] = InsightsFeature.statsLayoutStore()?.load().layout.count ?? next[.stats]
        }
        if shown.contains(.shortcuts) { await ShellNavigatorCounts.loadShortcuts() }
        if shown.contains(.coves) { await ShellNavigatorCounts.loadCoves() }
        fetched = next
        if !Task.isCancelled { loadedAt = Date() }
    }

    private static func load(_ kind: Route.Kind, api: ApiService, packages: PackagesClient?) async -> Int? {
        switch kind {
        case .repos: return try? await api.repositories().count
        case .notes: return await notes(api)
        case .tasks: return await tasks(api)
        case .calendar: return await eventsToday(api)
        case .orgs: return try? await api.myOrgs().count
        case .calories: return await calories(api)
        case .steps: return await steps(api)
        case .water: return await water(api)
        case .sleep: return await sleepHours(api)
        case .sugarBreak: return await sugarFreeDays(api)
        case .stretch: return await stretchSessionsToday(api)
        case .admin: return await adminUsers(api)
        case .crm: return await crmContacts(packages)
        case .marketing: return await marketingContent(packages)
        case .analytics: return await analyticsProjects(packages)
        default: return nil
        }
    }

    private static func coves() -> CovesModel? {
        guard let repo = ShellCounts.shared.currentRepo else { return nil }
        return CovesFeature.model(for: repo)
    }

    private static func loadCoves() async {
        guard let model = coves(), model.phase == .idle else { return }
        await model.load()
    }

    private static func loadShortcuts() async {
        guard let store = TeammatesFeature.shortcuts, store.catalog == nil,
            !(HostsFeature.hub?.candidates.isEmpty ?? true)
        else { return }
        await store.load(preferred: CodeCurrentRepo.memory.ref?.slug ?? "")
    }

    private static func calories(_ api: ApiService) async -> Int? {
        let model: CaloriesModel = CaloriesModel(api: api)
        await model.load()
        return model.installed && model.error.isEmpty ? Int(model.calories.rounded()) : nil
    }

    private static func steps(_ api: ApiService) async -> Int? {
        let model: StepsModel = StepsModel(api: StepsAPI(api: api), health: nil, prefs: HealthFeature.prefs)
        await model.load(syncIfDue: false)
        return model.installed && model.error.isEmpty ? model.todaySteps : nil
    }

    private static func water(_ api: ApiService) async -> Int? {
        let model: WaterModel = WaterModel(api: WaterAPI(api: api), health: nil, prefs: HealthFeature.prefs)
        await model.load(reconcile: false)
        return model.installed && model.error.isEmpty ? model.totalMl : nil
    }

    private static func sleepHours(_ api: ApiService) async -> Int? {
        let model: SleepModel = SleepModel(api: SleepAPI(api: api), bridge: nil, prefs: HealthFeature.prefs)
        await model.load(syncIfDue: false)
        return model.installed && model.error.isEmpty ? Int((Double(model.lastMinutes) / 60).rounded()) : nil
    }

    private static func sugarFreeDays(_ api: ApiService) async -> Int? {
        let model: SugarBreakModel = SugarBreakModel(api: SugarBreakAPI(api: api), health: nil, prefs: HealthFeature.prefs)
        await model.load()
        return model.isActive && model.error.isEmpty ? model.sugarFreeDays : nil
    }

    private static func stretchSessionsToday(_ api: ApiService) async -> Int? {
        let model: StretchModel = StretchModel(api: StretchAPI(api: api), health: nil, prefs: HealthFeature.prefs)
        await model.load()
        return model.installed && model.error.isEmpty ? model.todayTotal.sessions : nil
    }

    private static func adminUsers(_ api: ApiService) async -> Int? {
        guard InsightsFeature.isAdmin else { return nil }
        return try? await AdminClient(transport: api).overview().users
    }

    private static func installedOrgs(_ list: () async throws -> [PackageOrganization]) async -> [String]? {
        guard let orgs = try? await list() else { return nil }
        return orgs.filter(\.installed).prefix(5).map(\.name)
    }

    private static func crmContacts(_ client: PackagesClient?) async -> Int? {
        guard let client, let orgs = await installedOrgs({ try await client.crmOrganizations() }) else { return nil }
        var total: Int = 0
        for org in orgs {
            guard let data = try? await client.crm(org) else { continue }
            total += data.counts.contacts
        }
        return total
    }

    private static func marketingContent(_ client: PackagesClient?) async -> Int? {
        guard let client, let orgs = await installedOrgs({ try await client.marketingOrganizations() }) else {
            return nil
        }
        var total: Int = 0
        for org in orgs {
            guard let overview = try? await client.marketing(org) else { continue }
            total += overview.content.count
        }
        return total
    }

    private static func analyticsProjects(_ client: PackagesClient?) async -> Int? {
        guard let client, let orgs = await installedOrgs({ try await client.analyticsOrganizations() }) else {
            return nil
        }
        var total: Int = 0
        for org in orgs {
            guard let projects = try? await client.analyticsProjects(org) else { continue }
            total += projects.count
        }
        return total
    }

    private static func notes(_ api: ApiService) async -> Int? {
        let model: NotesListModel = NotesListModel(api: api)
        await model.load()
        return model.error.isEmpty ? model.notes.count : nil
    }

    private static func tasks(_ api: ApiService) async -> Int? {
        let model: TasksListModel = TasksListModel(api: api)
        await model.load()
        return model.error.isEmpty ? model.open.count : nil
    }

    private static func eventsToday(_ api: ApiService) async -> Int? {
        let model: CalendarModel = CalendarModel(client: api, mode: .day)
        await model.load()
        let today: Int64 = CalendarMath.startOfDay(Date().epochMillis, timeZone: model.timeZone)
        return model.occurrences(onDay: today).count
    }
}
