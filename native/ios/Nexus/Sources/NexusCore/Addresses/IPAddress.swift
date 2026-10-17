import Foundation

public struct IPv4Addr: Hashable, Sendable, Comparable, CustomStringConvertible {
    public let value: UInt32

    public init(_ value: UInt32) {
        self.value = value
    }

    public init(_ a: UInt8, _ b: UInt8, _ c: UInt8, _ d: UInt8) {
        value = UInt32(a) << 24 | UInt32(b) << 16 | UInt32(c) << 8 | UInt32(d)
    }

    public init?(bytes: some Collection<UInt8>) {
        guard bytes.count == 4 else { return nil }
        value = bytes.reduce(0) { $0 << 8 | UInt32($1) }
    }

    public init?(_ text: some StringProtocol) {
        let parts = text.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 4 else { return nil }
        var result: UInt32 = 0
        for part in parts {
            guard !part.isEmpty, part.count <= 3, part.allSatisfy(\.isASCIIDigit) else { return nil }
            if part.count > 1 && part.first == "0" { return nil }
            guard let octet = UInt8(part) else { return nil }
            result = result << 8 | UInt32(octet)
        }
        value = result
    }

    public var octets: [UInt8] {
        [UInt8(value >> 24), UInt8(value >> 16 & 0xFF), UInt8(value >> 8 & 0xFF), UInt8(value & 0xFF)]
    }

    public var description: String {
        octets.map(String.init).joined(separator: ".")
    }

    public var isUnspecified: Bool { value == 0 }
    public var isLoopback: Bool { value >> 24 == 127 }
    public var isPrivate: Bool {
        value >> 24 == 10 || value >> 20 == 0xAC1 || value >> 16 == 0xC0A8
    }
    public var isLinkLocal: Bool { value >> 16 == 0xA9FE }
    public var isSharedAddressSpace: Bool { value >> 22 == (100 << 2 | 1) }
    public var isMulticast: Bool { value >> 28 == 0xE }
    public var isBroadcast: Bool { value == .max }
    public var isDocumentation: Bool {
        value >> 8 == 0xC00002 || value >> 8 == 0xC63364 || value >> 8 == 0xCB0071
    }

    public var isLocalScope: Bool {
        isLoopback || isPrivate || isLinkLocal || isSharedAddressSpace
    }

    public var reversePointerName: String {
        octets.reversed().map(String.init).joined(separator: ".") + ".in-addr.arpa"
    }

    public static func < (lhs: IPv4Addr, rhs: IPv4Addr) -> Bool {
        lhs.value < rhs.value
    }
}

public struct IPv6Addr: Hashable, Sendable, Comparable, CustomStringConvertible {
    public let bytes: [UInt8]
    public let zone: String?

    public init?(bytes: some Collection<UInt8>, zone: String? = nil) {
        guard bytes.count == 16 else { return nil }
        self.bytes = Array(bytes)
        self.zone = zone.flatMap { $0.isEmpty ? nil : $0 }
    }

    public init?(_ input: some StringProtocol) {
        var text = Substring(input)
        if text.hasPrefix("[") && text.hasSuffix("]") {
            text = text.dropFirst().dropLast()
        }
        var zone: String?
        if let percent = text.firstIndex(of: "%") {
            zone = String(text[text.index(after: percent)...])
            text = text[..<percent]
            guard zone?.isEmpty == false else { return nil }
        }
        guard text.contains(":"), text.count <= 45 else { return nil }
        var tail: [UInt8] = []
        if let lastColon = text.lastIndex(of: ":"), text[text.index(after: lastColon)...].contains(".") {
            guard let v4 = IPv4Addr(text[text.index(after: lastColon)...]) else { return nil }
            tail = v4.octets
            text = text[...lastColon]
            if text.hasSuffix("::") == false {
                text = text.dropLast()
            }
        }
        let halves = text.components(separatedBy: "::")
        guard halves.count <= 2 else { return nil }
        func groups(_ part: String) -> [UInt16]? {
            if part.isEmpty { return [] }
            var result: [UInt16] = []
            for group in part.split(separator: ":", omittingEmptySubsequences: false) {
                guard !group.isEmpty, group.count <= 4, let number = UInt16(group, radix: 16) else { return nil }
                result.append(number)
            }
            return result
        }
        guard let head = groups(halves[0]) else { return nil }
        let rest = halves.count == 2 ? groups(halves[1]) : []
        guard let rest else { return nil }
        let tailGroups = tail.count / 2
        let used = head.count + rest.count + tailGroups
        var all: [UInt16]
        if halves.count == 2 {
            guard used <= 7 else { return nil }
            all = head + Array(repeating: 0, count: 8 - used) + rest
        } else {
            guard used == 8 else { return nil }
            all = head
        }
        var result: [UInt8] = []
        for group in all {
            result.append(UInt8(group >> 8))
            result.append(UInt8(group & 0xFF))
        }
        result.append(contentsOf: tail)
        guard result.count == 16 else { return nil }
        bytes = result
        self.zone = zone
    }

    public var groups: [UInt16] {
        stride(from: 0, to: 16, by: 2).map { UInt16(bytes[$0]) << 8 | UInt16(bytes[$0 + 1]) }
    }

    public var ipv4Mapped: IPv4Addr? {
        guard bytes[0..<10].allSatisfy({ $0 == 0 }), bytes[10] == 0xFF, bytes[11] == 0xFF else { return nil }
        return IPv4Addr(bytes: bytes[12..<16])
    }

    public var description: String {
        let suffix = zone.map { "%" + $0 } ?? ""
        if let mapped = ipv4Mapped {
            return "::ffff:\(mapped)" + suffix
        }
        let groups = groups
        var bestStart = -1
        var bestLength = 0
        var index = 0
        while index < 8 {
            guard groups[index] == 0 else { index += 1; continue }
            var end = index
            while end < 8 && groups[end] == 0 { end += 1 }
            if end - index > bestLength {
                bestStart = index
                bestLength = end - index
            }
            index = end
        }
        let hex = groups.map { String($0, radix: 16) }
        guard bestLength >= 2 else {
            return hex.joined(separator: ":") + suffix
        }
        let left = hex[0..<bestStart].joined(separator: ":")
        let right = hex[(bestStart + bestLength)...].joined(separator: ":")
        return left + "::" + right + suffix
    }

    public var isUnspecified: Bool { bytes.allSatisfy { $0 == 0 } }
    public var isLoopback: Bool { bytes[0..<15].allSatisfy { $0 == 0 } && bytes[15] == 1 }
    public var isLinkLocal: Bool { bytes[0] == 0xFE && bytes[1] & 0xC0 == 0x80 }
    public var isUniqueLocal: Bool { bytes[0] & 0xFE == 0xFC }
    public var isMulticast: Bool { bytes[0] == 0xFF }
    public var isDocumentation: Bool { bytes[0] == 0x20 && bytes[1] == 0x01 && bytes[2] == 0x0D && bytes[3] == 0xB8 }

    public var isLocalScope: Bool {
        if let mapped = ipv4Mapped { return mapped.isLocalScope }
        return isLoopback || isLinkLocal || isUniqueLocal
    }

    public var reversePointerName: String {
        var nibbles: [String] = []
        for byte in bytes.reversed() {
            nibbles.append(String(byte & 0x0F, radix: 16))
            nibbles.append(String(byte >> 4, radix: 16))
        }
        return nibbles.joined(separator: ".") + ".ip6.arpa"
    }

    public func withoutZone() -> IPv6Addr {
        IPv6Addr(bytes: bytes)!
    }

    public static func < (lhs: IPv6Addr, rhs: IPv6Addr) -> Bool {
        lhs.bytes.lexicographicallyPrecedes(rhs.bytes)
    }
}

public enum IPAddr: Hashable, Sendable, Comparable, CustomStringConvertible {
    case v4(IPv4Addr)
    case v6(IPv6Addr)

    public enum Family: String, Codable, Sendable {
        case ipv4
        case ipv6
    }

    public init?(_ text: some StringProtocol) {
        if let v4 = IPv4Addr(text) {
            self = .v4(v4)
        } else if let v6 = IPv6Addr(text) {
            self = .v6(v6)
        } else {
            return nil
        }
    }

    public var family: Family {
        switch self {
        case .v4: return .ipv4
        case .v6: return .ipv6
        }
    }

    public var description: String {
        switch self {
        case .v4(let address): return address.description
        case .v6(let address): return address.description
        }
    }

    public var urlHost: String {
        switch self {
        case .v4(let address): return address.description
        case .v6(let address): return "[\(address.withoutZone())]"
        }
    }

    public var isLocalScope: Bool {
        switch self {
        case .v4(let address): return address.isLocalScope
        case .v6(let address): return address.isLocalScope
        }
    }

    public var isLoopback: Bool {
        switch self {
        case .v4(let address): return address.isLoopback
        case .v6(let address): return address.isLoopback || address.ipv4Mapped?.isLoopback == true
        }
    }

    public var isMulticastOrBroadcast: Bool {
        switch self {
        case .v4(let address): return address.isMulticast || address.isBroadcast
        case .v6(let address): return address.isMulticast
        }
    }

    public var isUnspecified: Bool {
        switch self {
        case .v4(let address): return address.isUnspecified
        case .v6(let address): return address.isUnspecified
        }
    }

    public var reversePointerName: String {
        switch self {
        case .v4(let address): return address.reversePointerName
        case .v6(let address): return address.ipv4Mapped?.reversePointerName ?? address.reversePointerName
        }
    }

    public static func < (lhs: IPAddr, rhs: IPAddr) -> Bool {
        switch (lhs, rhs) {
        case (.v4(let a), .v4(let b)): return a < b
        case (.v6(let a), .v6(let b)): return a < b
        case (.v4, .v6): return true
        case (.v6, .v4): return false
        }
    }
}

extension IPv4Addr: Codable {
    public init(from decoder: Decoder) throws {
        let text = try decoder.singleValueContainer().decode(String.self)
        guard let value = IPv4Addr(text) else {
            throw DecodingError.dataCorrupted(.init(codingPath: decoder.codingPath, debugDescription: "Invalid IPv4 address \(text)"))
        }
        self = value
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(description)
    }
}

extension IPv6Addr: Codable {
    public init(from decoder: Decoder) throws {
        let text = try decoder.singleValueContainer().decode(String.self)
        guard let value = IPv6Addr(text) else {
            throw DecodingError.dataCorrupted(.init(codingPath: decoder.codingPath, debugDescription: "Invalid IPv6 address \(text)"))
        }
        self = value
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(description)
    }
}

extension IPAddr: Codable {
    public init(from decoder: Decoder) throws {
        let text = try decoder.singleValueContainer().decode(String.self)
        guard let value = IPAddr(text) else {
            throw DecodingError.dataCorrupted(.init(codingPath: decoder.codingPath, debugDescription: "Invalid IP address \(text)"))
        }
        self = value
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(description)
    }
}

extension Character {
    var isASCIIDigit: Bool {
        guard let ascii = asciiValue else { return false }
        return ascii >= 48 && ascii <= 57
    }
}
