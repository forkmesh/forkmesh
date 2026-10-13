import FMBase
import Foundation
import Observation

@MainActor
@Observable
public final class MarketModel {
    public private(set) var packages: [MarketPackage] = []
    public private(set) var counts: [String: PackageCounts] = [:]
    public private(set) var ratings: [String: RatingSummary] = [:]
    public private(set) var organizations: [OrgPackageState] = []
    public private(set) var loading = false
    public private(set) var errorText = ""
    public private(set) var partialError = ""
    public private(set) var installing: Set<String> = []
    public private(set) var removed: PackageRemoval?
    public var filter: MarketFilter
    public var query = ""

    @ObservationIgnored private let client: PackagesClient
    @ObservationIgnored public let account: AccountPackagesStore?

    public init(client: PackagesClient, filter: MarketFilter = .all, account: AccountPackagesStore? = nil) {
        self.client = client
        self.filter = filter
        self.account = account
    }

    public func refresh(organizations names: [String]) async {
        loading = true
        defer { loading = false }
        do {
            packages = try await client.packages()
            errorText = ""
        } catch {
            errorText = PackagesError.describe(error)
            return
        }
        account?.adoptCatalog(packages)
        await account?.refresh()
        var problems: [String] = []
        if let counts = try? await client.installCounts() { self.counts = counts } else { problems.append("install counts") }
        if let ratings = try? await client.ratingSummaries() { self.ratings = ratings } else { problems.append("ratings") }
        var states: [OrgPackageState] = []
        for name in names.filter(PackagesText.isOrgName).prefix(20) {
            if let state = try? await client.organizationPackages(name) { states.append(state) }
        }
        organizations = states
        partialError = problems.isEmpty ? "" : "Couldn't load " + problems.joined(separator: " and ") + "."
    }

    public func isInstalled(_ package: MarketPackage) -> Bool {
        if package.accountSync { return account?.isInstalled(package.id) ?? package.defaultOn }
        if PhonePackages.isRemoved(package.id) { return false }
        switch package.dataScope {
        case .organization: return organizations.contains { $0.isInstalled(package.id) }
        case .account, .none: return package.bundled && (PhonePackages.isOn(package.id) ?? true)
        }
    }

    public func marketParent(of package: MarketPackage) -> String? {
        guard let parent = package.dependencies.first, parent != package.id,
              packages.contains(where: { $0.id == parent }) else { return nil }
        return parent
    }

    public func nested(in package: MarketPackage) -> [MarketPackage] {
        packages.filter { marketParent(of: $0) == package.id }
    }

    public var visible: [MarketPackage] {
        packages.filter { package in
            if marketParent(of: package) != nil { return false }
            let installed = isInstalled(package)
            switch filter {
            case .all: break
            case .installed: if !installed { return false }
            case .notInstalled: if installed { return false }
            }
            return ([package] + nested(in: package)).contains {
                PackagesText.matches(query, $0.name, $0.summary, $0.category)
            }
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
            if let index = organizations.firstIndex(where: { $0.organization == organization }) {
                organizations[index] = state
            } else {
                organizations.append(state)
            }
            errorText = ""
            return true
        } catch {
            errorText = PackagesError.describe(error)
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
        if let index = organizations.firstIndex(where: { $0.organization == organization }) {
            organizations[index] = organizations[index].removing(package.id)
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
