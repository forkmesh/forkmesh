// The state and decisions of the vault share screen: who an item is
// shared with, checking a user's or an organization's keys before sealing, the changed-keys
// confirmation, sharing and stopping a share.

import FMBase
import Foundation
import Observation

@Observable
@MainActor
public final class VaultShareModel {
    public static let intro =
        "The login's key is sealed to each person's own public keys (X25519 and post-quantum ML-KEM-768). ForkMesh passes the sealed key along and cannot open it."
    public static let notShared = "Not shared with anyone."
    public static let lockedTitle = "The vault is locked"
    public static let userSegment = "A ForkMesh user"
    public static let organizationSegment = "An organization"
    public static let usernameLabel = "Username"
    public static let organizationLabel = "Organization"
    public static let readSegment = "View and use"
    public static let writeSegment = "View and edit"
    public static let checkKeys = "Check keys"
    public static let nobodyElse = "Nobody else is in this organization yet."
    public static let confirmChangedLabel = "I compared the changed fingerprint with that person, outside ForkMesh."
    public static let shareButton = "Share"
    public static let stopSharing = "Stop sharing"
    public static let stateLabels: [String: String] = [
        "new": "first share",
        "pinned": "known keys",
        "changed": "KEYS CHANGED",
        "no_vault": "no vault yet",
        "invalid_keys": "invalid keys",
    ]

    @ObservationIgnored public let vault: VaultService
    public let itemId: String

    /// "user" or "organization". Changing it forgets the checked keys.
    public var kind = "user" { didSet { if kind != oldValue { reset() } } }
    /// "read" or "write".
    public var permission = "read" { didSet { if permission != oldValue { reset() } } }
    /// The user name typed.
    public var name = "" { didSet { if name != oldValue { reset() } } }
    /// The organization picked.
    public var organization = "" { didSet { if organization != oldValue { reset() } } }
    public private(set) var organizations: [String] = []
    /// The checked recipients; nil until "Check keys".
    public private(set) var preview: [VaultRecipient]?
    public var confirmedChanged = false
    public private(set) var busy = false
    public private(set) var message = ""

    public init(vault: VaultService, itemId: String) {
        self.vault = vault
        self.itemId = itemId
    }

    /// Nil once the vault locked underneath the screen.
    public var item: VaultItem? { vault.find(itemId) }

    public var screenTitle: String { "Share \(item?.title ?? "")" }

    /// Call when the screen appears: loads the organizations this account belongs to.
    public func start() async {
        guard let names = try? await vault.organizations() else { return }
        organizations = names
        organization = names.first ?? ""
    }

    public var organizationsAvailable: Bool { !organizations.isEmpty }

    public var target: String { kind == "organization" ? organization : name.dartTrimmed.lowercased() }

    public var changed: [String] { (preview ?? []).filter { $0.state == "changed" }.map(\.name) }

    /// Sharing with one user whose keys changed waits for the out-of-band confirmation.
    public var blocked: Bool { kind == "user" && !changed.isEmpty && !confirmedChanged }

    public var canCheck: Bool { !busy && !target.isEmpty }

    public var canShare: Bool { preview != nil && !busy && !blocked }

    private func reset() {
        preview = nil
        confirmedChanged = false
    }

    private func perform(_ work: () async throws -> Void) async {
        busy = true
        message = ""
        defer { busy = false }
        do {
            try await work()
        } catch {
            message = VaultCopy.errorText(error)
        }
    }

    // MARK: Current shares

    public func shareTitle(_ share: VaultShare) -> String {
        "\(share.kind == "organization" ? "Organization " : "")\(share.name)"
    }

    public func shareSubtitle(_ share: VaultShare) -> String {
        share.permission == "write" ? "Can view and edit" : "Can view and use"
    }

    public func stopConfirmation(_ share: VaultShare) -> VaultConfirmation {
        VaultConfirmation(
            title: "Stop sharing?",
            message: "The login is re-encrypted under a new key. If \(share.name) already saw the password, change it on the website too.",
            confirm: Self.stopSharing,
            cancel: VaultCopy.cancel,
            destructive: false)
    }

    /// After the person confirmed `stopConfirmation`.
    public func stop(_ share: VaultShare) async {
        await perform { try await self.vault.unshare(self.itemId, kind: share.kind, name: share.name) }
    }

    // MARK: Checking and sharing

    public func check() async {
        await perform {
            let found = try await self.vault.previewShare(kind: self.kind, name: self.target)
            self.preview = found
            self.confirmedChanged = false
        }
    }

    public func stateLabel(_ state: String) -> String { Self.stateLabels[state] ?? state }

    /// Changed or invalid keys are shown in the danger colour.
    public func isAlarming(_ state: String) -> Bool { state == "changed" || state == "invalid_keys" }

    /// A pinned person's fingerprint is not repeated.
    public func fingerprintShown(_ member: VaultRecipient) -> String? {
        member.fingerprint.isEmpty || member.state == "pinned" ? nil : member.fingerprint
    }

    /// The paragraph under the checked recipients.
    public var advice: String {
        let members = preview ?? []
        var notes = [String]()
        if !changed.isEmpty {
            notes.append("\(changed.joined(separator: ", ")) now presents different keys than you pinned. That happens when someone deletes and recreates their vault, and it is also what an attack looks like. Compare the fingerprint with them before going on.")
        }
        if kind == "user" && members.contains(where: { $0.state == "new" }) {
            notes.append("First share with this person. To be certain these keys are theirs, ask them to read their fingerprint (Vault settings) to you.")
        }
        let without = members.filter { $0.state == "no_vault" }.count
        if without > 0 {
            notes.append("\(without) member\(without == 1 ? " has" : "s have") no vault yet. They get access the next time you unlock after they create one.")
        }
        return notes.joined(separator: " ")
    }

    public func share() async {
        let target = self.target
        let accepted = confirmedChanged ? changed : []
        await perform {
            let outcome = try await self.vault.share(
                self.itemId, kind: self.kind, name: target, permission: self.permission, accepted: accepted)
            self.preview = nil
            let skipped = outcome.skipped.isEmpty ? "" : ", skipped: \(outcome.skipped.joined(separator: ", "))"
            self.message = "Shared with \(target) (\(outcome.sealedTo.count) sealed\(skipped))."
        }
    }
}
