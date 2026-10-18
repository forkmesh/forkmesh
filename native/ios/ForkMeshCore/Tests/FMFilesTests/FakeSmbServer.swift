import FMCrypto
import Foundation
@testable import FMFiles

final class FakeSmbState: @unchecked Sendable {
    enum Node {
        case folder
        case file([UInt8])
    }

    let lock = NSLock()
    var users: [String: String] = ["alice": "secret"]
    var guestAllowed = true
    var anonymousOnly = false
    var requireSigning = false
    var dialect: UInt16 = 0x0210
    var maxRead: UInt32 = 65536
    var pendingReads = false
    var dropNextRead = false
    var connections = 0
    var badSignatures = 0
    var signedRequests = 0
    var shares: [String: [String: Node]] = [:]
    var shareList: [SmbShareInfo] = [
        SmbShareInfo(name: "usb", type: 0, remark: "USB disk"),
        SmbShareInfo(name: "IPC$", type: 0x8000_0003, remark: ""),
        SmbShareInfo(name: "media", type: 0, remark: ""),
        SmbShareInfo(name: "print", type: 1, remark: ""),
    ]
    var lastUser = ""

    init() {
        shares["usb"] = [
            "": .folder,
            "docs": .folder,
            "docs\\notes.txt": .file(Array("hello from the router".utf8)),
            "docs\\deep": .folder,
            "docs\\deep\\a.md": .file(Array("# A".utf8)),
            "photo.jpg": .file([0xff, 0xd8, 0xff, 0x00, 0x01]),
            ".hidden": .file([]),
        ]
    }

    func connector() -> SmbConnector {
        { [self] _, _ in
            lock.withLock { connections += 1 }
            return FakeSmbConnection(state: self)
        }
    }

    func node(_ share: String, _ path: String) -> Node? { lock.withLock { shares[share]?[path] } }
    func set(_ share: String, _ path: String, _ node: Node?) { lock.withLock { shares[share]?[path] = node } }
    func children(_ share: String, _ path: String) -> [String] {
        lock.withLock {
            let prefix = path.isEmpty ? "" : path + "\\"
            return (shares[share] ?? [:]).keys.filter { key in
                !key.isEmpty && key.hasPrefix(prefix) && !key.dropFirst(prefix.count).contains("\\")
            }.sorted()
        }
    }
}

final class FakeSmbConnection: SmbTransport, @unchecked Sendable {
    struct Handle {
        var share: String
        var path: String
        var deleteOnClose: Bool
        var listed = false
        var pipe = false
    }

    let state: FakeSmbState
    let lock = NSLock()
    var inbound: [UInt8] = []
    var outbound: [UInt8] = []
    var closed = false
    var sessionId: UInt64 = 0
    var sessionKey: [UInt8] = []
    var guest = false
    var serverChallenge: [UInt8] = [1, 2, 3, 4, 5, 6, 7, 8]
    var trees: [UInt32: String] = [:]
    var nextTree: UInt32 = 1
    var handles: [UInt64: Handle] = [:]
    var nextHandle: UInt64 = 1
    var pipeOut: [UInt8] = []

    init(state: FakeSmbState) { self.state = state }

    func write(_ bytes: [UInt8]) async throws {
        if closed { throw SmbTransportError("closed") }
        inbound += bytes
        while inbound.count >= 4 {
            let length = Int(inbound[1]) << 16 | Int(inbound[2]) << 8 | Int(inbound[3])
            guard inbound.count >= 4 + length else { break }
            let message = Array(inbound[4..<(4 + length)])
            inbound.removeFirst(4 + length)
            handle(message)
        }
    }

    func read(_ count: Int) async throws -> [UInt8] {
        if state.lock.withLock({ state.dropNextRead }) {
            state.lock.withLock { state.dropNextRead = false }
            closed = true
            throw SmbTransportError("dropped")
        }
        guard !closed, outbound.count >= count else { throw SmbTransportError("nothing to read") }
        let chunk = Array(outbound.prefix(count))
        outbound.removeFirst(count)
        return chunk
    }

    func close() { closed = true }

    func reply(_ request: SmbHeader, status: UInt32, body: [UInt8], tree: UInt32? = nil, session: UInt64? = nil, async: Bool = false) {
        var writer = SmbWriter()
        writer.append([0xfe, 0x53, 0x4d, 0x42])
        writer.u16(64)
        writer.u16(1)
        writer.u32(status)
        writer.u16(request.command)
        writer.u16(64)
        writer.u32(SmbFlags.serverToRedirector | (async ? SmbFlags.async : 0))
        writer.u32(0)
        writer.u64(request.messageId)
        if async {
            writer.u64(99)
        } else {
            writer.u32(0)
            writer.u32(tree ?? request.treeId)
        }
        writer.u64(session ?? sessionId)
        writer.zeros(16)
        writer.append(body)
        outbound += SmbMessages.frame(writer.bytes)
    }

    func error(_ request: SmbHeader, _ status: UInt32) {
        reply(request, status: status, body: [9, 0, 0, 0, 0, 0, 0, 0, 0])
    }

    func handle(_ message: [UInt8]) {
        guard let header = SmbHeader.parse(message), let command = SmbCommand(rawValue: header.command) else { return }
        let reader = SmbReader(message)
        if !sessionKey.isEmpty, command != .negotiate, command != .sessionSetup {
            let signed = SmbMessages.sign(message, key: sessionKey)
            if header.flags & SmbFlags.signed == 0 || signed != message {
                state.lock.withLock { state.badSignatures += 1 }
                return error(header, SmbStatus.accessDenied)
            }
            state.lock.withLock { state.signedRequests += 1 }
        }
        switch command {
        case .negotiate: negotiate(header, reader)
        case .sessionSetup: sessionSetup(header, reader)
        case .treeConnect: treeConnect(header, reader)
        case .treeDisconnect:
            trees[header.treeId] = nil
            reply(header, status: 0, body: [4, 0, 0, 0])
        case .create: create(header, reader)
        case .close: closeHandle(header, reader)
        case .queryDirectory: queryDirectory(header, reader)
        case .read: readFile(header, reader)
        case .write: writeFile(header, reader)
        case .setInfo: setInfo(header, reader)
        default: error(header, SmbStatus.notSupported)
        }
    }

    func negotiate(_ header: SmbHeader, _ reader: SmbReader) {
        let count = Int(reader.u16(66))
        let offered = (0..<count).map { reader.u16(100 + $0 * 2) }
        guard offered.contains(state.dialect) else { return error(header, SmbStatus.notSupported) }
        var body = SmbWriter()
        body.u16(65)
        body.u16(state.requireSigning ? 3 : 1)
        body.u16(state.dialect)
        body.u16(0)
        body.zeros(16)
        body.u32(0)
        body.u32(65536)
        body.u32(state.maxRead)
        body.u32(65536)
        body.u64(0)
        body.u64(0)
        body.u16(128)
        body.u16(0)
        body.u32(0)
        body.u8(0)
        reply(header, status: 0, body: body.bytes, session: 0)
    }

    func challengeMessage() -> [UInt8] {
        var info = SmbWriter()
        for (id, text) in [(UInt16(2), "Domain"), (UInt16(1), "Server")] {
            let value = SmbText.utf16(text)
            info.u16(id)
            info.u16(UInt16(value.count))
            info.append(value)
        }
        info.u16(7)
        info.u16(8)
        info.u64(Ntlm.fileTime(Date()))
        info.u32(0)
        var writer = SmbWriter()
        writer.append(Ntlm.signature)
        writer.u32(2)
        writer.u16(0)
        writer.u16(0)
        writer.u32(56)
        writer.u32(Ntlm.clientFlags)
        writer.append(serverChallenge)
        writer.zeros(8)
        writer.u16(UInt16(info.count))
        writer.u16(UInt16(info.count))
        writer.u32(56)
        writer.zeros(8)
        writer.append(info.bytes)
        return writer.bytes
    }

    func sessionSetup(_ header: SmbHeader, _ reader: SmbReader) {
        let token = reader.slice(Int(reader.u16(64 + 12)), Int(reader.u16(64 + 14)))
        guard let ntlm = Ntlm.find(token) else { return error(header, SmbStatus.logonFailure) }
        let message = SmbReader(ntlm)
        if message.u32(8) == 1 {
            sessionId = 0x1122_3344
            let challenge = Spnego.responseToken(challengeMessage())
            var body = SmbWriter()
            body.u16(9)
            body.u16(0)
            body.u16(72)
            body.u16(UInt16(challenge.count))
            body.append(challenge)
            return reply(header, status: SmbStatus.moreProcessingRequired, body: body.bytes)
        }
        func field(_ offset: Int) -> [UInt8] { message.slice(Int(message.u32(offset + 4)), Int(message.u16(offset))) }
        let nt = field(20)
        let domain = SmbText.string(field(28))
        let user = SmbText.string(field(36))
        state.lock.withLock { state.lastUser = user }
        var flags: UInt16 = 0
        sessionKey = []
        if user.isEmpty && nt.isEmpty {
            guard state.guestAllowed else { return error(header, SmbStatus.logonFailure) }
            flags = 2
        } else if let password = state.users[user.lowercased()], !state.anonymousOnly {
            let key = Ntlm.ntowfv2(password: password, user: user, domain: domain)
            let proof = Array(nt.prefix(16))
            guard nt.count > 16, Ntlm.ntProof(ntowfv2: key, serverChallenge: serverChallenge, blob: Array(nt.dropFirst(16))) == proof else {
                return error(header, SmbStatus.logonFailure)
            }
            if state.requireSigning { sessionKey = HmacMd5.mac(key: key, proof) }
        } else if state.guestAllowed && !state.anonymousOnly {
            flags = 1
        } else {
            return error(header, SmbStatus.logonFailure)
        }
        guest = flags != 0
        var body = SmbWriter()
        body.u16(9)
        body.u16(flags)
        body.u16(72)
        body.u16(0)
        body.u8(0)
        reply(header, status: 0, body: body.bytes)
    }

    func treeConnect(_ header: SmbHeader, _ reader: SmbReader) {
        let path = SmbText.string(reader.slice(Int(reader.u16(64 + 4)), Int(reader.u16(64 + 6))))
        let name = String(path.split(separator: "\\").last ?? "")
        guard name == "IPC$" || state.lock.withLock({ state.shares[name] != nil }) else { return error(header, SmbStatus.badNetworkName) }
        let id = nextTree
        nextTree += 1
        trees[id] = name
        var body = SmbWriter()
        body.u16(16)
        body.u8(name == "IPC$" ? 2 : 1)
        body.u8(0)
        body.u32(0)
        body.u32(0)
        body.u32(0x001f01ff)
        reply(header, status: 0, body: body.bytes, tree: id)
    }

    func fileIdBytes(_ id: UInt64) -> [UInt8] {
        var writer = SmbWriter()
        writer.u64(id)
        writer.u64(id)
        return writer.bytes
    }

    func createdBody(_ id: UInt64, node: FakeSmbState.Node?) -> [UInt8] {
        var body = SmbWriter()
        body.u16(89)
        body.u8(0)
        body.u8(0)
        body.u32(1)
        body.u64(0)
        body.u64(0)
        body.u64(Ntlm.fileTime(Date(timeIntervalSince1970: 1_700_000_000)))
        body.u64(0)
        var size: UInt64 = 0
        var attributes = SmbAttributes.normal
        if case .file(let data)? = node { size = UInt64(data.count) }
        if case .folder? = node { attributes = SmbAttributes.directory }
        body.u64(size)
        body.u64(size)
        body.u32(attributes)
        body.u32(0)
        body.append(fileIdBytes(id))
        body.u32(0)
        body.u32(0)
        return body.bytes
    }

    func create(_ header: SmbHeader, _ reader: SmbReader) {
        guard let share = trees[header.treeId] else { return error(header, SmbStatus.accessDenied) }
        let name = SmbText.string(reader.slice(Int(reader.u16(64 + 44)), Int(reader.u16(64 + 46))))
        let disposition = reader.u32(64 + 36)
        let options = reader.u32(64 + 40)
        let id = nextHandle
        nextHandle += 1
        if share == "IPC$" {
            guard name == "srvsvc" else { return error(header, SmbStatus.nameNotFound) }
            handles[id] = Handle(share: share, path: name, deleteOnClose: false, pipe: true)
            return reply(header, status: 0, body: createdBody(id, node: nil))
        }
        let parent = name.split(separator: "\\").dropLast().joined(separator: "\\")
        var node = state.node(share, name)
        switch disposition {
        case SmbDisposition.create:
            guard node == nil else { return error(header, SmbStatus.nameCollision) }
            guard case .folder? = state.node(share, parent) else { return error(header, SmbStatus.pathNotFound) }
            node = options & SmbOptions.directory != 0 ? .folder : .file([])
            state.set(share, name, node)
        case SmbDisposition.overwriteIf:
            if case .folder? = node { return error(header, SmbStatus.fileIsADirectory) }
            guard case .folder? = state.node(share, parent) else { return error(header, SmbStatus.pathNotFound) }
            node = .file([])
            state.set(share, name, node)
        default:
            guard node != nil else { return error(header, SmbStatus.nameNotFound) }
            if options & SmbOptions.directory != 0, case .file? = node { return error(header, SmbStatus.notADirectory) }
        }
        handles[id] = Handle(share: share, path: name, deleteOnClose: options & SmbOptions.deleteOnClose != 0)
        reply(header, status: 0, body: createdBody(id, node: node))
    }

    func handleId(_ reader: SmbReader, _ offset: Int) -> UInt64 { reader.u64(64 + offset) }

    func closeHandle(_ header: SmbHeader, _ reader: SmbReader) {
        let id = handleId(reader, 8)
        guard let handle = handles.removeValue(forKey: id) else { return error(header, SmbStatus.accessDenied) }
        if handle.deleteOnClose {
            if !state.children(handle.share, handle.path).isEmpty { return error(header, SmbStatus.directoryNotEmpty) }
            state.set(handle.share, handle.path, nil)
        }
        var body = SmbWriter()
        body.u16(60)
        body.zeros(58)
        reply(header, status: 0, body: body.bytes)
    }

    func queryDirectory(_ header: SmbHeader, _ reader: SmbReader) {
        let id = handleId(reader, 8)
        guard var handle = handles[id] else { return error(header, SmbStatus.accessDenied) }
        if handle.listed { return error(header, SmbStatus.noMoreFiles) }
        handle.listed = true
        handles[id] = handle
        var buffer = SmbWriter()
        var names = [".", ".."] + state.children(handle.share, handle.path)
        names = names.map { $0.split(separator: "\\").last.map(String.init) ?? $0 }
        for (index, name) in names.enumerated() {
            let full = handle.path.isEmpty ? name : handle.path + "\\" + name
            let node = name == "." || name == ".." ? .folder : state.node(handle.share, full)
            let start = buffer.count
            let encoded = SmbText.utf16(name)
            buffer.u32(0)
            buffer.u32(0)
            buffer.u64(0)
            buffer.u64(0)
            buffer.u64(Ntlm.fileTime(Date(timeIntervalSince1970: 1_700_000_000 + Double(index))))
            buffer.u64(0)
            var size: UInt64 = 0
            if case .file(let data)? = node { size = UInt64(data.count) }
            buffer.u64(size)
            buffer.u64(size)
            var attributes: UInt32 = SmbAttributes.normal
            if case .folder? = node { attributes = SmbAttributes.directory }
            if name.hasPrefix(".") && name.count > 2 { attributes |= SmbAttributes.hidden }
            buffer.u32(attributes)
            buffer.u32(UInt32(encoded.count))
            buffer.append(encoded)
            if index < names.count - 1 {
                buffer.align(8)
                buffer.put32(UInt32(buffer.count - start), at: start)
            }
        }
        var body = SmbWriter()
        body.u16(9)
        body.u16(72)
        body.u32(UInt32(buffer.count))
        body.append(buffer.bytes)
        reply(header, status: 0, body: body.bytes)
    }

    func readFile(_ header: SmbHeader, _ reader: SmbReader) {
        let id = handleId(reader, 16)
        guard let handle = handles[id] else { return error(header, SmbStatus.accessDenied) }
        if state.lock.withLock({ state.pendingReads }) {
            reply(header, status: SmbStatus.pending, body: [9, 0, 0, 0, 0, 0, 0, 0, 0], async: true)
        }
        let data: [UInt8]
        if handle.pipe {
            data = Array(pipeOut.prefix(Int(reader.u32(64 + 4))))
            pipeOut.removeFirst(data.count)
        } else {
            guard case .file(let content)? = state.node(handle.share, handle.path) else { return error(header, SmbStatus.nameNotFound) }
            let offset = Int(reader.u64(64 + 8))
            let length = min(Int(reader.u32(64 + 4)), Int(state.maxRead))
            guard offset < content.count else { return error(header, SmbStatus.endOfFile) }
            data = Array(content[offset..<min(content.count, offset + length)])
        }
        var body = SmbWriter()
        body.u16(17)
        body.u8(80)
        body.u8(0)
        body.u32(UInt32(data.count))
        body.u32(0)
        body.u32(0)
        body.append(data)
        reply(header, status: 0, body: body.bytes)
    }

    func writeFile(_ header: SmbHeader, _ reader: SmbReader) {
        let id = handleId(reader, 16)
        guard let handle = handles[id] else { return error(header, SmbStatus.accessDenied) }
        let data = reader.slice(Int(reader.u16(64 + 2)), Int(reader.u32(64 + 4)))
        if handle.pipe {
            pipeOut += rpc(data)
        } else {
            guard case .file(var content)? = state.node(handle.share, handle.path) else { return error(header, SmbStatus.nameNotFound) }
            let offset = Int(reader.u64(64 + 8))
            if content.count < offset + data.count { content += [UInt8](repeating: 0, count: offset + data.count - content.count) }
            content.replaceSubrange(offset..<(offset + data.count), with: data)
            state.set(handle.share, handle.path, .file(content))
        }
        var body = SmbWriter()
        body.u16(17)
        body.u16(0)
        body.u32(UInt32(data.count))
        body.u32(0)
        body.u16(0)
        body.u16(0)
        reply(header, status: 0, body: body.bytes)
    }

    func setInfo(_ header: SmbHeader, _ reader: SmbReader) {
        let id = handleId(reader, 16)
        guard let handle = handles[id], reader.u8(64 + 3) == 10 else { return error(header, SmbStatus.notSupported) }
        let info = SmbReader(reader.slice(Int(reader.u16(64 + 8)), Int(reader.u32(64 + 4))))
        let target = SmbText.string(info.slice(20, Int(info.u32(16))))
        if state.node(handle.share, target) != nil { return error(header, SmbStatus.nameCollision) }
        state.lock.withLock {
            var files = state.shares[handle.share] ?? [:]
            for key in Array(files.keys) where key == handle.path || key.hasPrefix(handle.path + "\\") {
                files[target + key.dropFirst(handle.path.count)] = files.removeValue(forKey: key)
            }
            state.shares[handle.share] = files
        }
        handles[id]?.path = target
        reply(header, status: 0, body: [2, 0])
    }

    func rpc(_ pdu: [UInt8]) -> [UInt8] {
        let reader = SmbReader(pdu)
        if reader.u8(2) == 11 {
            var body = SmbWriter()
            body.u16(4280)
            body.u16(4280)
            body.u32(0x1234)
            let port = Array("\\PIPE\\srvsvc".utf8) + [0]
            body.u16(UInt16(port.count))
            body.append(port)
            while (16 + body.count) % 4 != 0 { body.u8(0) }
            body.u8(1)
            body.zeros(3)
            body.u16(0)
            body.u16(0)
            body.append(SmbSrvsvc.ndrSyntax)
            body.u32(2)
            return SmbSrvsvc.pdu(type: 12, callId: reader.u32(12), body: body.bytes)
        }
        let list = state.lock.withLock { state.shareList }
        var stub = SmbWriter()
        stub.u32(1)
        stub.u32(1)
        stub.u32(0x20000)
        stub.u32(UInt32(list.count))
        stub.u32(0x20004)
        stub.u32(UInt32(list.count))
        for (index, share) in list.enumerated() {
            stub.u32(UInt32(0x20008 + index * 8))
            stub.u32(share.type)
            stub.u32(share.remark.isEmpty ? 0 : UInt32(0x2000c + index * 8))
        }
        for share in list {
            SmbSrvsvc.ndrString(&stub, share.name)
            if !share.remark.isEmpty { SmbSrvsvc.ndrString(&stub, share.remark) }
        }
        stub.u32(UInt32(list.count))
        stub.u32(0)
        stub.u32(0)
        let half = stub.bytes.count / 2
        let parts = [Array(stub.bytes[..<half]), Array(stub.bytes[half...])]
        var out: [UInt8] = []
        for (index, part) in parts.enumerated() {
            var body = SmbWriter()
            body.u32(UInt32(part.count))
            body.u16(0)
            body.u8(0)
            body.u8(0)
            body.append(part)
            var pdu = SmbSrvsvc.pdu(type: 2, callId: reader.u32(12), body: body.bytes)
            pdu[3] = index == 0 ? 1 : 2
            out += pdu
        }
        return out
    }
}
