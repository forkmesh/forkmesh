// Fetch, clone, and push orchestration over FMGit's protocol modules and a
// GitHttpTransport. Each public entry point runs through runGitOperation:
// failures surface as GitException codes and messages, and progress goes to
// the optional listener.

import Foundation

extension GitRepository {
    /// Fetches from a configured remote over smart HTTPS (protocol v2).
    ///
    /// Remote-tracking refs under `refs/remotes/<remote>/` and absent tags
    /// are updated; local branches and the worktree are never touched.
    public func fetch(_ request: GitFetchRequest, progress: GitProgressListener? = nil) async throws -> GitFetchResult {
        let config = try readConfig()
        let remote = config.remotes[request.remote]
        guard let url = request.url ?? remote?.url else {
            throw GitException(.invalidRepository, "remote \(request.remote) is not configured")
        }
        let refspec = try request.refspec ?? remote?.fetch ?? GitFetchRefspec.standard(request.remote)
        let shape = FetchShape(depth: nil, singleBranch: nil, deepen: request.deepen)
        return try await runGitOperation(cancellation: request.cancellation, progress: progress) { cancellation, sink in
            try await RemoteOperations(self).fetch(
                url: url,
                remoteName: request.remote,
                refspec: refspec,
                access: request.access,
                cancellation: cancellation,
                sink: sink,
                shape: shape
            )
        }
    }

    /// Pushes local branches to a configured remote with compare-and-swap
    /// ref commands. Non-fast-forward updates are refused locally before any
    /// bytes are sent; deletions and force updates are not expressible.
    public func push(_ request: GitPushRequest, progress: GitProgressListener? = nil) async throws -> GitPushResult {
        let config = try readConfig()
        guard let remote = config.remotes[request.remote] else {
            throw GitException(.invalidRepository, "remote \(request.remote) is not configured")
        }
        var refspecs = request.refspecs
        if refspecs.isEmpty {
            let head = try readHead()
            let branch = head.target.engSubstring("refs/heads/".utf8.count)
            let upstream = config.branches[branch]
            let remoteBranch: String
            if let upstream, upstream.remote.engSame(request.remote) {
                remoteBranch = upstream.merge.value.engSubstring("refs/heads/".utf8.count)
            } else {
                remoteBranch = branch
            }
            refspecs = [try GitPushRefspec(localBranch: branch, remoteBranch: remoteBranch)]
        }
        let pushed = refspecs
        return try await runGitOperation(cancellation: request.cancellation, progress: progress) { cancellation, sink in
            try await RemoteOperations(self).push(
                url: remote.url,
                remoteName: request.remote,
                refspecs: pushed,
                access: request.access,
                cancellation: cancellation,
                sink: sink
            )
        }
    }

    /// Clones an HTTPS remote into a new or empty directory: init, fetch,
    /// `HEAD` from the advertised symref, and a checkout through the
    /// existing checkout path. A failed clone removes everything it created.
    public static func clone(_ request: GitCloneRequest, progress: GitProgressListener? = nil) async throws -> GitCloneResult {
        let worktree = request.worktree
        let existed = EngineFS.directoryExists(worktree)
        if try existed && !EngineFS.listNames(worktree).isEmpty {
            throw GitException(.invalidRepository, "clone destination is not empty")
        }
        return try await runGitOperation(cancellation: request.cancellation, progress: progress) { cancellation, sink in
            do {
                let repository = try GitRepository.initialize(
                    GitRepositoryInit(worktree, limits: request.limits, detector: request.detector)
                )
                try repository.setRemote(request.remoteName, request.url)
                let fetched = try await RemoteOperations(repository).fetch(
                    url: request.url,
                    remoteName: request.remoteName,
                    refspec: try GitFetchRefspec.standard(request.remoteName),
                    access: request.access,
                    cancellation: cancellation,
                    sink: sink,
                    writeFetchHead: false,
                    shape: FetchShape(
                        depth: request.depth,
                        singleBranch: request.singleBranch ? .some(request.branch) : nil)
                )
                try cancellation.throwIfCancelled()
                let remoteHead = fetched.remoteHead
                var branchName = request.branch
                if branchName == nil, let remoteHead, remoteHead.engHasPrefix("refs/heads/") {
                    branchName = remoteHead.engSubstring("refs/heads/".utf8.count)
                }
                var checkedOut: String?
                if let branchName {
                    let branch = try GitBranchName(branchName)
                    let tracking = try GitRefName.parse("refs/remotes/\(request.remoteName)/\(branchName)")
                    let store = RefStore(repository.gitDirectory)
                    guard let target = try store.readDirect(tracking) else {
                        throw GitException(.invalidRef, "remote has no branch \(branchName)")
                    }
                    let created = try store.update(try branch.ref(), expectedOld: nil, next: target)
                    if created != .updated {
                        throw GitException(.refLocked, "clone branch already exists")
                    }
                    if request.singleBranch {
                        try repository.setRemote(
                            request.remoteName, request.url,
                            fetch: try GitFetchRefspec.parse(
                                "+refs/heads/\(branchName):refs/remotes/\(request.remoteName)/\(branchName)"))
                    }
                    try repository.setBranchUpstream(
                        branch,
                        remote: request.remoteName,
                        merge: try GitRefName.branch("refs/heads/\(branchName)")
                    )
                    sink.add(GitProgress(.checkingOut))
                    try cancellation.throwIfCancelled()
                    try repository.initialCheckout(branch, target, cancellation: cancellation) { written, total in
                        sink.add(GitProgress(.checkingOut, objects: written, total: total))
                    }
                    checkedOut = branchName
                }
                return GitCloneResult(repository: repository, checkedOutBranch: checkedOut, fetch: fetched)
            } catch {
                discardClone(worktree, existed)
                throw error
            }
        }
    }

    private static func discardClone(_ worktree: String, _ existed: Bool) {
        // Best effort: the caller sees the original failure.
        if !existed {
            try? EngineFS.deleteRecursive(worktree)
            return
        }
        guard let names = try? EngineFS.listNames(worktree) else { return }
        for name in names { try? EngineFS.deleteRecursive(EngineFS.join(worktree, name)) }
    }
}

// MARK: - Session

private final class RemoteSession {
    public let url: GitRemoteUrl
    public let transport: GitHttpTransport
    public let ownsTransport: Bool
    public let limits: GitResourceLimits
    public let cancellation: GitCancellationToken
    public let sink: GitProgressSink
    public let deadline: Date

    public init(
        url: GitRemoteUrl,
        access: GitRemoteAccess,
        limits: GitResourceLimits,
        cancellation: GitCancellationToken,
        sink: GitProgressSink
    ) {
        self.url = url
        if let provided = access.transport {
            transport = provided
            ownsTransport = false
        } else {
            transport = GitDefaultHttpTransport.make(headerProvider: access.headerProvider)
            ownsTransport = true
        }
        self.limits = limits
        self.cancellation = cancellation
        self.sink = sink
        deadline = Date().addingTimeInterval(limits.operationDeadline)
    }

    public var host: String { url.url.host ?? "" }

    public func close() async {
        if ownsTransport { await transport.close() }
    }

    public func checkpoint() throws {
        try cancellation.throwIfCancelled()
        if Date() > deadline {
            throw GitException(.networkUnavailable, "operation deadline exceeded")
        }
    }

    /// `base.replace(path: '$path/$suffix', query: query)`.
    public func serviceUrl(_ suffix: String, query: String? = nil) throws -> URL {
        var base = url.value
        if base.engHasSuffix("/") { base = base.engSubstring(0, base.utf8.count - 1) }
        var text = "\(base)/\(suffix)"
        if let query { text += "?\(query)" }
        guard let result = URL(string: text) else {
            throw GitException(.protocolUnsupported, "remote URL must be a credential-free HTTPS URL")
        }
        return result
    }

    public func advertisement(_ service: GitSmartHttpService) async throws -> [UInt8] {
        try checkpoint()
        sink.add(GitProgress(.connecting, remoteHost: host))
        var headers = ["accept": "*/*"]
        if service == .uploadPack { headers["git-protocol"] = "version=2" }
        let response = try await transport.send(
            try GitHttpRequest(
                method: "GET",
                url: try serviceUrl("info/refs", query: "service=\(service.name)"),
                headers: headers,
                maxResponseBytes: limits.maxAdvertisementBytes,
                expectedContentType: service.advertisementContentType,
                deadline: deadline
            ),
            cancellation: cancellation,
            limits: limits
        )
        return response.body
    }

    public func post(
        _ service: GitSmartHttpService,
        _ body: [UInt8],
        maxResponseBytes: Int,
        protocolV2: Bool = false,
        onBodyBytes: (@Sendable (Int) -> Void)? = nil
    ) async throws -> [UInt8] {
        try checkpoint()
        var headers = [
            "content-type": service.requestContentType,
            "accept": service.resultContentType,
        ]
        if protocolV2 { headers["git-protocol"] = "version=2" }
        let response = try await transport.send(
            try GitHttpRequest(
                method: "POST",
                url: try serviceUrl(service.name),
                headers: headers,
                body: body,
                maxResponseBytes: maxResponseBytes,
                expectedContentType: service.resultContentType,
                deadline: deadline,
                onBodyBytes: onBodyBytes
            ),
            cancellation: cancellation,
            limits: limits
        )
        return response.body
    }
}

private func encodePackets(_ packets: [GitPktLine]) throws -> [UInt8] {
    var output = [UInt8]()
    for packet in packets { output.append(contentsOf: try packet.encode()) }
    return output
}

struct FetchShape {
    var depth: Int?
    var singleBranch: String??
    var deepen: GitDeepen? = nil

    static let full = FetchShape(depth: nil, singleBranch: nil)
}

private struct FetchPlan {
    public let remoteRefs: [GitRemoteRef]
    public let wants: [GitObjectId]

    /// Local ref name to the remote object it should point at.
    public let refWrites: [String: GitObjectId]
    public let remoteHead: String?
}

// MARK: - Operations

private final class RemoteOperations {
    public let repository: GitRepository

    public static let havesPerRound = 32
    public static let maxHaves = 512

    public init(_ repository: GitRepository) { self.repository = repository }

    public func fetch(
        url: GitRemoteUrl,
        remoteName: String,
        refspec: GitFetchRefspec,
        access: GitRemoteAccess,
        cancellation: GitCancellationToken,
        sink: GitProgressSink,
        writeFetchHead: Bool = true,
        shape: FetchShape = .full
    ) async throws -> GitFetchResult {
        let session = RemoteSession(
            url: url, access: access, limits: repository.limits, cancellation: cancellation, sink: sink
        )
        let journal: GitOperationJournal
        do {
            journal = try GitOperationJournal.begin(repository.gitDirectory, "fetch")
        } catch {
            await session.close()
            throw error
        }
        do {
            let result = try await fetchBody(
                session, url: url, remoteName: remoteName, refspec: refspec,
                cancellation: cancellation, writeFetchHead: writeFetchHead, shape: shape
            )
            try journal.complete()
            await session.close()
            return result
        } catch {
            try? journal.complete()
            await session.close()
            throw error
        }
    }

    private func fetchBody(
        _ session: RemoteSession,
        url: GitRemoteUrl,
        remoteName: String,
        refspec: GitFetchRefspec,
        cancellation: GitCancellationToken,
        writeFetchHead: Bool,
        shape: FetchShape
    ) async throws -> GitFetchResult {
        let limits = repository.limits
        let advertisement = try GitSmartHttp.parseUploadPackAdvertisement(
            try await session.advertisement(.uploadPack),
            maxPackets: limits.maxPktLines
        )
        try Self.requireV2Fetch(advertisement)
        let shallow = try repository.shallowCommits()
        if shape.depth != nil || shape.deepen != nil || !shallow.isEmpty {
            try Self.requireShallowFetch(advertisement)
        }
        var capabilities = ["agent=\(GitProtocolV2.agent)"]
        if advertisement.supports("object-format") { capabilities.append("object-format=sha1") }
        let plan = try await planFetch(
            session, capabilities, refspec, singleBranch: shape.singleBranch, includeTags: shallow.isEmpty,
            wantPresent: shape.deepen != nil)
        var packBytes = 0
        var objects = 0
        var rounds = 0
        if !plan.wants.isEmpty {
            let negotiated = try await negotiateAndReceive(
                session, plan.wants, capabilities, shallow: shallow, deepen: shape.depth ?? shape.deepen?.depth,
                deepenRelative: shape.deepen?.isRelative ?? false, deepenSince: shape.deepen?.since)
            rounds = negotiated.rounds
            packBytes = negotiated.packBytes.count
            let declared = negotiated.packBytes.count >= 12 ? Self.packObjectCount(negotiated.packBytes) : 0
            let host = session.host
            let sink = session.sink
            let received = packBytes
            sink.add(GitProgress(.indexingPack, bytes: received, total: declared, remoteHost: host))
            let boundary = shallow.union(negotiated.shallow).subtracting(negotiated.unshallow)
            var roots = plan.wants
            for id in negotiated.unshallow where try repository.hasObject(id) {
                if let commit = try repository.readCommitOrTagTarget(id) { roots.append(contentsOf: commit.parents) }
            }
            objects = try installUntrustedPack(negotiated.packBytes, roots, boundary, cancellation) { resolved, total in
                sink.add(GitProgress(.indexingPack, bytes: received, objects: resolved, total: total, remoteHost: host))
            }
            if boundary != shallow {
                try GitShallowFile.write(repository.gitDirectory, boundary)
            }
        }
        try session.checkpoint()
        session.sink.add(GitProgress(.updatingRefs, refs: plan.refWrites.count, remoteHost: session.host))
        let updates = try writeFetchedRefs(plan.refWrites)
        if writeFetchHead {
            try self.writeFetchHead(plan, remoteName, url)
        }
        return GitFetchResult(
            updates: updates,
            packBytes: packBytes,
            objects: objects,
            remoteHead: plan.remoteHead,
            negotiationRounds: rounds
        )
    }

    private static func requireV2Fetch(_ advertisement: GitProtocolV2Advertisement) throws {
        if !advertisement.supports("ls-refs") || !advertisement.supports("fetch") {
            throw GitException(.protocolUnsupported, "remote does not advertise protocol v2 ls-refs and fetch")
        }
        let objectFormat = advertisement.valueOf("object-format")
        if advertisement.supports("object-format")
            && (objectFormat == nil || !objectFormat!.engSplit(0x20).contains { $0.engSame("sha1") })
        {
            throw GitException(.unsupportedRepositoryFormat, "remote object format is unsupported")
        }
    }

    private static func requireShallowFetch(_ advertisement: GitProtocolV2Advertisement) throws {
        let features = (advertisement.valueOf("fetch") ?? "").engSplit(0x20)
        if !features.contains(where: { $0.engSame("shallow") }) {
            throw GitException(.protocolUnsupported, "remote does not support shallow fetch")
        }
    }

    private func planFetch(
        _ session: RemoteSession,
        _ capabilities: [String],
        _ refspec: GitFetchRefspec,
        singleBranch: String?? = nil,
        includeTags: Bool = true,
        wantPresent: Bool = false
    ) async throws -> FetchPlan {
        let limits = repository.limits
        session.sink.add(GitProgress(.listingRefs, remoteHost: session.host))
        // Insertion ordered, duplicates dropped.
        var prefixes = [String]()
        for prefix in [
            "HEAD",
            refspec.isWildcard ? refspec.source.engSubstring(0, refspec.source.utf8.count - 1) : refspec.source,
            "refs/tags/",
        ] where !prefixes.contains(where: { $0.engSame(prefix) }) {
            prefixes.append(prefix)
        }
        let body = try await session.post(
            .uploadPack,
            try encodePackets(try GitProtocolV2.lsRefsRequest(refPrefixes: prefixes, capabilities: capabilities)),
            maxResponseBytes: limits.maxAdvertisementBytes,
            protocolV2: true
        )
        let remoteRefs = try GitProtocolV2.parseLsRefs(
            try GitPktLine.decodeAll(body, maximumPackets: limits.maxPktLines),
            maxRefCount: limits.maxRefCount
        )
        var remoteHead: String?
        var refWrites = [String: GitObjectId]()
        // An insertion-ordered set.
        var wants = [GitObjectId]()
        var wanted = Set<GitObjectId>()
        func want(_ id: GitObjectId) {
            if wanted.insert(id).inserted { wants.append(id) }
        }
        var onlyRef: String?
        if let singleBranch {
            if let branch = singleBranch {
                onlyRef = "refs/heads/\(branch)"
            } else if let head = remoteRefs.first(where: { $0.name.engSame("HEAD") })?.symrefTarget {
                onlyRef = head
            }
        }
        for remoteRef in remoteRefs {
            if remoteRef.name.engSame("HEAD") {
                remoteHead = remoteRef.symrefTarget
                continue
            }
            if singleBranch != nil {
                if let onlyRef, remoteRef.name.engSame(onlyRef), let destination = refspec.destinationFor(remoteRef.name) {
                    _ = try GitRefName.parse(destination)
                    refWrites[destination] = remoteRef.objectId
                    want(remoteRef.objectId)
                }
                continue
            }
            if let destination = refspec.destinationFor(remoteRef.name) {
                _ = try GitRefName.parse(destination)
                refWrites[destination] = remoteRef.objectId
                want(remoteRef.objectId)
                continue
            }
            if includeTags && remoteRef.name.engHasPrefix("refs/tags/") {
                let local = try GitRefName.parse(remoteRef.name)
                if try RefStore(repository.gitDirectory).readDirect(local) != nil { continue }
                refWrites[local.value] = remoteRef.objectId
                want(remoteRef.objectId)
            }
        }
        var missing = [GitObjectId]()
        for id in wants {
            try session.cancellation.throwIfCancelled()
            if try wantPresent || !repository.hasObject(id) { missing.append(id) }
        }
        return FetchPlan(remoteRefs: remoteRefs, wants: missing, refWrites: refWrites, remoteHead: remoteHead)
    }

    private func negotiateAndReceive(
        _ session: RemoteSession,
        _ wants: [GitObjectId],
        _ capabilities: [String],
        shallow: Set<GitObjectId> = [],
        deepen: Int? = nil,
        deepenRelative: Bool = false,
        deepenSince: Int64? = nil
    ) async throws -> (packBytes: [UInt8], rounds: Int, shallow: [GitObjectId], unshallow: [GitObjectId]) {
        let shallowLines = shallow.sorted { $0.hex < $1.hex }
        let limits = repository.limits
        let walk = HaveWalk(repository, limits.maxCommitWalk)
        try walk.seed()
        var common = [GitObjectId]()
        var commonSet = Set<GitObjectId>()
        var round = 0
        while true {
            try session.checkpoint()
            round += 1
            let batch = try walk.nextBatch(Self.havesPerRound, commonSet)
            let exhausted = batch.isEmpty || walk.sent >= Self.maxHaves || round >= limits.maxNegotiationRounds
            // `{...common, ...batch}`: insertion ordered, duplicates dropped.
            var haves = common
            var haveSet = commonSet
            for id in batch where haveSet.insert(id).inserted { haves.append(id) }
            session.sink.add(GitProgress(.negotiating, objects: haves.count, remoteHost: session.host))
            let body = try await session.post(
                .uploadPack,
                try encodePackets(try GitProtocolV2.fetchRequest(
                    wants: wants, haves: haves, done: exhausted, capabilities: capabilities,
                    shallow: shallowLines, deepen: deepen, deepenRelative: deepenRelative, deepenSince: deepenSince
                )),
                maxResponseBytes: limits.maxCompressedPackBytes,
                protocolV2: true,
                onBodyBytes: Self.receiveReporter(session)
            )
            session.sink.add(GitProgress(.receivingPack, bytes: body.count, remoteHost: session.host))
            let response = try GitFetchResponse.parse(
                try GitPktLine.decodeAll(body, maximumPackets: limits.maxPktLines),
                maxProgressBytes: limits.maxSidebandBytes
            )
            for id in response.acknowledged where commonSet.insert(id).inserted { common.append(id) }
            if let pack = response.packBytes { return (pack, round, response.shallow, response.unshallow) }
            if exhausted || response.ready {
                throw GitException(.protocolUnsupported, "remote ended negotiation without a packfile")
            }
        }
    }

    static let receiveReportStep = 256 * 1024

    private static func receiveReporter(_ session: RemoteSession) -> @Sendable (Int) -> Void {
        let sink = session.sink
        let host = session.host
        let reported = GitByteCounter()
        return { bytes in
            if reported.advance(to: bytes, step: receiveReportStep) {
                sink.add(GitProgress(.receivingPack, bytes: bytes, remoteHost: host))
            }
        }
    }

    /// Validates `packBytes` as untrusted transport input, verifies that
    /// every wanted object and every reference from new objects is
    /// satisfiable, and installs the pack atomically. Returns the object count.
    private func installUntrustedPack(
        _ packBytes: [UInt8],
        _ wants: [GitObjectId],
        _ shallow: Set<GitObjectId>,
        _ cancellation: GitCancellationToken,
        progress: ((Int, Int) -> Void)? = nil
    ) throws -> Int {
        let limits = repository.limits
        if packBytes.count > limits.maxCompressedPackBytes {
            throw GitException(.resourceLimitExceeded, "pack exceeds the compressed byte limit")
        }
        let quarantine = try GitObjectQuarantine.begin(
            repository.gitDirectory, cancellation: cancellation, limits: limits
        )
        do {
            let staged = try quarantine.stage("fetch.pack", packBytes)
            let pack = try GitPackReader.parse(
                packBytes,
                limits,
                origin: .untrustedTransport,
                cancellation: cancellation,
                detector: repository.detector,
                progress: progress
            )
            try verifyConnectivity(pack, wants, shallow, cancellation)
            if pack.objects.isEmpty {
                try quarantine.reject()
                return 0
            }
            let name = "pack-\(encodeLowerHex(pack.checksum))"
            let indexBytes = try GitPackIndexWriter.write(
                pack.objects.map { GitPackIndexWriteEntry($0.objectId, offset: $0.offset, crc32: $0.crc32) },
                pack.checksum
            )
            let stagedIndex = try quarantine.stage("fetch.idx", indexBytes)
            let packDirectory = EngineFS.join(EngineFS.join(repository.gitDirectory, "objects"), "pack")
            try EngineFS.createDirectories(packDirectory)
            let packFile = EngineFS.join(packDirectory, "\(name).pack")
            let indexFile = EngineFS.join(packDirectory, "\(name).idx")
            try cancellation.throwIfCancelled()
            if EngineFS.fileExists(packFile) && EngineFS.fileExists(indexFile) {
                // An identical pack is already installed; nothing to rename.
                try quarantine.reject()
            } else {
                try EngineFS.rename(staged, packFile)
                try cancellation.throwIfCancelled()
                try EngineFS.rename(stagedIndex, indexFile)
                try quarantine.reject()
            }
            repository.packedObjects.invalidate()
            return pack.objects.count
        } catch {
            try? quarantine.reject()
            throw error
        }
    }

    private func verifyConnectivity(
        _ pack: GitPack,
        _ wants: [GitObjectId],
        _ shallow: Set<GitObjectId>,
        _ cancellation: GitCancellationToken
    ) throws {
        var verified = Set<GitObjectId>()
        var stack = wants
        var visited = 0
        while let id = stack.popLast() {
            try cancellation.throwIfCancelled()
            if !verified.insert(id).inserted { continue }
            visited += 1
            if visited > repository.limits.maxCommitWalk * 8 {
                throw GitException(.resourceLimitExceeded, "connectivity check exceeded the configured limit")
            }
            guard let object = pack.lookup(id) else {
                if !(try repository.hasObject(id)) {
                    throw GitException(.packCorrupt, "remote pack does not contain a required object")
                }
                continue
            }
            switch object.type {
            case .blob:
                break
            case .tree:
                for entry in try GitTree.parse(object.body).entries { stack.append(entry.objectId) }
            case .commit:
                let commit = try GitCommit.parse(object.body)
                stack.append(commit.tree)
                stack.append(contentsOf: repository.walkParents(id, commit, shallow))
            case .tag:
                stack.append(try GitAnnotatedTag.parse(object.body).target)
            }
        }
    }

    private func writeFetchedRefs(_ refWrites: [String: GitObjectId]) throws -> [GitRefUpdateRecord] {
        let store = RefStore(repository.gitDirectory)
        var updates = [GitRefUpdateRecord]()
        for name in refWrites.keys.sorted(by: { $0.engCompare($1) < 0 }) {
            let target = refWrites[name]!
            let ref = try GitRefName.parse(name)
            if !(try repository.hasObject(target)) {
                throw GitException(.packCorrupt, "fetched ref target is not present after install")
            }
            let current = try store.readDirect(ref)
            if current == target {
                updates.append(GitRefUpdateRecord(ref: name, oldId: current, newId: target))
                continue
            }
            let result = try store.update(ref, expectedOld: current, next: target)
            if result != .updated {
                throw GitException(.refLocked, "remote-tracking ref changed during fetch")
            }
            updates.append(GitRefUpdateRecord(ref: name, oldId: current, newId: target))
        }
        return updates
    }

    private func writeFetchHead(_ plan: FetchPlan, _ remoteName: String, _ url: GitRemoteUrl) throws {
        var buffer = ""
        var first = true
        for remoteRef in plan.remoteRefs {
            if remoteRef.name.engSame("HEAD") { continue }
            let isBranch = remoteRef.name.engHasPrefix("refs/heads/")
            let isTag = remoteRef.name.engHasPrefix("refs/tags/")
            if !isBranch && !isTag { continue }
            let short = remoteRef.name.engSubstring(isBranch ? 11 : 10)
            let marker = first && isBranch ? "" : "not-for-merge"
            if first && isBranch { first = false }
            buffer += "\(remoteRef.objectId.hex)\t\(marker)\t\(isBranch ? "branch" : "tag") '\(short)' of \(url.value)\n"
        }
        let destination = EngineFS.join(repository.gitDirectory, "FETCH_HEAD")
        let lock = "\(destination).lock"
        if !EngineFS.createFileExclusive(lock) {
            throw GitException(.refLocked, "FETCH_HEAD lock already exists")
        }
        defer {
            if EngineFS.fileExists(lock) { try? EngineFS.deleteFile(lock) }
        }
        try EngineFS.writeStringFlushed(lock, buffer)
        try EngineFS.rename(lock, destination)
    }

    public func push(
        url: GitRemoteUrl,
        remoteName: String,
        refspecs: [GitPushRefspec],
        access: GitRemoteAccess,
        cancellation: GitCancellationToken,
        sink: GitProgressSink
    ) async throws -> GitPushResult {
        if refspecs.isEmpty {
            throw GitException(.invalidRef, "push requires at least one branch")
        }
        let session = RemoteSession(
            url: url, access: access, limits: repository.limits, cancellation: cancellation, sink: sink
        )
        let journal: GitOperationJournal
        do {
            journal = try GitOperationJournal.begin(repository.gitDirectory, "push")
        } catch {
            await session.close()
            throw error
        }
        do {
            let result = try await pushBody(session, remoteName: remoteName, refspecs: refspecs, cancellation: cancellation)
            try journal.complete()
            await session.close()
            return result
        } catch {
            try? journal.complete()
            await session.close()
            throw error
        }
    }

    private func pushBody(
        _ session: RemoteSession,
        remoteName: String,
        refspecs: [GitPushRefspec],
        cancellation: GitCancellationToken
    ) async throws -> GitPushResult {
        let limits = repository.limits
        let advertisement = try GitSmartHttp.parseReceivePackAdvertisement(
            try await session.advertisement(.receivePack),
            maxPackets: limits.maxPktLines,
            maxRefCount: limits.maxRefCount
        )
        let capabilities = try GitReceivePack.selectCapabilities(advertisement)
        let store = RefStore(repository.gitDirectory)
        var results = [GitPushRefResult]()
        var commands = [GitPushRefUpdate]()
        var newTips = [GitObjectId]()
        var seenRemote = Set<[UInt8]>()
        for refspec in refspecs {
            try session.checkpoint()
            if !seenRemote.insert(Array(refspec.remote.value.utf8)).inserted {
                throw GitException(.invalidRef, "push refspecs target the same remote branch twice")
            }
            guard let tip = try store.readDirect(refspec.local) else {
                throw GitException(.invalidRef, "local branch \(refspec.localBranch) does not exist")
            }
            let remoteOld = advertisement.refs[refspec.remote.value]
            if remoteOld == tip {
                results.append(GitPushRefResult(
                    localRef: refspec.local.value,
                    remoteRef: refspec.remote.value,
                    oldId: remoteOld,
                    newId: tip,
                    outcome: .upToDate
                ))
                continue
            }
            if let remoteOld {
                try requireFastForward(remoteOld, tip, refspec, cancellation)
            }
            commands.append(try GitPushRefUpdate(
                refName: refspec.remote.value,
                expectedOld: remoteOld ?? GitReceivePack.zeroId,
                next: tip
            ))
            newTips.append(tip)
            results.append(GitPushRefResult(
                localRef: refspec.local.value,
                remoteRef: refspec.remote.value,
                oldId: remoteOld,
                newId: tip,
                outcome: .updated
            ))
        }
        if commands.isEmpty {
            return GitPushResult(refs: results, packBytes: 0, objects: 0)
        }
        session.sink.add(GitProgress(.preparingPack, remoteHost: session.host))
        let exclude = try pushExclusionSet(Array(advertisement.refs.values), newTips, cancellation)
        let pack = try repository.writePack(newTips, exclude: exclude, cancellation: cancellation)
        try session.checkpoint()
        let objectCount = Self.packObjectCount(pack.packBytes)
        session.sink.add(GitProgress(
            .sendingPack, bytes: pack.packBytes.count, objects: objectCount, remoteHost: session.host
        ))
        var body = try encodePackets(try GitReceivePack.commandPackets(commands, capabilities: capabilities))
        body.append(contentsOf: pack.packBytes)
        let responseBody = try await session.post(
            .receivePack,
            body,
            maxResponseBytes: limits.maxAdvertisementBytes
        )
        session.sink.add(GitProgress(.awaitingReport, remoteHost: session.host))
        let status = try GitReceivePack.parseResponse(
            responseBody,
            sideband: capabilities.contains("side-band-64k"),
            maxPackets: limits.maxPktLines,
            maxProgressBytes: limits.maxSidebandBytes
        )
        if !status.unpackOk {
            throw GitException(
                .remoteRejected,
                "remote failed to unpack the push: \(status.unpackMessage ?? "")"
            )
        }
        var byRef = [String: GitPushRefStatus]()
        for ref in status.refs { byRef[ref.refName] = ref }
        var finalResults = [GitPushRefResult]()
        var firstRejection: GitException?
        for result in results {
            if result.outcome == .upToDate {
                finalResults.append(result)
                continue
            }
            guard let reported = byRef.removeValue(forKey: result.remoteRef) else {
                throw GitException(.protocolUnsupported, "remote report omitted a pushed ref")
            }
            if reported.accepted {
                try updateRemoteTracking(remoteName, result)
                finalResults.append(result)
                continue
            }
            let code = reported.code ?? .remoteRejected
            if firstRejection == nil {
                firstRejection = GitException(code, "remote rejected \(result.remoteRef): \(reported.message ?? "")")
            }
            finalResults.append(GitPushRefResult(
                localRef: result.localRef,
                remoteRef: result.remoteRef,
                oldId: result.oldId,
                newId: result.newId,
                outcome: .rejected,
                code: code,
                message: reported.message
            ))
        }
        if !byRef.isEmpty {
            throw GitException(.protocolUnsupported, "remote report names a ref that was not pushed")
        }
        if let firstRejection { throw firstRejection }
        return GitPushResult(refs: finalResults, packBytes: pack.packBytes.count, objects: objectCount)
    }

    private static func packObjectCount(_ packBytes: [UInt8]) -> Int {
        (Int(packBytes[8]) << 24) | (Int(packBytes[9]) << 16) | (Int(packBytes[10]) << 8) | Int(packBytes[11])
    }

    private func requireFastForward(
        _ remoteOld: GitObjectId,
        _ tip: GitObjectId,
        _ refspec: GitPushRefspec,
        _ cancellation: GitCancellationToken
    ) throws {
        if !(try repository.hasObject(remoteOld)) {
            throw GitException(
                .nonFastForward,
                "remote \(refspec.remote.value) points at a commit that is not known locally; fetch first"
            )
        }
        if !(try repository.isAncestor(remoteOld, tip, cancellation)) {
            throw GitException(
                .nonFastForward,
                "pushing \(refspec.local.value) to \(refspec.remote.value) would not fast-forward"
            )
        }
    }

    /// Objects the remote is known to have: every commit reachable from an
    /// advertised tip that exists locally, plus the tree closure of the
    /// boundary commits (the first remote-known ancestors of the new tips).
    private func pushExclusionSet(
        _ remoteTips: [GitObjectId],
        _ newTips: [GitObjectId],
        _ cancellation: GitCancellationToken
    ) throws -> Set<GitObjectId> {
        var knownTips = [GitObjectId]()
        for tip in remoteTips {
            try cancellation.throwIfCancelled()
            if try repository.hasObject(tip) { knownTips.append(tip) }
        }
        let remoteCommits = try repository.reachableCommits(knownTips, cancellation)
        var exclude = remoteCommits
        var boundaries = [GitObjectId]()
        var boundarySet = Set<GitObjectId>()
        let shallow = try repository.shallowCommits()
        var visited = Set<GitObjectId>()
        var stack = newTips
        while let id = stack.popLast() {
            try cancellation.throwIfCancelled()
            if !visited.insert(id).inserted { continue }
            if remoteCommits.contains(id) {
                if boundarySet.insert(id).inserted { boundaries.append(id) }
                continue
            }
            if visited.count > repository.limits.maxCommitWalk {
                throw GitException(.resourceLimitExceeded, "push commit walk exceeded the configured limit")
            }
            if let commit = try repository.readCommitOrTagTarget(id) {
                stack.append(contentsOf: repository.walkParents(id, commit, shallow))
            }
        }
        for boundary in boundaries {
            guard let commit = try repository.readCommitOrTagTarget(boundary) else { continue }
            try repository.collectTreeClosure(commit.tree, &exclude, cancellation)
        }
        return exclude
    }

    private func updateRemoteTracking(_ remoteName: String, _ result: GitPushRefResult) throws {
        let tracking = try GitRefName.parse("refs/remotes/\(remoteName)/\(result.remoteRef.engSubstring(11))")
        let store = RefStore(repository.gitDirectory)
        let current = try store.readDirect(tracking)
        if current == result.newId { return }
        _ = try store.update(tracking, expectedOld: current, next: result.newId)
    }
}

/// Produces `have` candidates from local ref tips, descending through
/// parents of commits the server has not acknowledged.
private final class HaveWalk {
    public let repository: GitRepository
    public let maxCommits: Int
    private var queue = [GitObjectId]()
    private var pendingExpansion = [GitObjectId]()
    private var seen = Set<GitObjectId>()
    private(set) var sent = 0

    public init(_ repository: GitRepository, _ maxCommits: Int) {
        self.repository = repository
        self.maxCommits = maxCommits
    }

    private var shallow = Set<GitObjectId>()

    public func seed() throws {
        shallow = try repository.shallowCommits()
        let store = RefStore(repository.gitDirectory)
        // An insertion-ordered set of tips, in listing order.
        var tips = [GitObjectId]()
        var known = Set<GitObjectId>()
        for prefix in ["refs/heads/", "refs/remotes/"] {
            let refs = try store.listDirect(prefix, repository.limits)
            for name in refs.keys.sorted(by: { $0.engCompare($1) < 0 }) where known.insert(refs[name]!).inserted {
                tips.append(refs[name]!)
            }
        }
        if let head = try repository.readHeadObjectId(), known.insert(head).inserted { tips.append(head) }
        queue.append(contentsOf: tips)
    }

    public func nextBatch(_ size: Int, _ common: Set<GitObjectId>) throws -> [GitObjectId] {
        for id in pendingExpansion where !common.contains(id) {
            if let commit = try repository.readCommitOrTagTarget(id) {
                queue.append(contentsOf: repository.walkParents(id, commit, shallow))
            }
        }
        pendingExpansion.removeAll()
        var batch = [GitObjectId]()
        while batch.count < size && !queue.isEmpty {
            let id = queue.removeFirst()
            if !seen.insert(id).inserted || common.contains(id) { continue }
            if seen.count > maxCommits { break }
            if !(try repository.hasObject(id)) { continue }
            batch.append(id)
            pendingExpansion.append(id)
        }
        sent += batch.count
        return batch
    }
}

final class GitByteCounter: @unchecked Sendable {
    private let lock = NSLock()
    private var last = 0

    func advance(to bytes: Int, step: Int) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        if bytes - last < step { return false }
        last = bytes
        return true
    }
}
