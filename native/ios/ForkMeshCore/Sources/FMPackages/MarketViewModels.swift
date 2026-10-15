import FMBase
import Foundation
import Observation

@MainActor
@Observable
public final class OrganizationPackagesLedger {
    public static let shared = OrganizationPackagesLedger()

    public private(set) var states: [OrgPackageState] = []
    public private(set) var loaded = false

    public init() {}

    public func replace(_ states: [OrgPackageState]) {
        self.states = states
        loaded = true
    }

    public func update(_ state: OrgPackageState) {
        if let index = states.firstIndex(where: { $0.organization == state.organization }) {
            states[index] = state
        } else {
            states.append(state)
        }
    }

    public func reset() {
        states = []
        loaded = false
    }
}

@MainActor
@Observable
public final class MarketModel {
    public private(set) var packages: [MarketPackage] = []
    public private(set) var counts: [String: PackageCounts] = [:]
    public private(set) var ratings: [String: RatingSummary] = [:]
    public private(set) var usage: [String: PackageUsage] = [:]
    public private(set) var loading = false
    public private(set) var errorText = ""
    public private(set) var partialError = ""
    public private(set) var installing: Set<String> = []
    public private(set) var installErrors: [String: String] = [:]
    public private(set) var removed: PackageRemoval?
    public var filter: MarketFilter
    public var sort: MarketSort = .featured
    public var filters = MarketFilters()
    public var query = ""

    @ObservationIgnored private let client: PackagesClient
    @ObservationIgnored public let account: AccountPackagesStore?
    @ObservationIgnored public let ledger: OrganizationPackagesLedger

    public init(
        client: PackagesClient, filter: MarketFilter = .all, account: AccountPackagesStore? = nil,
        ledger: OrganizationPackagesLedger? = nil
    ) {
        self.client = client
        self.filter = filter
        self.account = account
        self.ledger = ledger ?? OrganizationPackagesLedger()
    }

    public var organizations: [OrgPackageState] { ledger.states }

    public func refresh(organizations names: [String]) async {
        loading = true
        defer { loading = false }
        guard await loadCatalog() else { return }
        var problems: [String] = []
        if let counts = try? await client.installCounts() { self.counts = counts } else { problems.append("install counts") }
        if let summary = try? await client.marketSummary() {
            ratings = summary.ratings
            usage = summary.usage
        } else {
            problems.append("ratings")
        }
        let organizationsLoaded = await loadOrganizations(names)
        if !organizationsLoaded { problems.append("your organizations' packages") }
        partialError = problems.isEmpty ? "" : "Couldn't load " + problems.joined(separator: " and ") + "."
    }

    public func refreshInstalls(organizations names: [String]) async {
        loading = true
        defer { loading = false }
        guard await loadCatalog() else { return }
        let organizationsLoaded = await loadOrganizations(names)
        partialError = organizationsLoaded ? "" : "Couldn't load your organizations' packages."
    }

    private func loadCatalog() async -> Bool {
        do {
            packages = try await client.packages()
            errorText = ""
        } catch {
            errorText = PackagesError.describe(error)
            return false
        }
        account?.adoptCatalog(packages)
        await account?.refresh()
        return true
    }

    private func loadOrganizations(_ names: [String]) async -> Bool {
        var states: [OrgPackageState] = []
        var failed = false
        for name in names.filter(PackagesText.isOrgName).prefix(20) {
            if let state = try? await client.organizationPackages(name) {
                states.append(state)
            } else if let known = ledger.states.first(where: { $0.organization == name }) {
                states.append(known)
                failed = true
            } else {
                failed = true
            }
        }
        ledger.replace(states)
        return !failed
    }

    public func installScope(_ package: MarketPackage) -> MarketInstallScope {
        if package.accountSync { return .account }
        if package.installsPerOrganization { return .organization }
        if PhonePackages.deviceSwitched.contains(package.id) { return .device }
        return .desktop
    }

    public func isInstalled(_ package: MarketPackage) -> Bool {
        if package.accountSync {
            return (account?.isInstalled(package.id) ?? package.defaultOn) && !PhonePackages.isRemoved(package.id)
        }
        if let on = PhonePackages.isOn(package.id) { return on }
        if PhonePackages.isRemoved(package.id) { return false }
        switch package.dataScope {
        case .organization: return organizations.contains { $0.isInstalled(package.id) }
        case .account, .none: return package.defaultOn
        }
    }

    public func cardAction(_ package: MarketPackage, signedIn: Bool) -> MarketCardAction {
        if isBusy(package) { return .busy }
        if isInstalled(package) {
            return PackageRoutes.route(for: package) != nil || !package.webRoute.isEmpty ? .open : .installed
        }
        switch installScope(package) {
        case .account:
            if account?.isInstalled(package.id) == true && PhonePackages.isRemoved(package.id) { return .installPhone }
            return signedIn ? .installAccount : .signIn
        case .device:
            return .installDevice
        case .organization:
            if PhonePackages.isRemoved(package.id) && organizations.contains(where: { $0.isInstalled(package.id) }) {
                return .installPhone
            }
            if !signedIn { return .signIn }
            if organizations.isEmpty { return .createOrganization }
            let targets = installTargets(package).filter { !$0.isInstalled(package.id) }.map(\.organization)
            if targets.isEmpty { return .askAdmin }
            return targets.count == 1 ? .installOrganization(targets[0]) : .chooseOrganization(targets)
        case .desktop:
            return .desktop
        }
    }

    public func uninstallAction(_ package: MarketPackage, signedIn: Bool) -> MarketUninstall? {
        guard isInstalled(package), !isBusy(package) else { return nil }
        if package.accountSync {
            return signedIn && account?.isInstalled(package.id) == true ? .account : nil
        }
        if PhonePackages.isOn(package.id) == true { return .device }
        if PhonePackages.canRemove(package) { return .phone }
        guard package.installsPerOrganization, signedIn else { return nil }
        let managed = organizations.filter { $0.canManage && $0.isInstalled(package.id) }.map(\.organization)
        return managed.isEmpty ? nil : .organization(managed)
    }

    public func isBusy(_ package: MarketPackage) -> Bool {
        account?.isBusy(package.id) == true || installing.contains { $0.hasSuffix("/" + package.id) }
    }

    @discardableResult
    public func installForAccount(_ package: MarketPackage) async -> Bool {
        guard let account else {
            installErrors[package.id] = PackagesError.sentence("sign_in_required")
            return false
        }
        PhonePackages.setRemoved(package.id, removed: false)
        if account.isInstalled(package.id) {
            installErrors[package.id] = nil
            return true
        }
        let changed = await account.install(package.id)
        installErrors[package.id] = changed == nil ? (account.errorText.isEmpty ? nil : account.errorText) : nil
        return changed != nil
    }

    public func dismissInstallError(_ package: MarketPackage) {
        installErrors[package.id] = nil
    }

    public func marketParent(of package: MarketPackage) -> String? {
        guard let parent = package.dependencies.first, parent != package.id,
              packages.contains(where: { $0.id == parent }) else { return nil }
        return parent
    }

    nonisolated static func parent(of package: MarketPackage, among ids: Set<String>) -> String? {
        guard let parent = package.dependencies.first, parent != package.id, ids.contains(parent) else { return nil }
        return parent
    }

    public func nested(in package: MarketPackage) -> [MarketPackage] {
        let ids = Set(packages.map(\.id))
        return packages.filter { MarketModel.parent(of: $0, among: ids) == package.id }
    }

    public var categories: [String] {
        Array(Set(packages.map { $0.category.isEmpty ? "Other" : $0.category })).sorted()
    }

    public var kinds: [String] {
        Array(Set(packages.map(\.type).filter { !$0.isEmpty })).sorted()
    }

    public var publishers: [String] {
        Array(Set(packages.map(\.publisher).filter { !$0.isEmpty })).sorted()
    }

    public var hasPaid: Bool { packages.contains { !$0.free } }

    public func matchesFilters(_ package: MarketPackage) -> Bool {
        if let category = filters.category, (package.category.isEmpty ? "Other" : package.category) != category { return false }
        if let kind = filters.kind, package.type != kind { return false }
        if !package.runs(on: filters.runsOn) { return false }
        switch filters.price {
        case .any: break
        case .free: if !package.free { return false }
        case .paid: if package.free { return false }
        }
        if let scope = filters.scope, installScope(package) != scope { return false }
        if let publisher = filters.publisher, package.publisher != publisher { return false }
        if filters.verifiedOnly && !package.verified { return false }
        if filters.ratedOnly && (ratings[package.id]?.count ?? 0) == 0 { return false }
        return true
    }

    public var visible: [MarketPackage] {
        let ids = Set(packages.map(\.id))
        var children: [String: [MarketPackage]] = [:]
        for package in packages {
            if let parent = MarketModel.parent(of: package, among: ids) { children[parent, default: []].append(package) }
        }
        let rows = packages.filter { package in
            if MarketModel.parent(of: package, among: ids) != nil { return false }
            let installed = isInstalled(package)
            switch filter {
            case .all: break
            case .installed: if !installed { return false }
            case .notInstalled: if installed { return false }
            }
            if !matchesFilters(package) { return false }
            return ([package] + (children[package.id] ?? [])).contains {
                PackagesText.matches(query, $0.name, $0.summary, $0.category, $0.publisher)
            }
        }
        return sorted(rows)
    }

    public func sorted(_ rows: [MarketPackage]) -> [MarketPackage] {
        let order: [String: Int] = Dictionary(packages.enumerated().map { ($1.id, $0) }, uniquingKeysWith: { first, _ in first })
        func key(_ package: MarketPackage) -> Double {
            switch sort {
            case .featured, .name: return 0
            case .rating:
                let rating = ratings[package.id] ?? RatingSummary()
                return rating.count == 0 ? -1 : rating.average * 100_000 + Double(min(rating.count, 99_999))
            case .installs: return Double(counts[package.id]?.total ?? 0)
            case .downloads: return Double(counts[package.id]?.downloads ?? 0)
            case .used:
                let used = usage[package.id] ?? PackageUsage()
                return Double(used.people) * 1_000_000 + Double(min(used.opens, 999_999))
            case .opened: return Double(usage[package.id]?.opens ?? 0)
            case .installedFirst: return isInstalled(package) ? 1 : 0
            }
        }
        return rows.sorted { (left: MarketPackage, right: MarketPackage) -> Bool in
            if sort == .name {
                let compared = left.name.localizedCaseInsensitiveCompare(right.name)
                if compared != .orderedSame { return compared == .orderedAscending }
            } else {
                let a = key(left)
                let b = key(right)
                if a != b { return a > b }
            }
            return (order[left.id] ?? 0) < (order[right.id] ?? 0)
        }
    }

    public var shelves: [(category: String, packages: [MarketPackage])] {
        let rows = visible
        let categories = Array(Set(rows.map { $0.category.isEmpty ? "Other" : $0.category })).sorted()
        return categories.map { category in
            (category: category, packages: rows.filter { ($0.category.isEmpty ? "Other" : $0.category) == category })
        }
    }

    public func package(_ id: String) -> MarketPackage? { packages.first { $0.id == id } }

    public func installTargets(_ package: MarketPackage) -> [OrgPackageState] {
        guard package.installsPerOrganization else { return [] }
        return organizations.filter { $0.canManage }
    }

    @discardableResult
    public func install(_ package: MarketPackage, organization: String) async -> Bool {
        let key = organization + "/" + package.id
        installing.insert(key)
        defer { installing.remove(key) }
        do {
            let state = try await client.install(package.id, organization: organization)
            ledger.update(state)
            PhonePackages.setRemoved(package.id, removed: false)
            errorText = ""
            installErrors[package.id] = nil
            return true
        } catch {
            errorText = PackagesError.describe(error)
            installErrors[package.id] = errorText
            return false
        }
    }

    public func isBusy(_ package: MarketPackage, organization: String) -> Bool {
        installing.contains(organization + "/" + package.id)
    }

    @discardableResult
    public func uninstall(_ package: MarketPackage, organization: String) async -> Bool {
        let key = organization + "/" + package.id
        installing.insert(key)
        defer { installing.remove(key) }
        do {
            removed = try await client.uninstall(package.id, organization: organization)
        } catch let error as PackagesError where error.code == "plugin_not_installed" {
        } catch {
            errorText = PackagesError.describe(error)
            return false
        }
        if let state = organizations.first(where: { $0.organization == organization }) {
            ledger.update(state.removing(package.id))
        }
        errorText = ""
        return true
    }

    public func dismissRemoval() {
        removed = nil
    }
}

@MainActor
@Observable
public final class PackageReviewsModel {
    public let packageId: String
    public private(set) var page: ReviewPage?
    public private(set) var loading = false
    public private(set) var saving = false
    public private(set) var errorText = ""
    public private(set) var serverFields: [String: String] = [:]
    public private(set) var people: PackageActivityPage?
    public var draft = ReviewDraft()

    @ObservationIgnored private let client: PackagesClient

    public init(client: PackagesClient, packageId: String) {
        self.client = client
        self.packageId = packageId
    }

    public var rules: ReviewRules { page?.rules ?? ReviewRules() }

    public var fieldIssues: [String: String] {
        var issues = draft.issues(rules)
        for (key, value) in serverFields where issues[key] == nil { issues[key] = value }
        return issues
    }

    public var canSubmit: Bool { !saving && draft.issues(rules).isEmpty && (page?.viewer.canRate ?? false) }

    public func refresh() async {
        loading = true
        defer { loading = false }
        do {
            let page = try await client.reviews(packageId)
            self.page = page
            if let own = page.viewer.review { draft = ReviewDraft(review: own) }
            errorText = ""
        } catch {
            errorText = PackagesError.describe(error)
        }
    }

    @discardableResult
    public func submit() async -> Bool {
        guard canSubmit else { return false }
        saving = true
        defer { saving = false }
        do {
            _ = try await client.rate(packageId, draft: draft)
            serverFields = [:]
            await refresh()
            return true
        } catch let error as PackagesError {
            serverFields = error.fields
            errorText = error.message
            return false
        } catch {
            errorText = PackagesError.describe(error)
            return false
        }
    }

    public func loadPeople() async {
        guard page?.viewer.canSeePeople == true else {
            people = nil
            return
        }
        people = try? await client.packageActivity(packageId)
    }

    public func quickRate(_ stars: Int) async -> QuickRateOutcome {
        guard (1...5).contains(stars), page?.viewer.canRate == true else { return .failed }
        var next = draft
        next.stars = stars
        if next.needsReview(rules) && !next.issues(rules).isEmpty {
            draft = next
            return .needsReview
        }
        draft = next
        return await submit() ? .submitted : .failed
    }

    public func toggleHelpful(_ review: MarketReview) async {
        do {
            let result = try await client.setHelpful(review.id, helpful: !review.viewerFoundHelpful)
            guard var page, let index = page.reviews.firstIndex(where: { $0.id == review.id }) else { return }
            page.reviews[index].helpful = result.helpful
            page.reviews[index].viewerFoundHelpful = result.mine
            self.page = page
        } catch {
            errorText = PackagesError.describe(error)
        }
    }
}

public enum QuickRateOutcome: Equatable, Sendable {
    case submitted
    case needsReview
    case failed
}
