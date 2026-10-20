import Foundation
#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#endif

public struct InterfaceAddress: Hashable, Sendable {
    public var name: String
    public var address: IPv4Addr
    public var netmask: IPv4Addr

    public init(name: String, address: IPv4Addr, netmask: IPv4Addr) {
        self.name = name
        self.address = address
        self.netmask = netmask
    }
}

public enum LocalNetwork {
    static let skippedInterfacePrefixes = ["pdp_ip", "utun", "ipsec", "awdl", "llw", "docker", "veth", "virbr", "br-"]

    public static func current() -> IPv4Network? {
        suggested(from: interfaceAddresses())
    }

    public static func suggested(from interfaces: [InterfaceAddress]) -> IPv4Network? {
        let candidates = interfaces.filter { entry in
            !entry.address.isLoopback && !entry.address.isLinkLocal && entry.address.isLocalScope
                && !skippedInterfacePrefixes.contains { entry.name.hasPrefix($0) }
        }
        guard let entry = candidates.first(where: { $0.name == "en0" }) ?? candidates.first else { return nil }
        let prefix = IPv4Network(address: entry.address, netmask: entry.netmask)?.prefixLength ?? 24
        return IPv4Network(address: entry.address, prefixLength: min(max(prefix, 24), 32))
    }

    public static func interfaceAddresses() -> [InterfaceAddress] {
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0, let first = head else { return [] }
        defer { freeifaddrs(head) }
        var found: [InterfaceAddress] = []
        for entry in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let flags = Int32(truncatingIfNeeded: entry.pointee.ifa_flags)
            guard flags & Int32(IFF_UP) != 0, flags & Int32(IFF_RUNNING) != 0, flags & Int32(IFF_LOOPBACK) == 0,
                  let address = entry.pointee.ifa_addr, address.pointee.sa_family == sa_family_t(AF_INET),
                  let netmask = entry.pointee.ifa_netmask, let name = entry.pointee.ifa_name else { continue }
            found.append(InterfaceAddress(name: String(cString: name), address: ipv4(address), netmask: ipv4(netmask)))
        }
        return found
    }

    private static func ipv4(_ pointer: UnsafeMutablePointer<sockaddr>) -> IPv4Addr {
        pointer.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { IPv4Addr(UInt32(bigEndian: $0.pointee.sin_addr.s_addr)) }
    }
}
