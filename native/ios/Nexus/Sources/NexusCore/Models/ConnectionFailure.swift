import Foundation

public enum PortState: String, Codable, Sendable, CaseIterable, Comparable {
    case open
    case closed
    case timedOut
    case unreachable
    case indeterminate

    public var title: String {
        switch self {
        case .open: return "Open"
        case .closed: return "Closed"
        case .timedOut: return "No response"
        case .unreachable: return "Unreachable"
        case .indeterminate: return "Indeterminate"
        }
    }

    public var explanation: String {
        switch self {
        case .open: return "A TCP connection was established."
        case .closed: return "The host answered and refused the connection."
        case .timedOut: return "No answer before the timeout: filtered by a firewall, or the host is offline."
        case .unreachable: return "The network reported the host or network as unreachable."
        case .indeterminate: return "The attempt failed in a way that does not show whether the port is open."
        }
    }

    public var symbolName: String {
        switch self {
        case .open: return "lock.open"
        case .closed: return "xmark.circle"
        case .timedOut: return "hourglass"
        case .unreachable: return "wifi.exclamationmark"
        case .indeterminate: return "questionmark.circle"
        }
    }

    var rank: Int {
        switch self {
        case .open: return 0
        case .indeterminate: return 1
        case .timedOut: return 2
        case .unreachable: return 3
        case .closed: return 4
        }
    }

    public static func < (lhs: PortState, rhs: PortState) -> Bool { lhs.rank < rhs.rank }
}

public enum ConnectionFailure: Error, Codable, Hashable, Sendable, CustomStringConvertible {
    case refused
    case timedOut
    case hostUnreachable
    case networkUnreachable
    case nameResolution(String)
    case localNetworkDenied
    case resetByPeer
    case tls(String)
    case cancelled
    case unsupported(String)
    case system(code: Int32, message: String)

    public var portState: PortState {
        switch self {
        case .refused: return .closed
        case .timedOut: return .timedOut
        case .hostUnreachable, .networkUnreachable: return .unreachable
        case .nameResolution, .localNetworkDenied, .resetByPeer, .tls, .cancelled, .unsupported, .system: return .indeterminate
        }
    }

    public var title: String {
        switch self {
        case .refused: return "Connection refused"
        case .timedOut: return "Timed out"
        case .hostUnreachable: return "Host unreachable"
        case .networkUnreachable: return "Network unreachable"
        case .nameResolution: return "Name resolution failed"
        case .localNetworkDenied: return "Local network access denied"
        case .resetByPeer: return "Connection reset"
        case .tls: return "TLS failure"
        case .cancelled: return "Cancelled"
        case .unsupported: return "Not supported"
        case .system: return "System error"
        }
    }

    public var description: String {
        switch self {
        case .refused: return "The host answered and refused the connection (TCP reset)."
        case .timedOut: return "No answer arrived before the timeout."
        case .hostUnreachable: return "The host could not be reached (no route or no ARP/NDP reply)."
        case .networkUnreachable: return "There is no route to the network."
        case .nameResolution(let detail): return "The name could not be resolved: \(detail)"
        case .localNetworkDenied: return "iOS blocked the connection: allow Local Network access for this app in Settings › Privacy & Security › Local Network."
        case .resetByPeer: return "The host closed the connection abruptly."
        case .tls(let detail): return "The TLS handshake failed: \(detail)"
        case .cancelled: return "The operation was cancelled."
        case .unsupported(let detail): return detail
        case .system(let code, let message): return "\(message) (error \(code))."
        }
    }
}
