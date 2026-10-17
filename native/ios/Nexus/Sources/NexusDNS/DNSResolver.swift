import Foundation
import NexusCore

public protocol DNSResolving: Sendable {
    func lookup(name: String, type: DNSRecordType, timeout: Duration) async throws -> DNSLookup
    func resolve(host: String, timeout: Duration) async throws -> ResolvedHost
}

public struct DNSResolver: DNSResolving, HostResolving {
    public let connector: any DatagramConnector
    public let resolver: Endpoint

    public init(resolver: Endpoint = Endpoint(host: "1.1.1.1", port: 53), connector: any DatagramConnector = Transports.defaultDatagramConnector) {
        self.resolver = resolver
        self.connector = connector
    }

    public func lookup(name: String, type: DNSRecordType, timeout: Duration = .seconds(5)) async throws -> DNSLookup {
        let normalized = name.hasSuffix(".") ? String(name.dropLast()) : name
        guard !normalized.isEmpty else { throw ConnectionFailure.nameResolution("Enter a hostname.") }
        let identifier = UInt16.random(in: .min ... .max)
        let request = try DNSWire.query(identifier: identifier, name: normalized, type: type)
        let started = ContinuousClock.now
        let socket = try await connector.open(to: resolver, timeout: timeout)
        defer { socket.close() }
        try await socket.send(request)
        let response = try await socket.receive(timeout: timeout)
        let parsed = try DNSWire.parse(response, expectedIdentifier: identifier)
        let elapsed = started.duration(to: .now).milliseconds
        return DNSLookup(name: normalized, type: type, answers: parsed.answers, authority: parsed.authority, additional: parsed.additional, responseCode: parsed.responseCode, resolver: resolver.description, latencyMilliseconds: elapsed, error: DNSWire.responseError(parsed.responseCode))
    }

    public func resolve(host: String, timeout: Duration = .seconds(5)) async throws -> ResolvedHost {
        if let address = IPAddr(host) { return ResolvedHost(name: host, addresses: [address]) }
        async let v4: DNSLookup? = try? lookup(name: host, type: .a, timeout: timeout)
        async let v6: DNSLookup? = try? lookup(name: host, type: .aaaa, timeout: timeout)
        let results = await [v4, v6].compactMap { $0 }
        let addresses = results.flatMap { $0.answers.compactMap { IPAddr($0.value) } }
        guard !addresses.isEmpty else { throw ConnectionFailure.nameResolution("No A or AAAA record returned for \(host).") }
        return ResolvedHost(name: host, addresses: Array(Set(addresses)).sorted())
    }
}

enum DNSWire {
    struct Message {
        var responseCode: Int
        var answers: [DNSRecord]
        var authority: [DNSRecord]
        var additional: [DNSRecord]
    }

    static func query(identifier: UInt16, name: String, type: DNSRecordType) throws -> Data {
        var data = Data()
        append(identifier, to: &data)
        append(0x0100, to: &data)
        append(1, to: &data)
        append(0, to: &data)
        append(0, to: &data)
        append(0, to: &data)
        try appendName(name, to: &data)
        append(type.rawValue, to: &data)
        append(1, to: &data)
        return data
    }

    static func parse(_ data: Data, expectedIdentifier: UInt16) throws -> Message {
        var cursor = 0
        let identifier = try read16(data, &cursor)
        guard identifier == expectedIdentifier else { throw ConnectionFailure.system(code: 0, message: "DNS response identifier did not match request.") }
        let flags = try read16(data, &cursor)
        guard flags & 0x8000 != 0 else { throw ConnectionFailure.system(code: 0, message: "DNS response was not a reply.") }
        let questions = try read16(data, &cursor)
        let answerCount = try read16(data, &cursor)
        let authorityCount = try read16(data, &cursor)
        let additionalCount = try read16(data, &cursor)
        for _ in 0..<questions { _ = try readName(data, &cursor); cursor += 4; guard cursor <= data.count else { throw malformed } }
        let answers = try readRecords(data, &cursor, count: answerCount)
        let authority = try readRecords(data, &cursor, count: authorityCount)
        let additional = try readRecords(data, &cursor, count: additionalCount)
        return Message(responseCode: Int(flags & 0x000F), answers: answers, authority: authority, additional: additional)
    }

    static func readRecords(_ data: Data, _ cursor: inout Int, count: UInt16) throws -> [DNSRecord] {
        try (0..<count).compactMap { _ in
            let name = try readName(data, &cursor)
            let rawType = try read16(data, &cursor)
            _ = try read16(data, &cursor)
            let ttl = try read32(data, &cursor)
            let length = Int(try read16(data, &cursor))
            let end = cursor + length
            guard end <= data.count else { throw malformed }
            defer { cursor = end }
            guard let type = DNSRecordType(rawValue: rawType) else { return nil }
            let value = try recordValue(data, offset: cursor, end: end, type: type)
            return DNSRecord(name: name, type: type, ttl: ttl, value: value)
        }
    }

    static func recordValue(_ data: Data, offset: Int, end: Int, type: DNSRecordType) throws -> String {
        switch type {
        case .a:
            guard end - offset == 4, let value = IPv4Addr(bytes: data[offset..<end]) else { throw malformed }
            return value.description
        case .aaaa:
            guard end - offset == 16, let value = IPv6Addr(bytes: data[offset..<end]) else { throw malformed }
            return value.description
        case .cname, .ns, .ptr:
            var cursor = offset
            return try readName(data, &cursor)
        case .mx:
            var cursor = offset
            let preference = try read16(data, &cursor)
            return "\(preference) \(try readName(data, &cursor))"
        case .txt:
            var cursor = offset
            var values: [String] = []
            while cursor < end {
                let length = Int(data[cursor]); cursor += 1
                guard cursor + length <= end else { throw malformed }
                values.append(String(decoding: data[cursor..<(cursor + length)], as: UTF8.self))
                cursor += length
            }
            return values.joined(separator: " ")
        case .soa:
            var cursor = offset
            let primary = try readName(data, &cursor)
            let mailbox = try readName(data, &cursor)
            return "\(primary) \(mailbox)"
        }
    }

    static func appendName(_ name: String, to data: inout Data) throws {
        for label in name.split(separator: ".", omittingEmptySubsequences: false) {
            guard !label.isEmpty, label.utf8.count <= 63 else { throw ConnectionFailure.nameResolution("Invalid DNS name \(name).") }
            data.append(UInt8(label.utf8.count)); data.append(contentsOf: label.utf8)
        }
        data.append(0)
    }

    static func readName(_ data: Data, _ cursor: inout Int, jumps: Int = 0) throws -> String {
        guard jumps < 20 else { throw malformed }
        var labels: [String] = []
        var position = cursor
        var didJump = false
        while true {
            guard position < data.count else { throw malformed }
            let length = data[position]
            if length & 0xC0 == 0xC0 {
                guard position + 1 < data.count else { throw malformed }
                let pointer = Int(length & 0x3F) << 8 | Int(data[position + 1])
                if !didJump { cursor = position + 2 }
                position = pointer
                didJump = true
                continue
            }
            guard length & 0xC0 == 0 else { throw malformed }
            position += 1
            if length == 0 {
                if !didJump { cursor = position }
                return labels.isEmpty ? "." : labels.joined(separator: ".")
            }
            let end = position + Int(length)
            guard end <= data.count else { throw malformed }
            labels.append(String(decoding: data[position..<end], as: UTF8.self))
            position = end
        }
    }

    static func append(_ value: UInt16, to data: inout Data) { data.append(UInt8(value >> 8)); data.append(UInt8(value & 0xFF)) }
    static func read16(_ data: Data, _ cursor: inout Int) throws -> UInt16 { guard cursor + 2 <= data.count else { throw malformed }; defer { cursor += 2 }; return UInt16(data[cursor]) << 8 | UInt16(data[cursor + 1]) }
    static func read32(_ data: Data, _ cursor: inout Int) throws -> UInt32 { guard cursor + 4 <= data.count else { throw malformed }; defer { cursor += 4 }; return UInt32(data[cursor]) << 24 | UInt32(data[cursor + 1]) << 16 | UInt32(data[cursor + 2]) << 8 | UInt32(data[cursor + 3]) }
    static func responseError(_ code: Int) -> String? { [0: nil, 1: "Format error", 2: "Server failure", 3: "Name does not exist", 4: "Not implemented", 5: "Refused"][code] ?? "DNS response code \(code)" }
    static let malformed = ConnectionFailure.system(code: 0, message: "Malformed DNS response.")
}
