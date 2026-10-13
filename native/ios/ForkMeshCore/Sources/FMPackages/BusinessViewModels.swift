import FMBase
import Foundation
import Observation

@MainActor
@Observable
public final class OrgPackageModel<Payload: Sendable> {
    public private(set) var organizations: [PackageOrganization] = []
    public private(set) var payload: Payload?
    public private(set) var loading = false
    public var errorText = ""
    public var organization = ""

    @ObservationIgnored private let listOrganizations: @MainActor () async throws -> [PackageOrganization]
    @ObservationIgnored private let loadPayload: @MainActor (String) async throws -> Payload

    public init(
        organization: String = "", organizations: @escaping @MainActor () async throws -> [PackageOrganization],
        load: @escaping @MainActor (String) async throws -> Payload
    ) {
        self.organization = organization
        listOrganizations = organizations
        loadPayload = load
    }

    public var installedOrganizations: [PackageOrganization] { organizations.filter(\.installed) }

    public func refresh() async {
        loading = true
        defer { loading = false }
        do {
            organizations = try await listOrganizations()
            if organization.isEmpty || !organizations.contains(where: { $0.name == organization }) {
                organization = installedOrganizations.first?.name ?? ""
            }
            errorText = ""
        } catch {
            errorText = PackagesError.describe(error)
            return
        }
        await load()
    }

    public func load() async {
        guard PackagesText.isOrgName(organization) else {
            payload = nil
            return
        }
        do {
            payload = try await loadPayload(organization)
            errorText = ""
        } catch {
            errorText = PackagesError.describe(error)
        }
    }

    public func update(_ change: (inout Payload) -> Void) {
        guard var current = payload else { return }
        change(&current)
        payload = current
    }
}

@MainActor
@Observable
public final class AdsModel {
    public private(set) var isReviewer = false
    public private(set) var checked = false
    public private(set) var queue: AdReviewQueue?
    public private(set) var loading = false
    public private(set) var working: Set<String> = []
    public private(set) var errorText = ""
    public var reviewFilter = "pending"

    @ObservationIgnored private let client: PackagesClient

    public init(client: PackagesClient) {
        self.client = client
    }

    public func refresh() async {
        loading = true
        defer { loading = false }
        do {
            isReviewer = try await client.isAdReviewer()
            checked = true
            errorText = ""
        } catch {
            errorText = PackagesError.describe(error)
            return
        }
        if isReviewer { await loadQueue() }
    }

    public func loadQueue() async {
        guard isReviewer else {
            queue = nil
            return
        }
        do {
            queue = try await client.adReviewQueue(status: reviewFilter)
        } catch {
            errorText = PackagesError.describe(error)
        }
    }

    @discardableResult
    public func review(_ ad: OwnedAd, decision: AdReviewDecision) async -> Bool {
        guard isReviewer else { return false }
        working.insert(ad.id)
        defer { working.remove(ad.id) }
        do {
            try await client.reviewAd(ad, decision: decision)
            queue?.ads.removeAll { $0.id == ad.id }
            errorText = ""
            return true
        } catch {
            errorText = PackagesError.describe(error)
            return false
        }
    }
}

@MainActor
@Observable
public final class LedgerModel {
    public private(set) var entries: [LedgerEntry] = []
    public private(set) var solUsd = ""
    public private(set) var next: LedgerCursor?
    public private(set) var rewards: [RewardEstimate] = []
    public private(set) var records: [PersonalRecord] = []
    public private(set) var loading = false
    public private(set) var errorText = ""

    @ObservationIgnored private let client: PackagesClient
    @ObservationIgnored private let store: PersonalLedgerStore

    public init(client: PackagesClient, store: PersonalLedgerStore) {
        self.client = client
        self.store = store
        records = store.load()
    }

    public var pendingRewards: [RewardEstimate] { rewards.filter { !$0.paid } }

    public var paidRewards: [RewardEstimate] { rewards.filter(\.paid) }

    public func refresh() async {
        loading = true
        defer { loading = false }
        do {
            let page = try await client.ledger()
            entries = page.entries
            solUsd = page.solUsd
            next = page.next
            errorText = ""
        } catch {
            errorText = PackagesError.describe(error)
        }
        rewards = (try? await client.rewardEstimates()) ?? rewards
    }

    public func loadMore() async {
        guard let cursor = next, !loading else { return }
        loading = true
        defer { loading = false }
        do {
            let page = try await client.ledger(after: cursor)
            let known = Set(entries.map(\.id))
            entries += page.entries.filter { !known.contains($0.id) }
            next = page.next
        } catch {
            errorText = PackagesError.describe(error)
        }
    }

    public func add(_ record: PersonalRecord) {
        records = store.save(records.filter { $0.id != record.id } + [record])
    }

    public func remove(_ id: String) {
        records = store.save(records.filter { $0.id != id })
    }

    public func toggleSettled(_ id: String) {
        records = store.save(records.map { record in
            var copy = record
            if copy.id == id { copy.settled.toggle() }
            return copy
        })
    }

    public var totals: [(currency: String, income: Int64, expense: Int64)] { PersonalLedgerStore.totals(records) }
}
