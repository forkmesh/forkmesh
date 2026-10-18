import Foundation

public protocol SmbTransport: AnyObject, Sendable {
    func write(_ bytes: [UInt8]) async throws
    func read(_ count: Int) async throws -> [UInt8]
    func close()
}

public typealias SmbConnector = @Sendable (_ host: String, _ port: UInt16) async throws -> any SmbTransport
public typealias SmbProgress = @Sendable (_ done: Int64, _ total: Int64) -> Void

public struct SmbCredentials: Equatable, Sendable {
    public var user: String
    public var password: String
    public var guest: Bool

    public init(user: String = "", password: String = "", guest: Bool = false) {
        self.user = user
        self.password = password
        self.guest = guest
    }
}

public actor SmbClient {
    public let host: String
    public let port: UInt16
    public let share: String
    public let credentials: SmbCredentials
    private let connector: SmbConnector

    private var transport: (any SmbTransport)?
    private var messageId: UInt64 = 0
    private var sessionId: UInt64 = 0
    private var treeId: UInt32 = 0
    private var signingKey: [UInt8] = []
    private var negotiated: SmbNegotiated?
    private var locked = false
    private var waiters: [CheckedContinuation<Void, Never>] = []
    public private(set) var guestSession = false

    public static let chunk: UInt32 = 65536

    public init(host: String, port: UInt16 = 445, share: String, credentials: SmbCredentials, connector: @escaping SmbConnector) {
        self.host = host
        self.port = port
        self.share = share
        self.credentials = credentials
        self.connector = connector
    }

    public static func wire(_ path: String) -> String {
        path.split(separator: "/", omittingEmptySubsequences: true).joined(separator: "\\")
    }

    public func disconnect() {
        transport?.close()
        reset()
    }

    private func reset() {
        transport = nil
        messageId = 0
        sessionId = 0
        treeId = 0
        signingKey = []
        negotiated = nil
        guestSession = false
    }

    private func acquire() async {
        if !locked {
            locked = true
            return
        }
        await withCheckedContinuation { waiters.append($0) }
    }

    private func release() {
        if waiters.isEmpty {
            locked = false
        } else {
            waiters.removeFirst().resume()
        }
    }

    private func perform<T: Sendable>(_ operation: () async throws -> T) async throws -> T {
        await acquire()
        defer { release() }
        do {
            try await ensureConnected()
            return try await operation()
        } catch is SmbTransportError {
            transport?.close()
            reset()
            do {
                try await ensureConnected()
                return try await operation()
            } catch is SmbTransportError {
                transport?.close()
                reset()
                throw SmbError.unreachable(host)
            }
        }
    }

    private func ensureConnected() async throws {
        guard transport == nil else { return }
        let opened: any SmbTransport
        do {
            opened = try await connector(host, port)
        } catch let error as SmbError {
            throw error
        } catch {
            throw SmbError.unreachable(host)
        }
        transport = opened
        do {
            try await negotiate()
            try await login()
            if !share.isEmpty { treeId = try await connectTree(share) }
        } catch {
            opened.close()
            reset()
            if error is SmbTransportError { throw SmbError.unreachable(host) }
            throw error
        }
    }

    private func readFrame() async throws -> [UInt8] {
        guard let transport else { throw SmbTransportError("closed") }
        let head = try await transport.read(4)
        guard head.count == 4 else { throw SmbTransportError("short") }
        let length = Int(head[1]) << 16 | Int(head[2]) << 8 | Int(head[3])
        let body = try await transport.read(length)
        guard body.count == length else { throw SmbTransportError("short") }
        return body
    }

    @discardableResult
    private func request(
        _ command: SmbCommand, _ body: [UInt8], tree: UInt32? = nil, accept: Set<UInt32> = [SmbStatus.success]
    ) async throws -> (header: SmbHeader, message: [UInt8]) {
        guard let transport else { throw SmbTransportError("closed") }
        let id = messageId
        messageId += 1
        let charge: UInt16 = (negotiated?.dialect ?? 0) >= 0x0210 ? 1 : 0
        var writer = SmbMessages.header(command, messageId: id, sessionId: sessionId, treeId: tree ?? treeId, creditCharge: charge)
        writer.append(body)
        var message = writer.bytes
        if !signingKey.isEmpty, command != .negotiate, command != .sessionSetup {
            message = SmbMessages.sign(message, key: signingKey)
        }
        try await transport.write(SmbMessages.frame(message))
        while true {
            let reply = try await readFrame()
            guard let header = SmbHeader.parse(reply) else { throw SmbTransportError("bad reply") }
            if header.messageId != id { continue }
            if header.status == SmbStatus.pending && header.isAsync { continue }
            guard accept.contains(header.status) else { throw SmbError.status(header.status, host: host, share: share) }
            return (header, reply)
        }
    }

    private func negotiate() async throws {
        let guid = (0..<16).map { _ in UInt8.random(in: 0...255) }
        let reply: (header: SmbHeader, message: [UInt8])
        do {
            reply = try await request(.negotiate, SmbMessages.negotiateBody(clientGuid: guid))
        } catch let error as SmbError {
            throw error.status == 0 ? error : SmbError.noDialect(host)
        }
        guard let result = SmbMessages.parseNegotiate(reply.message), SmbMessages.dialects.contains(result.dialect) else {
            throw SmbError.noDialect(host)
        }
        negotiated = result
    }

    private func login() async throws {
        if credentials.guest {
            do {
                try await sessionSetup(user: "guest", password: "", anonymous: false)
            } catch let error as SmbError where error.status == SmbStatus.logonFailure || error.status == SmbStatus.accessDenied {
                try await sessionSetup(user: "", password: "", anonymous: true)
            }
        } else {
            try await sessionSetup(user: credentials.user, password: credentials.password, anonymous: false)
        }
    }

    private func sessionSetup(user: String, password: String, anonymous: Bool) async throws {
        sessionId = 0
        signingKey = []
        let first = try await request(
            .sessionSetup, SmbMessages.sessionSetupBody(Spnego.initToken(Ntlm.negotiateMessage(anonymous: anonymous))),
            accept: [SmbStatus.moreProcessingRequired])
        sessionId = first.header.sessionId
        guard let challenge = Ntlm.parseChallenge(SmbMessages.parseSessionSetup(first.message).token) else {
            throw SmbError(message: "\(host) did not offer NTLM sign-in.")
        }
        let clientChallenge = (0..<8).map { _ in UInt8.random(in: 0...255) }
        let auth = Ntlm.authenticate(challenge: challenge, user: user, password: password, clientChallenge: clientChallenge, anonymous: anonymous)
        let second = try await request(.sessionSetup, SmbMessages.sessionSetupBody(Spnego.responseToken(auth.message)))
        let flags = SmbMessages.parseSessionSetup(second.message).flags
        guestSession = anonymous || flags & 0x3 != 0
        if negotiated?.signingRequired == true, !guestSession, auth.sessionKey.count == 16 {
            signingKey = auth.sessionKey
        }
    }

    private func connectTree(_ name: String) async throws -> UInt32 {
        let reply = try await request(.treeConnect, SmbMessages.treeConnectBody("\\\\\(host)\\\(name)"), tree: 0)
        return reply.header.treeId
    }

    private func open(_ path: String, access: UInt32, disposition: UInt32, options: UInt32, tree: UInt32? = nil, attributes: UInt32 = 0) async throws -> SmbCreated {
        let reply = try await request(.create, SmbMessages.createBody(Self.wire(path), access: access, attributes: attributes, disposition: disposition, options: options), tree: tree)
        return SmbMessages.parseCreate(reply.message)
    }

    private func close(_ file: SmbFileId, tree: UInt32? = nil) async {
        _ = try? await request(.close, SmbMessages.closeBody(file), tree: tree)
    }

    private var readChunk: UInt32 { min(negotiated?.maxRead ?? Self.chunk, Self.chunk) }
    private var writeChunk: UInt32 { min(negotiated?.maxWrite ?? Self.chunk, Self.chunk) }

    private func listUnlocked(_ path: String) async throws -> [SmbDirectoryEntry] {
        let folder = try await open(path, access: SmbAccess.listFolder, disposition: SmbDisposition.open, options: SmbOptions.directory)
        var entries: [SmbDirectoryEntry] = []
        var first = true
        do {
            while true {
                try Task.checkCancellation()
                let reply = try await request(.queryDirectory, SmbMessages.queryDirectoryBody(folder.fileId, restart: first), accept: [SmbStatus.success, SmbStatus.noMoreFiles])
                first = false
                if reply.header.status == SmbStatus.noMoreFiles { break }
                let page = SmbMessages.parseQueryDirectory(reply.message)
                if page.isEmpty { break }
                entries += page.filter { $0.name != "." && $0.name != ".." }
            }
        } catch {
            await close(folder.fileId)
            throw error
        }
        await close(folder.fileId)
        return entries
    }

    public func list(_ path: String) async throws -> [SmbDirectoryEntry] {
        try await perform { try await listUnlocked(path) }
    }

    public func stat(_ path: String) async throws -> SmbCreated {
        try await perform {
            let file = try await open(path, access: SmbAccess.readAttributes | SmbAccess.synchronize, disposition: SmbDisposition.open, options: 0)
            await close(file.fileId)
            return file
        }
    }

    public func read(_ path: String, limit: Int64 = Int64.max, progress: SmbProgress? = nil) async throws -> [UInt8] {
        try await perform {
            let file = try await open(path, access: SmbAccess.readFile, disposition: SmbDisposition.open, options: SmbOptions.nonDirectory)
            if Int64(file.size) > limit {
                await close(file.fileId)
                throw SmbError.tooLarge
            }
            var data: [UInt8] = []
            data.reserveCapacity(Int(min(file.size, 64 << 20)))
            do {
                try await readLoop(file, progress: progress) { chunk in data += chunk }
            } catch {
                await close(file.fileId)
                throw error
            }
            await close(file.fileId)
            return data
        }
    }

    private func readLoop(_ file: SmbCreated, progress: SmbProgress? = nil, _ sink: ([UInt8]) throws -> Void) async throws {
        var offset: UInt64 = 0
        while true {
            try Task.checkCancellation()
            let reply = try await request(.read, SmbMessages.readBody(file.fileId, offset: offset, length: readChunk), accept: [SmbStatus.success, SmbStatus.endOfFile])
            if reply.header.status == SmbStatus.endOfFile { break }
            let chunk = SmbMessages.parseRead(reply.message)
            if chunk.isEmpty { break }
            try sink(chunk)
            offset += UInt64(chunk.count)
            progress?(Int64(offset), Int64(file.size))
            if file.size > 0 && offset >= file.size { break }
        }
    }

    public func download(_ path: String, to url: URL, progress: SmbProgress? = nil) async throws {
        try await perform {
            let file = try await open(path, access: SmbAccess.readFile, disposition: SmbDisposition.open, options: SmbOptions.nonDirectory)
            guard FileManager.default.createFile(atPath: url.path, contents: nil), let handle = try? FileHandle(forWritingTo: url) else {
                await close(file.fileId)
                throw SmbError(message: "Could not save \(url.lastPathComponent) on this device.")
            }
            do {
                try await readLoop(file, progress: progress) { chunk in try handle.write(contentsOf: Data(chunk)) }
            } catch {
                try? handle.close()
                await close(file.fileId)
                throw error
            }
            try? handle.close()
            await close(file.fileId)
        }
    }

    private func writeLoop(_ path: String, total: Int64, progress: SmbProgress?, _ next: () throws -> [UInt8]) async throws {
        let file = try await open(path, access: SmbAccess.writeFile, disposition: SmbDisposition.overwriteIf, options: SmbOptions.nonDirectory, attributes: SmbAttributes.normal)
        var offset: UInt64 = 0
        do {
            while true {
                try Task.checkCancellation()
                let chunk = try next()
                if chunk.isEmpty { break }
                var sent = 0
                while sent < chunk.count {
                    let piece = Array(chunk[sent..<min(chunk.count, sent + Int(writeChunk))])
                    let reply = try await request(.write, SmbMessages.writeBody(file.fileId, offset: offset, data: piece))
                    let count = Int(SmbMessages.parseWrite(reply.message))
                    guard count > 0 else { throw SmbError(message: "The file server stopped accepting data.") }
                    sent += count
                    offset += UInt64(count)
                    progress?(Int64(offset), total)
                }
            }
        } catch {
            await close(file.fileId)
            throw error
        }
        await close(file.fileId)
    }

    public func write(_ path: String, data: [UInt8], progress: SmbProgress? = nil) async throws {
        try await perform {
            var consumed = false
            try await writeLoop(path, total: Int64(data.count), progress: progress) {
                if consumed { return [] }
                consumed = true
                return data
            }
        }
    }

    public func upload(_ url: URL, to path: String, progress: SmbProgress? = nil) async throws {
        try await perform {
            guard let handle = try? FileHandle(forReadingFrom: url) else {
                throw SmbError(message: "Could not read \(url.lastPathComponent).")
            }
            defer { try? handle.close() }
            let total = (try? FileManager.default.attributesOfItem(atPath: url.path)[.size] as? NSNumber)?.int64Value ?? 0
            let size = Int(writeChunk) * 4
            try await writeLoop(path, total: total, progress: progress) {
                Array(try handle.read(upToCount: size) ?? Data())
            }
        }
    }

    public func makeFolder(_ path: String) async throws {
        try await perform {
            let folder = try await open(path, access: SmbAccess.listFolder, disposition: SmbDisposition.create, options: SmbOptions.directory)
            await close(folder.fileId)
        }
    }

    public func makeFile(_ path: String) async throws {
        try await perform {
            let file = try await open(path, access: SmbAccess.writeFile, disposition: SmbDisposition.create, options: SmbOptions.nonDirectory, attributes: SmbAttributes.normal)
            await close(file.fileId)
        }
    }

    private func deleteUnlocked(_ path: String, isFolder: Bool) async throws {
        if isFolder {
            for child in try await listUnlocked(path) {
                try Task.checkCancellation()
                let childPath = path.isEmpty ? child.name : path + "/" + child.name
                try await deleteUnlocked(childPath, isFolder: child.attributes & SmbAttributes.directory != 0)
            }
        }
        let item = try await open(
            path, access: SmbAccess.delete | SmbAccess.readAttributes | SmbAccess.synchronize, disposition: SmbDisposition.open,
            options: SmbOptions.deleteOnClose | (isFolder ? SmbOptions.directory : SmbOptions.nonDirectory))
        _ = try await request(.close, SmbMessages.closeBody(item.fileId))
    }

    public func delete(_ path: String, isFolder: Bool) async throws {
        guard !Self.wire(path).isEmpty else { throw SmbError.status(SmbStatus.accessDenied, host: host, share: share) }
        try await perform { try await deleteUnlocked(path, isFolder: isFolder) }
    }

    public func rename(_ path: String, to target: String) async throws {
        try await perform {
            let item = try await open(path, access: SmbAccess.delete | SmbAccess.readAttributes | SmbAccess.synchronize, disposition: SmbDisposition.open, options: 0)
            do {
                try await request(.setInfo, SmbMessages.renameBody(item.fileId, to: Self.wire(target)))
            } catch {
                await close(item.fileId)
                throw error
            }
            await close(item.fileId)
        }
    }

    public func shares() async throws -> [SmbShareInfo] {
        try await perform {
            let ipc = try await connectTree("IPC$")
            do {
                let list = try await enumerateShares(ipc)
                await disconnectTree(ipc)
                return list
            } catch {
                await disconnectTree(ipc)
                throw error
            }
        }
    }

    private func enumerateShares(_ ipc: UInt32) async throws -> [SmbShareInfo] {
        let refused = SmbError(message: "\(host) would not list its shares. Type the share name.")
        let pipe = try await open("srvsvc", access: SmbAccess.pipe, disposition: SmbDisposition.open, options: 0, tree: ipc)
        do {
            let bound = try await transact(pipe.fileId, tree: ipc, SmbSrvsvc.bind())
            guard SmbSrvsvc.bindAccepted(bound) else { throw refused }
            let reply = try await transact(pipe.fileId, tree: ipc, SmbSrvsvc.shareEnumRequest(host: host))
            let parts = SmbSrvsvc.fragments(reply)
            guard !parts.fault, let list = SmbSrvsvc.parseShareEnum(parts.stub) else { throw refused }
            await close(pipe.fileId, tree: ipc)
            return list.filter(\.isBrowsableDisk).sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        } catch {
            await close(pipe.fileId, tree: ipc)
            throw error
        }
    }

    private func disconnectTree(_ tree: UInt32) async {
        var writer = SmbWriter()
        writer.u16(4)
        writer.u16(0)
        _ = try? await request(.treeDisconnect, writer.bytes, tree: tree)
    }

    private func transact(_ pipe: SmbFileId, tree: UInt32, _ data: [UInt8]) async throws -> [UInt8] {
        try await request(.write, SmbMessages.writeBody(pipe, offset: 0, data: data), tree: tree)
        var received: [UInt8] = []
        for _ in 0..<64 {
            let reply = try await request(.read, SmbMessages.readBody(pipe, offset: 0, length: Self.chunk), tree: tree, accept: [SmbStatus.success, SmbStatus.bufferOverflow])
            received += SmbMessages.parseRead(reply.message)
            if SmbSrvsvc.fragments(received).complete { return received }
        }
        return received
    }
}
