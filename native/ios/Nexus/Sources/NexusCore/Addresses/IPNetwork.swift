import Foundation

public struct IPv4Network: Hashable, Sendable, Codable, CustomStringConvertible {
    public let network: IPv4Addr
    public let prefixLength: Int

    public init?(address: IPv4Addr, prefixLength: Int) {
        guard (0...32).contains(prefixLength) else { return nil }
        self.prefixLength = prefixLength
        network = IPv4Addr(address.value & IPv4Network.mask(prefixLength))
    }

    public init?(address: IPv4Addr, netmask: IPv4Addr) {
        let length = (~netmask.value).leadingZeroBitCount
        guard IPv4Network.mask(length) == netmask.value else { return nil }
        self.init(address: address, prefixLength: length)
    }

    public init?(_ text: some StringProtocol) {
        let parts = text.split(separator: "/", omittingEmptySubsequences: false)
        guard parts.count == 2, let address = IPv4Addr(parts[0]),
              parts[1].allSatisfy(\.isASCIIDigit), let length = Int(parts[1]) else { return nil }
        self.init(address: address, prefixLength: length)
    }

    static func mask(_ length: Int) -> UInt32 {
        length == 0 ? 0 : UInt32.max << UInt32(32 - length)
    }

    public var netmask: IPv4Addr { IPv4Addr(IPv4Network.mask(prefixLength)) }
    public var broadcast: IPv4Addr { IPv4Addr(network.value | ~IPv4Network.mask(prefixLength)) }

    public var addressCount: UInt64 { UInt64(1) << UInt64(32 - prefixLength) }

    public var hostCount: UInt64 {
        prefixLength >= 31 ? addressCount : addressCount - 2
    }

    public var firstHost: IPv4Addr {
        prefixLength >= 31 ? network : IPv4Addr(network.value + 1)
    }

    public var lastHost: IPv4Addr {
        prefixLength >= 31 ? broadcast : IPv4Addr(broadcast.value - 1)
    }

    public func contains(_ address: IPv4Addr) -> Bool {
        address.value & IPv4Network.mask(prefixLength) == network.value
    }

    public func hosts(limit: Int = .max) -> [IPv4Addr] {
        let count = Int(min(hostCount, UInt64(limit)))
        guard count > 0 else { return [] }
        return (0..<count).map { IPv4Addr(firstHost.value + UInt32($0)) }
    }

    public var description: String { "\(network)/\(prefixLength)" }

    public init(from decoder: Decoder) throws {
        let text = try decoder.singleValueContainer().decode(String.self)
        guard let value = IPv4Network(text) else {
            throw DecodingError.dataCorrupted(.init(codingPath: decoder.codingPath, debugDescription: "Invalid IPv4 network \(text)"))
        }
        self = value
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(description)
    }
}

public struct IPv6Network: Hashable, Sendable, Codable, CustomStringConvertible {
    public let network: IPv6Addr
    public let prefixLength: Int

    public init?(address: IPv6Addr, prefixLength: Int) {
        guard (0...128).contains(prefixLength) else { return nil }
        self.prefixLength = prefixLength
        var bytes = address.bytes
        for index in 0..<16 {
            let bitsBefore = index * 8
            if bitsBefore >= prefixLength {
                bytes[index] = 0
            } else if bitsBefore + 8 > prefixLength {
                bytes[index] &= UInt8(0xFF << (8 - (prefixLength - bitsBefore)) & 0xFF)
            }
        }
        network = IPv6Addr(bytes: bytes)!
    }

    public init?(_ text: some StringProtocol) {
        let parts = text.split(separator: "/", omittingEmptySubsequences: false)
        guard parts.count == 2, let address = IPv6Addr(parts[0]),
              parts[1].allSatisfy(\.isASCIIDigit), let length = Int(parts[1]) else { return nil }
        self.init(address: address, prefixLength: length)
    }

    public func contains(_ address: IPv6Addr) -> Bool {
        IPv6Network(address: address, prefixLength: prefixLength)?.network == network
    }

    public var addressCount: UInt64? {
        prefixLength >= 65 ? UInt64(1) << UInt64(128 - prefixLength) : nil
    }

    public func addresses(limit: Int) -> [IPv6Addr] {
        guard let total = addressCount else { return [] }
        let count = Int(min(total, UInt64(limit)))
        var result: [IPv6Addr] = []
        result.reserveCapacity(count)
        for offset in 0..<UInt64(count) {
            var bytes = network.bytes
            var carry = offset
            var index = 15
            while carry > 0 && index >= 0 {
                let sum = UInt64(bytes[index]) + (carry & 0xFF)
                bytes[index] = UInt8(sum & 0xFF)
                carry = (carry >> 8) + (sum >> 8)
                index -= 1
            }
            result.append(IPv6Addr(bytes: bytes)!)
        }
        return result
    }

    public var description: String { "\(network)/\(prefixLength)" }

    public init(from decoder: Decoder) throws {
        let text = try decoder.singleValueContainer().decode(String.self)
        guard let value = IPv6Network(text) else {
            throw DecodingError.dataCorrupted(.init(codingPath: decoder.codingPath, debugDescription: "Invalid IPv6 network \(text)"))
        }
        self = value
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(description)
    }
}

public enum IPNetwork: Hashable, Sendable, Codable, CustomStringConvertible {
    case v4(IPv4Network)
    case v6(IPv6Network)

    public init?(_ text: some StringProtocol) {
        if let v4 = IPv4Network(text) {
            self = .v4(v4)
        } else if let v6 = IPv6Network(text) {
            self = .v6(v6)
        } else {
            return nil
        }
    }

    public func contains(_ address: IPAddr) -> Bool {
        switch (self, address) {
        case (.v4(let network), .v4(let value)): return network.contains(value)
        case (.v6(let network), .v6(let value)): return network.contains(value)
        case (.v4(let network), .v6(let value)): return value.ipv4Mapped.map(network.contains) ?? false
        case (.v6, .v4): return false
        }
    }

    public var description: String {
        switch self {
        case .v4(let network): return network.description
        case .v6(let network): return network.description
        }
    }

    public init(from decoder: Decoder) throws {
        let text = try decoder.singleValueContainer().decode(String.self)
        guard let value = IPNetwork(text) else {
            throw DecodingError.dataCorrupted(.init(codingPath: decoder.codingPath, debugDescription: "Invalid network \(text)"))
        }
        self = value
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(description)
    }
}
