import Foundation

public enum PortParseError: Error, Equatable, Sendable, CustomStringConvertible {
    case empty
    case invalid(String)
    case outOfRange(String)
    case reversedRange(String)

    public var description: String {
        switch self {
        case .empty: return "Enter at least one port."
        case .invalid(let token): return "“\(token)” is not a port or port range."
        case .outOfRange(let token): return "“\(token)” is outside 1–65535."
        case .reversedRange(let token): return "The range “\(token)” ends before it starts."
        }
    }
}

public struct PortList: Hashable, Sendable, Codable, CustomStringConvertible {
    public let ports: [UInt16]

    public init(_ ports: some Sequence<UInt16>) {
        self.ports = Array(Set(ports.filter { $0 > 0 })).sorted()
    }

    public init(parsing text: String) throws {
        let tokens = text.split { $0 == "," || $0.isWhitespace }
        guard !tokens.isEmpty else { throw PortParseError.empty }
        var set = Set<UInt16>()
        for token in tokens {
            let parts = token.split(separator: "-", omittingEmptySubsequences: false)
            guard parts.count <= 2, parts.allSatisfy({ !$0.isEmpty && $0.allSatisfy(\.isASCIIDigit) && $0.count <= 5 }) else {
                throw PortParseError.invalid(String(token))
            }
            let numbers = parts.compactMap { Int($0) }
            guard numbers.allSatisfy({ (1...65535).contains($0) }) else { throw PortParseError.outOfRange(String(token)) }
            if numbers.count == 2 {
                guard numbers[0] <= numbers[1] else { throw PortParseError.reversedRange(String(token)) }
                for port in numbers[0]...numbers[1] { set.insert(UInt16(port)) }
            } else {
                set.insert(UInt16(numbers[0]))
            }
        }
        ports = set.sorted()
    }

    public var count: Int { ports.count }
    public var isEmpty: Bool { ports.isEmpty }

    public var description: String {
        var parts: [String] = []
        var index = 0
        while index < ports.count {
            var end = index
            while end + 1 < ports.count && ports[end + 1] == ports[end] + 1 { end += 1 }
            parts.append(end - index >= 2 ? "\(ports[index])-\(ports[end])" : (index...end).map { String(ports[$0]) }.joined(separator: ","))
            index = end + 1
        }
        return parts.joined(separator: ",")
    }

    public func union(_ other: PortList) -> PortList { PortList(ports + other.ports) }

    public init(from decoder: Decoder) throws {
        let text = try decoder.singleValueContainer().decode(String.self)
        self = text.isEmpty ? PortList([]) : try PortList(parsing: text)
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(description)
    }
}

public enum PortPreset: String, CaseIterable, Codable, Sendable, Identifiable {
    case quick
    case common
    case web
    case remoteAccess
    case fileSharing
    case mail
    case smartHome
    case media
    case databases
    case wellKnown
    case all

    public var id: String { rawValue }

    public var title: String {
        switch self {
        case .quick: return "Quick (20)"
        case .common: return "Common (100)"
        case .web: return "Web"
        case .remoteAccess: return "Remote access"
        case .fileSharing: return "File sharing"
        case .mail: return "Mail"
        case .smartHome: return "Smart home & IoT"
        case .media: return "Media & casting"
        case .databases: return "Databases"
        case .wellKnown: return "Well-known (1–1024)"
        case .all: return "All ports (1–65535)"
        }
    }

    public var symbolName: String {
        switch self {
        case .quick: return "bolt"
        case .common: return "list.bullet"
        case .web: return "globe"
        case .remoteAccess: return "terminal"
        case .fileSharing: return "folder"
        case .mail: return "envelope"
        case .smartHome: return "house"
        case .media: return "tv"
        case .databases: return "cylinder.split.1x2"
        case .wellKnown: return "number"
        case .all: return "square.grid.3x3"
        }
    }

    public var ports: PortList {
        switch self {
        case .quick: return PortList(PortPreset.quickPorts)
        case .common: return PortList(PortPreset.commonPorts)
        case .web: return PortList([80, 81, 443, 591, 3000, 4443, 5000, 5001, 7080, 7443, 8000, 8008, 8080, 8081, 8088, 8443, 8888, 9000, 9090, 9443])
        case .remoteAccess: return PortList([22, 23, 2222, 3389, 5900, 5901, 5938, 6568, 7070, 8291, 5985, 5986])
        case .fileSharing: return PortList([20, 21, 69, 111, 135, 137, 139, 445, 548, 873, 2049, 5005, 8384, 22000])
        case .mail: return PortList([25, 110, 143, 465, 587, 993, 995, 4190])
        case .smartHome: return PortList([80, 443, 554, 1400, 1883, 1900, 5353, 5540, 6053, 6466, 6467, 8008, 8009, 8060, 8123, 8883, 9000, 9123, 21063, 40000, 49152, 55443])
        case .media: return PortList([554, 1400, 3689, 5000, 7000, 7100, 8008, 8009, 8060, 8096, 8200, 8443, 9080, 32400, 49153])
        case .databases: return PortList([1433, 1521, 3306, 5432, 5984, 6379, 7474, 8086, 9042, 9200, 11211, 27017, 28015])
        case .wellKnown: return PortList(1...1024)
        case .all: return PortList(1...65535)
        }
    }

    static let quickPorts: [UInt16] = [21, 22, 23, 25, 53, 80, 110, 139, 143, 443, 445, 554, 3306, 3389, 5900, 8000, 8080, 8443, 9100, 62078]

    static let commonPorts: [UInt16] = [
        7, 9, 13, 21, 22, 23, 25, 26, 37, 53, 79, 80, 81, 88, 106, 110, 111, 113, 119, 135,
        139, 143, 144, 179, 199, 389, 427, 443, 444, 445, 465, 513, 514, 515, 543, 544, 548, 554, 587, 631,
        646, 873, 990, 993, 995, 1025, 1026, 1027, 1028, 1029, 1110, 1433, 1720, 1723, 1755, 1883, 1900, 2000, 2001, 2049,
        2121, 2717, 3000, 3128, 3306, 3389, 3986, 4899, 5000, 5009, 5051, 5060, 5101, 5190, 5357, 5432, 5631, 5666, 5800, 5900,
        6000, 6001, 6646, 7070, 8000, 8008, 8009, 8080, 8081, 8443, 8888, 9100, 9999, 10000, 32768, 49152, 49153, 49154, 49155, 62078,
    ]
}
