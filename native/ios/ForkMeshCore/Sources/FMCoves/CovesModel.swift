import FMApi
import FMBase
import Foundation
import Observation

public struct CoveFileRef: Equatable, Sendable {
    public var path: String
    public var size: Int

    public init(path: String, size: Int = 0) {
        self.path = path
        self.size = size
    }
}

public struct CoveAccountCheck: Equatable, Sendable {
    public var exists: Bool
    public var active: Bool

    public init(exists: Bool, active: Bool) {
        self.exists = exists
        self.active = active
    }
}

public struct CoveRepositoryIO {
    public var listFiles: @MainActor (_ repo: RepoRef) async throws -> [CoveFileRef]
    public var readFile: @MainActor (_ repo: RepoRef, _ path: String, _ maxBytes: Int) async throws -> [UInt8]
    public var propose: @MainActor (_ repo: RepoRef, _ title: String, _ head: String, _ patch: String, _ description: String) async throws -> Void
    public var checkAccount: (@MainActor (_ name: String) async -> CoveAccountCheck)?

    public init(
        listFiles: @escaping @MainActor (_ repo: RepoRef) async throws -> [CoveFileRef],
        readFile: @escaping @MainActor (_ repo: RepoRef, _ path: String, _ maxBytes: Int) async throws -> [UInt8],
        propose: @escaping @MainActor (_ repo: RepoRef, _ title: String, _ head: String, _ patch: String, _ description: String) async throws -> Void,
        checkAccount: (@MainActor (_ name: String) async -> CoveAccountCheck)? = nil
    ) {
        self.listFiles = listFiles
        self.readFile = readFile
        self.propose = propose
        self.checkAccount = checkAccount
    }
}

public struct CoveEntry: Equatable, Sendable, Identifiable {
    public var cove: Cove
    public var servedBytes: [UInt8]?
    public var pendingReview: Bool

    public var id: String { cove.id }
}

public enum CovesPhase: Equatable, Sendable {
    case idle, loading, loaded, failed(String)
}

public enum CoveActionError: Error, Equatable, CustomStringConvertible {
    case signInRequired
    case notFound
    case locked
    case onlyCreatorShares
    case notAccountCove
    case alreadyMember(String)
    case invalidAccount(String)
    case inactiveAccount(String)
    case documentExists(String)
    case store(String)
    case submit(String)

    public var description: String {
        switch self {
        case .signInRequired: return "Sign in with a ForkMesh account."
        case .notFound: return "That cove is no longer in this repository."
        case .locked: return "Unlock the cove first."
        case .onlyCreatorShares: return "Only the cove creator can change sharing."
        case .notAccountCove: return "Password coves are shared by passing the password out of band."
        case .alreadyMember(let name): return "@\(name) already has access to this cove."
        case .invalidAccount(let name): return "\(name) is not a ForkMesh account."
        case .inactiveAccount(let name): return "\(name) is not an active ForkMesh account."
        case .documentExists(let name): return "A document named \(name) already exists."
        case .store(let message): return message
        case .submit(let message): return message
        }
    }
}

@MainActor
@Observable
public final class CovesModel {
    public static let maxCoves = 200
    public static let maxEnvelopeBytes = 6 * 1024 * 1024

    public let repo: RepoRef
    public private(set) var phase: CovesPhase = .idle
    public private(set) var entries: [CoveEntry] = []
    public private(set) var skipped = 0
    public private(set) var busy = false
    public var account: String
    public var openerKey: String
    @ObservationIgnored public var notifier: CoveNotifier?

    @ObservationIgnored private let io: CoveRepositoryIO
    @ObservationIgnored private var announcedOpens: Set<String> = []
    @ObservationIgnored private var passwords: [String: String] = [:]
    @ObservationIgnored private let clock: () -> Date
    @ObservationIgnored let rounds: Int

    public init(
        repo: RepoRef, account: String, openerKey: String = "", io: CoveRepositoryIO,
        rounds: Int = CoveCrypto.defaultRounds, clock: @escaping () -> Date = Date.init
    ) {
        self.repo = repo
        self.account = account
        self.openerKey = openerKey
        self.io = io
        self.rounds = rounds
        self.clock = clock
    }

    public var unlockedCoves: [Cove] { entries.map(\.cove).filter(\.unlocked) }
    public var lockedCoves: [Cove] { entries.map(\.cove).filter { !$0.unlocked } }

    public func entry(_ id: String) -> CoveEntry? { entries.first { $0.cove.id == id } }
    public func cove(_ id: String) -> Cove? { entry(id)?.cove }

    public func shareLink(_ id: String) -> String {
        CoveFiles.shareUrl(owner: repo.owner, repository: repo.name, coveId: id)
    }

    private var nowMs: Int64 { Int64((clock().timeIntervalSince1970 * 1000).rounded()) }

    public func load() async {
        phase = .loading
        do {
            let files = try await io.listFiles(repo).filter { $0.path.hasSuffix(".cove") }.prefix(CovesModel.maxCoves)
            var loaded: [CoveEntry] = []
            var skipped = 0
            for file in files {
                if file.size > CovesModel.maxEnvelopeBytes {
                    skipped += 1
                    continue
                }
                guard let bytes = try? await io.readFile(repo, file.path, CovesModel.maxEnvelopeBytes),
                    let cove = CoveStore.parseEnvelope(bytes, path: file.path)
                else {
                    skipped += 1
                    continue
                }
                let previous = entries.first { $0.cove.id == cove.id }
                if let previous, previous.cove.unlocked, previous.servedBytes == bytes {
                    loaded.append(previous)
                } else {
                    loaded.append(CoveEntry(cove: cove, servedBytes: bytes, pendingReview: false))
                }
            }
            let listed = Set(loaded.map(\.cove.id))
            loaded += entries.filter { $0.servedBytes == nil && $0.pendingReview && !listed.contains($0.cove.id) }
            entries = loaded
            self.skipped = skipped
            phase = .loaded
            await unlockAccountCoves()
            await reopenPasswordCoves()
        } catch {
            phase = .failed("\(error)")
        }
    }

    public func unlockAccountCoves() async {
        let me = CoveStore.normalizedAccount(account)
        if me.isEmpty { return }
        for index in entries.indices where !entries[index].cove.unlocked {
            let candidate = entries[index].cove
            let opened: Cove? = await Task.detached(priority: .userInitiated) {
                var copy = candidate
                return CoveStore.unlockForAccount(&copy, account: me) ? copy : nil
            }.value
            if let opened, index < entries.count, entries[index].cove.id == opened.id {
                entries[index].cove = opened
            }
        }
    }

    private func reopenPasswordCoves() async {
        for index in entries.indices where !entries[index].cove.unlocked {
            guard let password = passwords[entries[index].cove.id] else { continue }
            let candidate = entries[index].cove
            let opened: Cove? = await Task.detached(priority: .userInitiated) {
                var copy = candidate
                return CoveStore.unlock(&copy, password: password) ? copy : nil
            }.value
            if let opened, index < entries.count, entries[index].cove.id == opened.id { entries[index].cove = opened }
        }
    }

    @discardableResult
    public func unlock(_ id: String, password: String) async -> Bool {
        guard let index = entries.firstIndex(where: { $0.cove.id == id }), !password.isEmpty else { return false }
        let candidate = entries[index].cove
        let opened: Cove? = await Task.detached(priority: .userInitiated) {
            var copy = candidate
            return CoveStore.unlock(&copy, password: password) ? copy : nil
        }.value
        guard let opened, let current = entries.firstIndex(where: { $0.cove.id == id }) else { return false }
        entries[current].cove = opened
        passwords[id] = password
        return true
    }

    @discardableResult
    public func noteOpened(_ id: String) -> Bool {
        guard let cove = cove(id), CoveNotices.shouldAnnounceOpen(cove, openerKey: openerKey),
              !announcedOpens.contains(id), let notifier
        else { return false }
        announcedOpens.insert(id)
        let notice = CoveOpenNotice(
            creatorKey: cove.creator, coveId: cove.id, coveName: cove.name, openerKey: openerKey,
            openerName: CoveStore.normalizedAccount(account), ts: nowMs)
        return notifier.opened(notice)
    }

    public func lock(_ id: String) {
        guard let index = entries.firstIndex(where: { $0.cove.id == id }) else { return }
        passwords[id] = nil
        announcedOpens.remove(id)
        guard var locked = CoveStore.parseEnvelope(entries[index].servedBytes ?? [], path: entries[index].cove.relPath) else {
            entries[index].cove.documents = []
            entries[index].cove.accessLog = []
            entries[index].cove.contentKey = []
            entries[index].cove.unlocked = false
            return
        }
        locked.unlocked = false
        entries[index].cove = locked
    }

    public func lockAll() {
        for entry in entries { lock(entry.cove.id) }
        passwords.removeAll()
    }

    public func forget() {
        passwords.removeAll()
        announcedOpens.removeAll()
        entries = []
        phase = .idle
    }

    public func createAccountCove(name: String, notifyOnOpen: Bool = false) async throws -> Cove {
        let me = CoveStore.normalizedAccount(account)
        if me.isEmpty { throw CoveActionError.signInRequired }
        let creator = openerKey
        let now = nowMs
        let rounds = self.rounds
        let sealed: Cove = try await Task.detached(priority: .userInitiated) {
            try CoveStore.newAccountCove(
                name: name, creatorAccount: me, invitedAccounts: [me], creator: creator, notifyOnOpen: notifyOnOpen,
                nowMs: now, rounds: rounds)
        }.value
        try await publish(sealed, previous: nil, password: nil)
        return sealed
    }

    public func createPasswordCove(name: String, password: String, notifyOnOpen: Bool = true) async throws -> Cove {
        if CoveStore.normalizedAccount(account).isEmpty { throw CoveActionError.signInRequired }
        let creator = openerKey
        let now = nowMs
        let rounds = self.rounds
        let sealed: Cove = try await Task.detached(priority: .userInitiated) {
            try CoveStore.newPasswordCove(
                name: name, password: password, creator: creator, notifyOnOpen: notifyOnOpen, nowMs: now, rounds: rounds)
        }.value
        try await publish(sealed, previous: nil, password: password)
        passwords[sealed.id] = password
        return sealed
    }

    public func saveDocument(coveId: String, name: String, body: String) async throws {
        try await mutate(coveId, action: "edit") { cove in
            let wanted = CoveStore.cleanPath(name)
            guard let index = cove.documents.firstIndex(where: { CoveStore.cleanPath($0.name) == wanted }) else {
                throw CoveActionError.notFound
            }
            cove.documents[index].body = body
            cove.documents[index].updatedAtMs = self.nowMs
        }
    }

    public func addDocument(coveId: String, template: CoveFileTemplate, name: String = "") async throws -> String {
        var created = ""
        try await mutate(coveId, action: "edit") { cove in
            let wanted = name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? template.defaultPath : name
            let unique = CoveFiles.uniqueName(wanted, existing: cove.documents.map(\.name))
            created = unique
            cove.documents.append(CoveDocument(
                id: "doc-\(self.nowMs)", name: unique, mime: template.mime, body: template.body, updatedAtMs: self.nowMs))
        }
        return created
    }

    public func renameDocument(coveId: String, from: String, to: String) async throws {
        try await mutate(coveId, action: "edit") { cove in
            let source = CoveStore.cleanPath(from)
            let target = CoveStore.cleanPath(to.trimmingCharacters(in: .whitespacesAndNewlines))
            if target.isEmpty || target == source { return }
            if cove.documents.contains(where: { CoveStore.cleanPath($0.name).lowercased() == target.lowercased() }) {
                throw CoveActionError.documentExists(target)
            }
            guard let index = cove.documents.firstIndex(where: { CoveStore.cleanPath($0.name) == source }) else {
                throw CoveActionError.notFound
            }
            cove.documents[index].name = target
            cove.documents[index].updatedAtMs = self.nowMs
        }
    }

    public func deleteDocument(coveId: String, name: String) async throws {
        try await mutate(coveId, action: "edit") { cove in
            let wanted = CoveStore.cleanPath(name)
            cove.documents.removeAll { CoveStore.cleanPath($0.name) == wanted }
        }
    }

    @discardableResult
    public func invite(coveId: String, account invitee: String) async throws -> CoveInviteDelivery {
        let me = CoveStore.normalizedAccount(account)
        let grantee = CoveStore.normalizedAccount(invitee)
        guard let cove = cove(coveId) else { throw CoveActionError.notFound }
        if !cove.unlocked { throw CoveActionError.locked }
        if !cove.accountScoped { throw CoveActionError.notAccountCove }
        if !cove.createdBy(account: me) { throw CoveActionError.onlyCreatorShares }
        if grantee.isEmpty { throw CoveActionError.invalidAccount(invitee) }
        if grantee == me || cove.members.contains(grantee) { throw CoveActionError.alreadyMember(grantee) }
        if let check = io.checkAccount {
            let result = await check(grantee)
            if !result.exists { throw CoveActionError.invalidAccount(grantee) }
            if !result.active { throw CoveActionError.inactiveAccount(grantee) }
        }
        try await mutate(coveId, action: nil) { cove in
            var invited = cove.invitedAccounts
            invited.append(grantee)
            cove.invitedAccounts = CoveStore.normalizedAccounts(invited)
        }
        let link = shareLink(coveId)
        let notice = CoveInviteNotice(
            inviteeAccount: grantee, coveId: cove.id, coveName: cove.name, inviterName: me, shareUrl: link, ts: nowMs)
        if let notifier, await notifier.invite(notice) { return .relayed }
        return .shareLink(link)
    }

    private func mutate(_ coveId: String, action: String?, _ change: (inout Cove) throws -> Void) async throws {
        guard let index = entries.firstIndex(where: { $0.cove.id == coveId }) else { throw CoveActionError.notFound }
        var updated = entries[index].cove
        if !updated.unlocked { throw CoveActionError.locked }
        try change(&updated)
        if let action {
            CoveStore.appendAccess(&updated, CoveAccessEntry(who: openerKey, name: CoveStore.normalizedAccount(account), ts: nowMs, action: action))
        }
        let password = updated.accountScoped ? nil : passwords[coveId]
        if !updated.accountScoped && password == nil { throw CoveActionError.locked }
        let draft = updated
        let sealed: Cove
        do {
            sealed = try await Task.detached(priority: .userInitiated) {
                if let password { return try CoveStore.sealPassword(draft, password: password) }
                return try CoveStore.sealAccount(draft)
            }.value
        } catch let error as CoveStoreError {
            throw CoveActionError.store(error.description)
        }
        try await publish(sealed, previous: entries[index].servedBytes, password: password)
    }

    private func publish(_ sealed: Cove, previous: [UInt8]?, password: String?) async throws {
        let text = CoveStore.envelopeText(sealed)
        let patch = CovePatch.replaceFile(path: sealed.relPath, before: previous, after: Array(text.utf8))
        if patch.isEmpty { return }
        busy = true
        defer { busy = false }
        let title = "cove: \(sealed.slug)"
        let head = CovePatch.head(account: account, slug: sealed.slug, now: clock())
        do {
            try await io.propose(repo, title, head, patch, "An encrypted cove change proposed from ForkMesh on iPhone.")
        } catch {
            throw CoveActionError.submit("\(error)")
        }
        var unlocked = sealed
        unlocked.unlocked = true
        if let index = entries.firstIndex(where: { $0.cove.id == sealed.id }) {
            entries[index].cove = unlocked
            entries[index].pendingReview = true
        } else {
            entries.append(CoveEntry(cove: unlocked, servedBytes: previous, pendingReview: true))
        }
        if let password { passwords[sealed.id] = password }
    }
}
