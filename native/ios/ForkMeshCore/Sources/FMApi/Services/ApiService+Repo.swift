// ApiService, part 6: repository reads (tree, blob, raw bytes,
// history, commits, compare, branches, search, mirrors, releases, federated replies) with their
// public-route fallback and caches, and the published-record readers for issues, pulls and
// discussions (`.forkmesh/...` folders on the repository itself).

import FMAccount
import FMBase
import FMNet
import Foundation

extension ApiService {
    /// Pull records live only on the `forkmesh/pulls` ledger branch.
    nonisolated public static let pullLedgerRef = "forkmesh/pulls"
    nonisolated public static let blobBatch = 50
    nonisolated static let blobFallbackWidth = 2
    nonisolated static let recordWidth = 4

    // MARK: Routing

    /// The owner to address mirror-served reads by: the public route (organization alias or the
    /// upstream a mirror copies) when the catalog folded this row under one, else `owner`.
    func readOwner(_ owner: String, _ name: String) -> String {
        publicRouteOwners["\(owner.lowercased())/\(name.lowercased())"] ?? owner
    }

    /// `GET /api/repo/<owner>/<name>/<operation>` for a mirror-served read: the public route
    /// first, the serving node itself when that fails (until the public route has answered once).
    func getRepoRead(_ owner: String, _ name: String, _ operation: String, _ query: [(String, String)] = []) async throws -> JSONValue {
        let publicOwner = readOwner(owner, name)
        if publicOwner == owner { return try await getJson(url("/api/repo/\(owner)/\(name)/\(operation)", query)) }
        let key = "\(owner.lowercased())/\(name.lowercased())"
        let publicURL = url("/api/repo/\(publicOwner)/\(name)/\(operation)", query)
        if publicRouteProven.contains(key) { return try await getJson(publicURL) }
        if let data = try? await getJson(publicURL) {
            publicRouteProven.insert(key)
            return data
        }
        // Not every public name is routable; the serving node may still be.
        return try await getJson(url("/api/repo/\(owner)/\(name)/\(operation)", query))
    }

    /// `/api/repo/<owner>/<name>/raw?path=&ref=` by the public route.
    public func rawURL(_ owner: String, _ name: String, _ path: String, ref: String = "") -> URL {
        var query: [(String, String)] = [("path", path)]
        if !ref.isEmpty { query.append(("ref", ref)) }
        return url("/api/repo/\(readOwner(owner, name))/\(name)/raw", query)
    }

    /// A file's bytes exactly as the serving node holds them (for git blob ids). Uncached; 12 s.
    /// Throws `ApiException` for a non-200 answer or more than `maxBytes`.
    public func rawBytes(_ owner: String, _ name: String, _ path: String, ref: String = "", maxBytes: Int = 1024 * 1024) async throws -> Data {
        let response = try await http.send(HTTPRequest(
            method: "GET", url: rawURL(owner, name, path, ref: ref), headers: withAuth(["Accept": "*/*"]), timeout: 12))
        if response.status != 200 { throw ApiException("Could not read \(path) (HTTP \(response.status)).") }
        if response.body.count > maxBytes { throw ApiException("\(path) is too large to edit on this device.") }
        return response.body
    }

    /// Smart-HTTP clone endpoint for a repository on the app origin (`https://host/<owner>/<repo>`).
    public func cloneURL(_ owner: String, _ name: String) -> URL { url("/\(owner)/\(name)") }

    // MARK: Collaboration lists (served)

    public func issues(_ owner: String, _ name: String) async throws -> [Issue] {
        let data = try await getJson(url("/api/repo/\(owner)/\(name)/issues"))
        return ApiService.asList(data).compactMap(\.objectValue).map(Issue.init(json:))
    }

    public func pulls(_ owner: String, _ name: String) async throws -> [PullRequest] {
        let data = try await getJson(url("/api/repo/\(owner)/\(name)/pulls"))
        return ApiService.asList(data).compactMap(\.objectValue).map(PullRequest.init(json:))
    }

    /// Remote ActivityPub replies for a native thread (`kind`: issue, pull or discussion). Cached.
    public func federatedReplies(_ owner: String, _ name: String, kind: String, number: Int) async throws -> [FederatedReply] {
        try await cached(fediRepliesCache, "\(owner)/\(name):\(kind):\(number)") {
            if !["issue", "pull", "discussion"].contains(kind) || number <= 0 { return [] }
            let data = try await self.getJson(self.url(
                "/api/repo/\(owner)/\(name)/fedi-comments", [("kind", kind), ("number", "\(number)")]))
            let raw = data.objectValue?["comments"]?.arrayValue ?? data.objectValue?["items"]?.arrayValue ?? []
            return raw.compactMap(\.objectValue).map(FederatedReply.init(json:))
                .filter { !$0.remoteId.isEmpty || !$0.backlink.isEmpty }
        }
    }

    // MARK: Git reads

    /// Commit history (`history`), raw rows. Cached.
    public func commits(_ owner: String, _ name: String) async throws -> [JSONObject] {
        try await cached(commitsCache, "\(owner)/\(name)") {
            ApiService.asList(try await self.getRepoRead(owner, name, "history")).compactMap(\.objectValue)
        }
    }

    public func historyRead(_ owner: String, _ name: String, ref: String) async throws -> JSONValue {
        try await getRepoRead(owner, name, "history", ref.isEmpty ? [] : [("ref", ref)])
    }

    public func serverVersion() async throws -> JSONObject {
        try await getJson(url("/api/version")).objectValue ?? [:]
    }

    public func commitDetail(_ owner: String, _ name: String, _ hash: String, fallbackCommit: JSONObject = JSONObject()) async throws -> RepoCommitDetail {
        try await cached(commitDetailCache, "\(owner)/\(name):\(hash)") {
            RepoCommitDetail(json: try await self.getRepoRead(owner, name, "commit", [("path", hash)]), fallbackCommit: fallbackCommit)
        }
    }

    /// What `head` adds on top of `base` (branch names or commit ids).
    public func compare(_ owner: String, _ name: String, base: String, head: String) async throws -> RepoComparison {
        RepoComparison(json: try await getRepoRead(owner, name, "compare", [("base", base), ("head", head)]))
    }

    /// A directory listing. Cached; fails fast while the repo host is marked offline.
    public func tree(_ owner: String, _ name: String, path: String = "", ref: String = "") async throws -> RepoTree {
        let repoKey = "\(owner)/\(name)"
        if let offline = repoOfflineError(repoKey) { throw offline }
        return try await cached(treeCache, "\(repoKey):\(ref):\(path)") {
            var query: [(String, String)] = [("path", path)]
            if !ref.isEmpty { query.append(("ref", ref)) }
            return RepoTree(json: try await self.getRepoRead(owner, name, "tree", query), path: path)
        }
    }

    /// A file's content. Cached; fails fast while the repo host is marked offline.
    public func blob(_ owner: String, _ name: String, _ path: String, ref: String = "") async throws -> RepoBlob {
        let repoKey = "\(owner)/\(name)"
        if let offline = repoOfflineError(repoKey) { throw offline }
        return try await cached(blobCache, "\(repoKey):\(ref):\(path)") {
            var query: [(String, String)] = [("path", path)]
            if !ref.isEmpty { query.append(("ref", ref)) }
            return RepoBlob(json: try await self.getRepoRead(owner, name, "blob", query), path: path)
        }
    }

    public func blobs(_ owner: String, _ name: String, _ paths: [String], ref: String = "") async -> [String: RepoBlob] {
        let repoKey = "\(owner)/\(name)"
        if repoOfflineError(repoKey) != nil || paths.isEmpty { return [:] }
        var found: [String: RepoBlob] = [:]
        let now = clock.now
        for path in paths {
            guard let task = blobCache.fresh("\(repoKey):\(ref):\(path)", ttl: ApiService.cacheTtl, now: now, epoch: relayEpoch),
                  let blob = try? await task.value else { continue }
            found[path] = blob
        }
        let wanted = paths.filter { found[$0] == nil }
        for start in stride(from: 0, to: wanted.count, by: ApiService.blobBatch) {
            let chunk = Array(wanted[start..<min(start + ApiService.blobBatch, wanted.count)])
            var query = chunk.map { ("path", $0) }
            if !ref.isEmpty { query.append(("ref", ref)) }
            guard let data = try? await getRepoRead(owner, name, "blobs", query),
                  let map = data.objectValue?["blobs"]?.objectValue else { continue }
            for path in chunk {
                guard let value = map[path], value.objectValue != nil || value.stringValue != nil else { continue }
                let blob = RepoBlob(json: value, path: path)
                found[path] = blob
                _ = blobCache.load("\(repoKey):\(ref):\(path)", now: clock.now, epoch: relayEpoch) { blob }
            }
        }
        let missing = paths.filter { found[$0] == nil }
        let retried: [RepoBlob?] = await ApiService.parallelMap(missing, width: ApiService.blobFallbackWidth) { path in
            try? await self.blob(owner, name, path, ref: ref)
        }
        for (path, blob) in zip(missing, retried) { if let blob { found[path] = blob } }
        return found
    }

    public func branches(_ owner: String, _ name: String) async throws -> [RepoBranch] {
        let data = try await getRepoRead(owner, name, "branches")
        return ApiService.asList(data).compactMap(\.objectValue).map(RepoBranch.init(json:)).filter { !$0.name.isEmpty }
    }

    public func remoteForks(_ owner: String, _ name: String) async throws -> [RemoteFork] {
        RemoteFork.list(try await getRepoRead(owner, name, "remote-forks"))
    }

    public var instanceHost: String {
        let server = ServerURL.parse(settings.serverUrl)
        guard let port = server.port, port != 443 else { return server.host }
        return "\(server.host):\(port)"
    }

    public func searchRepo(_ owner: String, _ name: String, _ query: String, ref: String = "") async throws -> RepoSearchResults {
        var params: [(String, String)] = [("q", query)]
        if !ref.isEmpty { params.append(("ref", ref)) }
        return RepoSearchResults(json: try await getRepoRead(owner, name, "search", params))
    }

    public func mirrors(_ owner: String, _ name: String) async throws -> [RepoMirror] {
        try await mirrorSet(owner, name).mirrors
    }

    /// Mirror rows plus the repository's forkmesh-sync-status-v1 status. Cached.
    public func mirrorSet(_ owner: String, _ name: String) async throws -> RepoMirrorSet {
        try await cached(mirrorSetCache, "\(owner)/\(name)") {
            let data = try await self.getJson(self.url("/api/repo/\(owner)/\(name)/mirrors"))
            return RepoMirrorSet(
                mirrors: ApiService.asList(data).compactMap(\.objectValue).map(RepoMirror.init(json:)).filter { !$0.label.isEmpty },
                syncStatus: data.objectValue.map(SyncStatus.mirrorsPayloadStatus))
        }
    }

    public func freshMirrorSet(_ owner: String, _ name: String) async throws -> RepoMirrorSet {
        mirrorSetCache.remove("\(owner)/\(name)")
        return try await mirrorSet(owner, name)
    }

    /// Releases published under `.forkmesh/releases/<dir>/release.json`, newest first, with
    /// download counts. Cached; an unreadable release folder is skipped.
    public func releases(_ owner: String, _ name: String) async throws -> [RepoRelease] {
        try await cached(releasesCache, "\(owner)/\(name)") {
            let downloads = await self.releaseDownloadCounts(owner, name)
            let root: RepoTree
            do {
                root = RepoTree(
                    json: try await self.getRepoRead(owner, name, "tree", [("path", ".forkmesh/releases")]),
                    path: ".forkmesh/releases")
            } catch {
                return []
            }
            let dirs = root.entries.filter(\.isDirectory).sorted { $0.name < $1.name }
            let paths = dirs.map { ".forkmesh/releases/\($0.name)/release.json" }
            let blobs = await self.blobs(owner, name, paths)
            let manifests = paths.compactMap { blobs[$0]?.content }
            return await Task.detached(priority: .utility) {
                manifests.compactMap { text in
                    (try? JSON.parse(text))?.objectValue.map { RepoRelease(json: $0, downloads: downloads) }
                }.sorted { a, b in
                    if a.createdAtMs != b.createdAtMs { return a.createdAtMs > b.createdAtMs }
                    return a.tag > b.tag
                }
            }.value
        }
    }

    func releaseDownloadCounts(_ owner: String, _ name: String) async -> [String: Int] {
        do {
            let data = try await getJson(url("/api/repo/\(owner)/\(name)/releases/downloads"))
            let raw = data.objectValue?["counts"]?.objectValue ?? data.objectValue ?? JSONObject()
            var out: [String: Int] = [:]
            for (key, value) in raw { out[key] = ModelJSON.int(value) }
            return out
        } catch {
            offlineRepoUntil.removeValue(forKey: "\(owner)/\(name)")
            return [:]
        }
    }

    // MARK: Published records

    /// Issues published under `.forkmesh/issues/{open,closed}/<n>/issue-<n>.json` (or the legacy
    /// `<n>/` layout, or pre-JSON `issues/<n>/issue.md`), newest number first. Cached.
    public func publishedIssues(_ owner: String, _ name: String) async throws -> [Issue] {
        try await cached(issuesCache, "\(owner)/\(name)") {
            var order: [String] = []
            var dirByNumber: [String: String] = [:]
            func add(_ number: String, _ path: String) {
                if dirByNumber[number] == nil { order.append(number) }
                dirByNumber[number] = path
            }
            for dir in await self.numberedFolders(owner, name, ".forkmesh/issues") { add(dir, ".forkmesh/issues/\(dir)") }
            for sub in ["open", "closed"] {
                for dir in await self.numberedFolders(owner, name, ".forkmesh/issues/\(sub)") {
                    add(dir, ".forkmesh/issues/\(sub)/\(dir)")
                }
            }
            if dirByNumber.isEmpty { return await self.legacyMarkdownIssues(owner, name) }
            let entries = order.map { ($0, "\(dirByNumber[$0] ?? "")/issue-\($0).json") }
            let blobs = await self.blobs(owner, name, entries.map(\.1))
            let records = entries.compactMap { entry in blobs[entry.1].map { (entry.0, $0.content) } }
            let parse = self.issueRecord
            return await Task.detached(priority: .utility) {
                records.compactMap { parse($0.0, $0.1) }.sorted { $0.number > $1.number }
            }.value
        }
    }

    func legacyMarkdownIssues(_ owner: String, _ name: String) async -> [Issue] {
        let dirs = await numberedFolders(owner, name, "issues")
        let blobs = await self.blobs(owner, name, dirs.map { "issues/\($0)/issue.md" })
        let items: [Issue?] = await ApiService.parallelMap(dirs, width: ApiService.recordWidth) { dir in
            guard let blob = blobs["issues/\(dir)/issue.md"] else { return nil }
            let events = await self.recordEvents(owner, name, "issues/\(dir)", skip: "issue.md", ref: "") {
                PublishedRecords.issueEventFromMarkdown($0)
            }
            return PublishedRecords.issueFromMarkdown(dir, blob.content, events: events)
        }
        return items.compactMap { $0 }.sorted { $0.number > $1.number }
    }

    /// Pull requests on the `forkmesh/pulls` ledger (`.forkmesh/pulls/<n>/pull.md` plus event
    /// files), newest first. Cached.
    public func publishedPulls(_ owner: String, _ name: String) async throws -> [PublishedPull] {
        try await cached(pullsCache, "\(owner)/\(name)") {
            let ledgerRef = ApiService.pullLedgerRef
            let ledger = try? await self.tree(owner, name, path: ".forkmesh/pulls", ref: ledgerRef)
            let dirs = (ledger?.entries ?? []).filter { $0.isDirectory && DartCore.intTryParse($0.name) != nil }
                .map(\.name)
            let indexed = Set(dirs)
            let branchOnly = (ledger?.pullRefs ?? [:]).filter { !indexed.contains($0.key) }
                .sorted { $0.key < $1.key }
            let blobs = await self.blobs(owner, name, dirs.map { ".forkmesh/pulls/\($0)/pull.md" }, ref: ledgerRef)
            let entries = dirs.map { ($0, ledgerRef) } + branchOnly.map { ($0.key, $0.value) }
            let items: [PublishedPull?] = await ApiService.parallelMap(entries, width: ApiService.recordWidth) { entry in
                let (dir, ref) = entry
                if ref != ledgerRef { return await self.pullRecord(owner, name, dir, ref: ref) }
                guard let blob = blobs[".forkmesh/pulls/\(dir)/pull.md"] else { return nil }
                let events = await self.recordEvents(owner, name, ".forkmesh/pulls/\(dir)", skip: "pull.md", ref: ref) {
                    PublishedRecords.pullEventFromMarkdown($0)
                }
                let content: String = blob.content
                return await Task.detached(priority: .utility) {
                    PublishedRecords.pullFromMarkdown(dir, content, events: events)
                }.value
            }
            return items.compactMap { $0 }.sorted { $0.number > $1.number }
        }
    }

    public func publishedPullHeads(_ owner: String, _ name: String) async throws -> [PublishedPull] {
        let key = "\(owner)/\(name)"
        if let full = pullsCache.fresh(key, ttl: ApiService.cacheTtl, now: clock.now, epoch: relayEpoch),
           let pulls = try? await full.value {
            return pulls
        }
        return try await cached(pullHeadsCache, key) {
            let ledgerRef = ApiService.pullLedgerRef
            let ledger = try? await self.tree(owner, name, path: ".forkmesh/pulls", ref: ledgerRef)
            let dirs = (ledger?.entries ?? []).filter { $0.isDirectory && DartCore.intTryParse($0.name) != nil }
                .map(\.name)
            let indexed = Set(dirs)
            let branchOnly = (ledger?.pullRefs ?? [:]).filter { !indexed.contains($0.key) }
                .sorted { $0.key < $1.key }.map { ($0.key, $0.value) }
            let blobs = await self.blobs(owner, name, dirs.map { ".forkmesh/pulls/\($0)/pull.md" }, ref: ledgerRef)
            let listed: [(String, String)] = dirs.compactMap { dir in
                blobs[".forkmesh/pulls/\(dir)/pull.md"].map { (dir, $0.content) }
            }
            let branched: [(String, String)?] = await ApiService.parallelMap(branchOnly, width: ApiService.recordWidth) { entry in
                let (dir, ref) = entry
                guard let blob = try? await self.blob(owner, name, ".forkmesh/pulls/\(dir)/pull.md", ref: ref) else {
                    return nil
                }
                return (dir, blob.content)
            }
            let files: [(String, String)] = listed + branched.compactMap { $0 }
            return await Task.detached(priority: .utility) {
                files.map { PublishedRecords.pullHeadFromMarkdown($0.0, $0.1) }.sorted { $0.number > $1.number }
            }.value
        }
    }

    public func publishedPull(_ owner: String, _ name: String, _ number: Int) async throws -> PublishedPull? {
        let key = "\(owner)/\(name)"
        if let offline = repoOfflineError(key) { throw offline }
        let dir = String(number)
        for ref in [ApiService.pullLedgerRef, "forkmesh/pull/\(dir)"] {
            guard var pull = await pullRecord(owner, name, dir, ref: ref) else { continue }
            let payload = await pullPayload(owner, name, dir, ref: ref)
            if !payload.patch.dartTrimmed.isEmpty { pull.patch = payload.patch }
            pull.commits = payload.commits
            pullsCache.remove(key)
            pullHeadsCache.remove(key)
            treeCache.remove("\(key):\(ApiService.pullLedgerRef):.forkmesh/pulls")
            return pull
        }
        if let offline = repoOfflineError(key) { throw offline }
        return nil
    }

    func pullPayload(_ owner: String, _ name: String, _ dir: String, ref: String) async -> (patch: String, commits: String) {
        let folder = ".forkmesh/pulls/\(dir)"
        guard let listing = try? await tree(owner, name, path: folder, ref: ref) else { return ("", "") }
        let files = Set(listing.entries.filter { !$0.isDirectory }.map(\.name))
        async let patch: String = files.contains("changes.patch")
            ? ((try? await blob(owner, name, "\(folder)/changes.patch", ref: ref))?.content ?? "") : ""
        async let commits: String = files.contains("commits.mbox")
            ? ((try? await blob(owner, name, "\(folder)/commits.mbox", ref: ref))?.content ?? "") : ""
        return await (patch, commits)
    }

    func pullRecord(_ owner: String, _ name: String, _ dir: String, ref: String) async -> PublishedPull? {
        guard let blob = try? await blob(owner, name, ".forkmesh/pulls/\(dir)/pull.md", ref: ref) else { return nil }
        let events = await recordEvents(owner, name, ".forkmesh/pulls/\(dir)", skip: "pull.md", ref: ref) {
            PublishedRecords.pullEventFromMarkdown($0)
        }
        return PublishedRecords.pullFromMarkdown(dir, blob.content, events: events)
    }

    /// Discussions (`.forkmesh/discussions/<n>/discussion.md` plus event files), newest first. Cached.
    public func publishedDiscussions(_ owner: String, _ name: String) async throws -> [RepoDiscussion] {
        try await cached(discussionsCache, "\(owner)/\(name)") {
            let dirs = await self.numberedFolders(owner, name, ".forkmesh/discussions")
            let blobs = await self.blobs(owner, name, dirs.map { ".forkmesh/discussions/\($0)/discussion.md" })
            let items: [RepoDiscussion?] = await ApiService.parallelMap(dirs, width: ApiService.recordWidth) { dir in
                guard let blob = blobs[".forkmesh/discussions/\(dir)/discussion.md"] else { return nil }
                let events = await self.recordEvents(owner, name, ".forkmesh/discussions/\(dir)", skip: "discussion.md", ref: "") {
                    PublishedRecords.discussionEventFromMarkdown($0)
                }
                return PublishedRecords.discussionFromMarkdown(dir, blob.content, events: events)
            }
            return items.compactMap { $0 }.sorted { $0.number > $1.number }
        }
    }

    /// The `.md` event files of one record folder (all but `skip`), in name order, read one by
    /// one (`_issueEvents`, `_pullEvents`, `_discussionEvents`). Unreadable files are skipped; an
    /// unreadable folder gives no events.
    func recordEvents<E>(
        _ owner: String, _ name: String, _ folder: String, skip: String, ref: String, _ parse: (String) -> E
    ) async -> [E] {
        guard let listing = try? await tree(owner, name, path: folder, ref: ref) else { return [] }
        let files = listing.entries.filter { !$0.isDirectory && $0.name.hasSuffix(".md") && $0.name != skip }
            .sorted { $0.name < $1.name }
        let blobs = await self.blobs(owner, name, files.map(\.path), ref: ref)
        return files.compactMap { file in blobs[file.path].map { parse($0.content) } }
    }

    /// Names of the numbered sub-folders of `path`; empty when it cannot be read.
    func numberedFolders(_ owner: String, _ name: String, _ path: String, ref: String = "") async -> [String] {
        guard let listing = try? await tree(owner, name, path: path, ref: ref) else { return [] }
        return listing.entries.filter { $0.isDirectory && DartCore.intTryParse($0.name) != nil }.map(\.name)
    }

    /// `Future.wait(items.map(transform))`: runs concurrently, answers in input order.
    nonisolated static func parallelMap<T: Sendable, R: Sendable>(
        _ items: [T], width: Int, _ transform: @escaping @MainActor @Sendable (T) async -> R
    ) async -> [R] {
        await withTaskGroup(of: (Int, R).self) { group in
            var out = [R?](repeating: nil, count: items.count)
            var next = 0
            while next < min(max(1, width), items.count) {
                let index = next, item = items[index]
                group.addTask { (index, await transform(item)) }
                next += 1
            }
            for await (index, value) in group {
                out[index] = value
                if next < items.count {
                    let queued = next, item = items[queued]
                    group.addTask { (queued, await transform(item)) }
                    next += 1
                }
            }
            return out.map { $0! }
        }
    }
}
