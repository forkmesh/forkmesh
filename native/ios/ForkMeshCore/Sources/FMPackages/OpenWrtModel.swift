import FMBase
import FMCrypto
import Foundation

public enum OpenWrt {
    public static let packageId = "openwrt"
    public static let anonymousSession = String(repeating: "0", count: 32)
    public static let sessionTimeout = 3600
    public static let rollbackSeconds = 90
}

public enum UbusStatus: Int, Sendable {
    case ok = 0
    case invalidCommand = 1
    case invalidArgument = 2
    case methodNotFound = 3
    case notFound = 4
    case noData = 5
    case permissionDenied = 6
    case timeout = 7
    case notSupported = 8
    case unknownError = 9
    case connectionFailed = 10
}

public struct OpenWrtResult: Sendable, Equatable {
    public var ok: Bool = false
    public var status: Int = -1
    public var data: JSONObject = JSONObject()
    public var error: String = ""
    public var accessDenied: Bool = false
    public var unreachable: Bool = false
    public var certificate: String = ""

    public init(ok: Bool = false, status: Int = -1, data: JSONObject = JSONObject(), error: String = "") {
        self.ok = ok
        self.status = status
        self.data = data
        self.error = error
    }

    public static func failure(_ message: String) -> OpenWrtResult { OpenWrtResult(error: message) }
}

public struct OpenWrtProblem: Error, Equatable, Sendable {
    public let message: String
    public init(_ message: String) { self.message = message }
}

public struct OpenWrtAddress: Equatable, Sendable {
    public var base: URL?
    public var error: String = ""
    public var encrypted: Bool = false
    public var host: String { base?.host ?? "" }
    public var valid: Bool { error.isEmpty && base != nil }
}

public struct UciSection: Equatable, Sendable, Identifiable {
    public var name: String
    public var type: String
    public var anonymous: Bool
    public var index: Int
    public var options: JSONObject

    public var id: String { name }

    public init(name: String, type: String, anonymous: Bool = false, index: Int = 0, options: JSONObject = JSONObject()) {
        self.name = name
        self.type = type
        self.anonymous = anonymous
        self.index = index
        self.options = options
    }

    public func option(_ key: String, _ fallback: String = "") -> String {
        guard let value = options[key] else { return fallback }
        return OpenWrtModel.optionText(value)
    }

    public func flag(_ key: String, _ fallback: Bool = false) -> Bool {
        guard options.contains(key) else { return fallback }
        return ["1", "on", "true", "yes", "enabled"].contains(option(key).lowercased())
    }

    public func isList(_ key: String) -> Bool { options[key]?.arrayValue != nil }
}

public struct UciEdit: Equatable, Sendable {
    public var values: JSONObject
    public var removed: [String]

    public init(values: JSONObject = JSONObject(), removed: [String] = []) {
        self.values = values
        self.removed = removed
    }
}

public struct OpenWrtLease: Equatable, Sendable, Identifiable {
    public var expires: Int64
    public var mac: String
    public var ip: String
    public var hostname: String
    public var id: String { mac }

    public init(expires: Int64, mac: String, ip: String, hostname: String) {
        self.expires = expires
        self.mac = mac
        self.ip = ip
        self.hostname = hostname
    }
}

public struct OpenWrtPackage: Equatable, Sendable, Identifiable {
    public var name: String
    public var version: String
    public var id: String { name }
}

public struct OpenWrtPackageTool: Equatable, Sendable {
    public var program: String
    public var wrapper: Bool
}

public struct OpenWrtService: Equatable, Sendable, Identifiable {
    public var name: String
    public var enabled: Bool
    public var running: Bool
    public var id: String { name }
}

public struct OpenWrtInterfaceStatus: Equatable, Sendable, Identifiable {
    public var name: String = ""
    public var proto: String = ""
    public var device: String = ""
    public var up: Bool = false
    public var uptime: Int64 = 0
    public var ipv4: [String] = []
    public var ipv6: [String] = []
    public var dns: [String] = []
    public var gateway: String = ""
    public var id: String { name }
}

public struct OpenWrtChoice: Equatable, Sendable, Identifiable {
    public var value: String
    public var label: String
    public var id: String { value }
}

public struct OpenWrtStorageCheck: Equatable, Sendable {
    public var packagesKnown = false
    public var installed: [String] = []
    public var blockTool = false
    public var fstabExists = false
    public var autoMount = false
    public var autoMountPending = false
    public var usbKnown = false
    public var usbControllers = 0
    public var usbDevices: [String] = []
    public var partitionsKnown = false
    public var partitions: [String] = []
    public var disks = 0
    public var mounted = 0
    public var listProblem = ""
    public var kernelLines: [String] = []

    public init() {}
}

public struct OpenWrtBlockDevice: Equatable, Sendable {
    public var name: String = ""
    public var device: String = ""
    public var uuid: String = ""
    public var label: String = ""
    public var type: String = ""
    public var mount: String = ""
    public var size: Int64 = 0
}

public struct OpenWrtMountPoint: Equatable, Sendable {
    public var device: String
    public var mount: String
    public var type: String
    public var size: Int64
    public var avail: Int64
}

public struct OpenWrtDisk: Equatable, Sendable, Identifiable {
    public var block: OpenWrtBlockDevice
    public var mount: String = ""
    public var size: Int64 = 0
    public var avail: Int64 = 0
    public var usageKnown: Bool = false
    public var fstabSection: String = ""
    public var bootTarget: String = ""
    public var bootEnabled: Bool = false

    public var id: String { block.name }
    public var mounted: Bool { !mount.isEmpty }
    public var title: String { block.label.isEmpty ? block.name : "\(block.label) (\(block.name))" }
}

public struct OpenWrtShare: Equatable, Sendable, Identifiable {
    public var section: String
    public var name: String
    public var path: String
    public var readOnly: Bool
    public var guests: Bool
    public var id: String { section }
}

public enum OpenWrtModel {
    public static func callBody(id: Int, session: String, object: String, method: String, args: JSONObject = JSONObject()) -> Data {
        let body: JSONValue = [
            "jsonrpc": "2.0",
            "id": .int(Int64(id)),
            "method": "call",
            "params": [.string(session.isEmpty ? OpenWrt.anonymousSession : session), .string(object), .string(method), .object(args)],
        ]
        return JSON.encodeData(body)
    }

    public static func statusMessage(_ status: Int) -> String {
        switch UbusStatus(rawValue: status) {
        case .ok: return "Done."
        case .invalidCommand: return "The router did not recognise the command."
        case .invalidArgument: return "The router rejected a value as invalid."
        case .methodNotFound: return "The router does not offer this function. It may need an extra package (LuCI provides most of them)."
        case .notFound: return "The router could not find what was asked for."
        case .noData: return "The router had nothing to return."
        case .permissionDenied: return "The router refused: the session expired or this account is not allowed to do that."
        case .timeout: return "The router took too long to answer."
        case .notSupported: return "The router does not support this."
        case .unknownError: return "The router reported an unknown error."
        case .connectionFailed: return "A service on the router is not running."
        case nil: return "The router answered with error \(status)."
        }
    }

    public static func parseReply(_ body: Data, httpStatus: Int = 200) -> OpenWrtResult {
        var result = OpenWrtResult()
        guard let root = (try? JSON.parse(body))?.objectValue else {
            if httpStatus == 404 {
                result.error = "This device has no /ubus endpoint. Is it an OpenWrt router with uhttpd-mod-ubus and rpcd installed?"
            } else if httpStatus == 401 || httpStatus == 403 {
                result.error = statusMessage(UbusStatus.permissionDenied.rawValue)
                result.accessDenied = true
            } else {
                result.error = "The router did not answer with ubus JSON (HTTP \(httpStatus)). Is this an OpenWrt router?"
            }
            return result
        }
        if root.contains("error") {
            let error = root.obj("error")
            let code = error.int("code")
            result.status = code
            switch code {
            case -32002:
                result.accessDenied = true
                result.error = statusMessage(UbusStatus.permissionDenied.rawValue)
            case -32000:
                result.error = "The router does not offer this function (object not found). It may need an extra package; LuCI provides most of them."
            case -32601:
                result.error = statusMessage(UbusStatus.methodNotFound.rawValue)
            case -32602:
                result.error = statusMessage(UbusStatus.invalidArgument.rawValue)
            default:
                result.error = "The router answered: \(error.str("message", "unknown error"))"
            }
            return result
        }
        let values = root.list("result")
        guard let first = values.first, let status = first.intValue else {
            result.error = "The router's answer had no result."
            return result
        }
        result.status = status
        result.data = values.count > 1 ? values[1].objectOrEmpty : JSONObject()
        result.ok = status == 0
        if !result.ok {
            result.accessDenied = status == UbusStatus.permissionDenied.rawValue
            result.error = statusMessage(status)
        }
        return result
    }

    public static func ipv4Bytes(_ text: String) -> [Int]? {
        let parts = text.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 4 else { return nil }
        var bytes: [Int] = []
        for part in parts {
            guard !part.isEmpty, part.count <= 3, part.allSatisfy(\.isNumber), let value = Int(part), value <= 255 else { return nil }
            if part.count > 1 && part.hasPrefix("0") { return nil }
            bytes.append(value)
        }
        return bytes
    }

    static func localIpv4(_ bytes: [Int]) -> Bool {
        let a = bytes[0], b = bytes[1]
        return a == 10 || a == 127 || (a == 172 && (16...31).contains(b)) || (a == 192 && b == 168) || (a == 169 && b == 254)
    }

    static func localIpv6(_ text: String) -> Bool? {
        guard text.contains(":") else { return nil }
        let lower = text.lowercased()
        if lower == "::1" { return true }
        let first = lower.split(separator: ":", omittingEmptySubsequences: false).first.map(String.init) ?? ""
        guard !first.isEmpty, let value = Int(first, radix: 16) else { return false }
        return (value & 0xfe00) == 0xfc00 || (value & 0xffc0) == 0xfe80
    }

    public static func isLiteralAddress(_ host: String) -> Bool {
        let name = host.trimmingCharacters(in: CharacterSet(charactersIn: "[]"))
        return ipv4Bytes(name) != nil || name.contains(":")
    }

    public static func isLocalHost(_ host: String) -> Bool {
        let name = host.trimmingCharacters(in: .whitespaces).lowercased().trimmingCharacters(in: CharacterSet(charactersIn: "[]"))
        if name.isEmpty { return false }
        if let bytes = ipv4Bytes(name) { return localIpv4(bytes) }
        if let v6 = localIpv6(name) { return v6 }
        if name == "localhost" || !name.contains(".") { return true }
        return [".lan", ".local", ".home", ".home.arpa", ".internal", ".localdomain", ".router"].contains { name.hasSuffix($0) }
    }

    public static func parseAddress(_ input: String) -> OpenWrtAddress {
        var address = OpenWrtAddress()
        var text = input.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.isEmpty {
            address.error = "Enter the router's address, for example 192.168.1.1."
            return address
        }
        if !text.contains("://") { text = "http://" + text }
        guard let components = URLComponents(string: text), let scheme = components.scheme?.lowercased(),
              scheme == "http" || scheme == "https", let host = components.host, !host.isEmpty else {
            address.error = "\"\(input.trimmingCharacters(in: .whitespaces))\" is not a router address. Use an IP address or a name such as openwrt.lan."
            return address
        }
        if components.user != nil || components.password != nil {
            address.error = "Leave the user name out of the address; type it in the User field."
            return address
        }
        let bare = host.trimmingCharacters(in: CharacterSet(charactersIn: "[]"))
        if isLiteralAddress(bare) && !isLocalHost(bare) {
            address.error = "\(bare) is not on your local network. Only routers on your own network can be managed, so the router password never crosses the internet."
            return address
        }
        var base = URLComponents()
        base.scheme = scheme
        base.host = host
        base.port = components.port
        address.base = base.url
        address.encrypted = scheme == "https"
        return address
    }

    public static func uciSections(_ values: JSONObject, type: String = "") -> [UciSection] {
        var sections: [UciSection] = []
        for (key, value) in values {
            let raw = value.objectOrEmpty
            var options = JSONObject()
            for (name, option) in raw where !name.hasPrefix(".") { options[name] = option }
            let section = UciSection(
                name: raw.str(".name", key), type: raw.str(".type"), anonymous: raw.bool(".anonymous"),
                index: raw.int(".index"), options: options)
            if !type.isEmpty && section.type != type { continue }
            sections.append(section)
        }
        return sections.enumerated().sorted { ($0.element.index, $0.offset) < ($1.element.index, $1.offset) }.map(\.element)
    }

    public static func optionText(_ value: JSONValue) -> String {
        switch value {
        case .array(let items): return items.map(optionText).joined(separator: " ")
        case .bool(let flag): return flag ? "1" : "0"
        case .null: return ""
        default: return value.text
        }
    }

    public static func optionValue(_ text: String, list: Bool) -> JSONValue {
        if !list { return .string(text.trimmingCharacters(in: .whitespacesAndNewlines)) }
        let parts = text.split { $0 == " " || $0 == "," || $0 == "\n" || $0 == "\t" }.map(String.init)
        return .array(parts.map(JSONValue.string))
    }

    public static func isSecretOption(_ name: String) -> Bool {
        ["key", "key1", "key2", "key3", "key4", "password", "sae_password", "auth_secret", "acct_secret", "private_key", "preshared_key", "psk"]
            .contains(name.lowercased())
    }

    public static func sectionLabel(_ section: UciSection) -> String {
        if !section.anonymous { return section.name }
        let name = section.option("name")
        return name.isEmpty ? "\(section.type) #\(section.index)" : "\(section.type) \"\(name)\""
    }

    public static func validUciName(_ name: String) -> Bool {
        !name.isEmpty && name.count <= 64 && name.unicodeScalars.allSatisfy { $0.isASCII && (CharacterSet.alphanumerics.contains($0) || $0 == "_") }
    }

    public static func pendingCount(_ changes: JSONObject) -> Int {
        changes.values.reduce(0) { $0 + $1.arrayOrEmpty.count }
    }

    public static func describeChanges(_ changes: JSONObject) -> [String] {
        var lines: [String] = []
        for (config, entries) in changes {
            for entry in entries.arrayOrEmpty {
                let change = entry.arrayOrEmpty
                let op = change.first?.text ?? ""
                let section = change.count > 1 ? change[1].text : ""
                let option = change.count > 2 ? change[2].text : ""
                let value = change.count > 3 ? change[3] : JSONValue.null
                let shown = isSecretOption(option) ? "••••••" : optionText(value)
                let path = "\(config).\(section)"
                switch op {
                case "set" where change.count > 3: lines.append("\(path).\(option) = \(shown)")
                case "set", "add": lines.append("\(path): new \(option.isEmpty ? "unnamed" : option) section")
                case "remove" where !option.isEmpty: lines.append("\(path).\(option) removed")
                case "remove": lines.append("\(path) deleted")
                case "list-add": lines.append("\(path).\(option) += \(shown)")
                case "list-del": lines.append("\(path).\(option) -= \(shown)")
                case "rename": lines.append("\(path) renamed to \(option)")
                case "order": lines.append("\(path) moved")
                default: lines.append("\(path): \(op)")
                }
            }
        }
        return lines
    }

    public static func validMac(_ text: String) -> Bool {
        let parts = text.trimmingCharacters(in: .whitespaces).split(separator: ":", omittingEmptySubsequences: false)
        return parts.count == 6 && parts.allSatisfy { $0.count == 2 && $0.allSatisfy(\.isHexDigit) }
    }

    public static func parseLeases(_ text: String) -> [OpenWrtLease] {
        text.split(separator: "\n").compactMap { line in
            let fields = line.split(separator: " ").map(String.init)
            guard fields.count >= 4, let expires = Int64(fields[0]), validMac(fields[1]) else { return nil }
            return OpenWrtLease(expires: expires, mac: fields[1].lowercased(), ip: fields[2], hostname: fields[3] == "*" ? "" : fields[3])
        }
    }

    public static func parseLuciLeases(_ data: JSONObject, now: Int64) -> [OpenWrtLease] {
        data.maps("dhcp_leases").compactMap { raw in
            let mac = raw.str("macaddr").lowercased()
            guard !mac.isEmpty else { return nil }
            let left = raw["expires"]?.int64Value ?? 0
            return OpenWrtLease(expires: left > 0 ? now + left : 0, mac: mac, ip: raw.str("ipaddr"), hostname: raw.str("hostname"))
        }
    }

    public static func leaseExpiry(_ expires: Int64, now: Int64) -> String {
        if expires <= 0 { return "Never" }
        let left = expires - now
        return left <= 0 ? "Expired" : "in \(formatUptime(left))"
    }

    public static func parseInstalledPackages(_ text: String) -> [OpenWrtPackage] {
        var packages: [OpenWrtPackage] = []
        var blockName = ""
        var blockVersion = ""
        func flush() {
            if !blockName.isEmpty { packages.append(OpenWrtPackage(name: blockName, version: blockVersion)) }
            blockName = ""
            blockVersion = ""
        }
        for raw in text.split(separator: "\n", omittingEmptySubsequences: false) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty {
                flush()
                continue
            }
            if line.hasPrefix("Package: ") {
                flush()
                blockName = String(line.dropFirst(9)).trimmingCharacters(in: .whitespaces)
                continue
            }
            if line.hasPrefix("Version: ") {
                blockVersion = String(line.dropFirst(9)).trimmingCharacters(in: .whitespaces)
                continue
            }
            if !blockName.isEmpty || line.contains(": ") { continue }
            if let dash = line.range(of: " - ") {
                let rest = line[dash.upperBound...]
                let version = rest.range(of: " - ").map { String(rest[..<$0.lowerBound]) } ?? String(rest)
                packages.append(OpenWrtPackage(name: String(line[..<dash.lowerBound]), version: version))
                continue
            }
            if let package = apkPackage(line) { packages.append(package) }
        }
        flush()
        return packages.sorted { $0.name < $1.name }
    }

    static func apkPackage(_ line: String) -> OpenWrtPackage? {
        let token = String(line.split(separator: " ").first ?? "")
        let pieces = token.split(separator: "-", omittingEmptySubsequences: false).map(String.init)
        guard pieces.count >= 2 else { return nil }
        var versionStart = pieces.count - 1
        if pieces[versionStart].hasPrefix("r"), pieces[versionStart].dropFirst().allSatisfy(\.isNumber), versionStart >= 2 {
            versionStart -= 1
        }
        guard let first = pieces[versionStart].first, first.isNumber, versionStart >= 1 else { return nil }
        return OpenWrtPackage(name: pieces[..<versionStart].joined(separator: "-"), version: pieces[versionStart...].joined(separator: "-"))
    }

    public static let packageTools: [OpenWrtPackageTool] = [
        OpenWrtPackageTool(program: "/usr/libexec/package-manager-call", wrapper: true),
        OpenWrtPackageTool(program: "/usr/libexec/opkg-call", wrapper: true),
        OpenWrtPackageTool(program: "/usr/bin/apk", wrapper: false),
        OpenWrtPackageTool(program: "/bin/opkg", wrapper: false),
    ]

    public static func packageArguments(_ tool: OpenWrtPackageTool, action: String, names: [String] = []) -> [String] {
        let apk = !tool.wrapper && tool.program.hasSuffix("/apk")
        switch action {
        case "list": return tool.wrapper || !apk ? ["list-installed"] : ["list", "--installed"]
        case "update": return ["update"]
        case "install": return [apk ? "add" : "install"] + names
        case "remove": return [apk ? "del" : "remove"] + names
        default: return []
        }
    }

    public static func validPackageName(_ name: String) -> Bool {
        guard let first = name.unicodeScalars.first, name.count <= 100 else { return false }
        let lowerDigit = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyz0123456789")
        let allowed = lowerDigit.union(CharacterSet(charactersIn: "+._-"))
        return lowerDigit.contains(first) && name.unicodeScalars.allSatisfy { allowed.contains($0) }
    }

    public static func parseServices(_ list: JSONObject) -> [OpenWrtService] {
        list.map { key, value in
            let raw = value.objectOrEmpty
            return OpenWrtService(name: key, enabled: raw.bool("enabled"), running: raw.bool("running"))
        }.sorted { $0.name < $1.name }
    }

    public static func parseInterfaces(_ dump: JSONObject) -> [OpenWrtInterfaceStatus] {
        dump.maps("interface").compactMap { raw in
            var status = OpenWrtInterfaceStatus()
            status.name = raw.str("interface")
            status.proto = raw.str("proto")
            status.device = raw.str("l3_device").isEmpty ? raw.str("device") : raw.str("l3_device")
            status.up = raw.bool("up")
            status.uptime = raw.int64("uptime")
            status.ipv4 = raw.maps("ipv4-address").map { "\($0.str("address"))/\($0.int("mask"))" }
            status.ipv6 = raw.maps("ipv6-address").map { "\($0.str("address"))/\($0.int("mask"))" }
            status.dns = raw.strings("dns-server")
            status.gateway = raw.maps("route").first { $0.str("target") == "0.0.0.0" && $0.int("mask") == 0 }?.str("nexthop") ?? ""
            return status.name.isEmpty ? nil : status
        }
    }

    public static func formatUptime(_ seconds: Int64) -> String {
        let value = max(0, seconds)
        let days = value / 86400, hours = (value % 86400) / 3600, minutes = (value % 3600) / 60
        if days > 0 { return "\(days)d \(hours)h" }
        if hours > 0 { return "\(hours)h \(minutes)m" }
        if minutes > 0 { return "\(minutes)m" }
        return "\(value)s"
    }

    public static func formatBytes(_ bytes: Int64) -> String {
        let units = ["B", "KB", "MB", "GB", "TB"]
        var value = Double(max(0, bytes))
        var unit = 0
        while value >= 1024 && unit < units.count - 1 {
            value /= 1024
            unit += 1
        }
        if unit == 0 { return "\(Int64(value)) B" }
        return String(format: value < 10 ? "%.1f %@" : "%.0f %@", value, units[unit])
    }

    public static func formatLoad(_ load: [JSONValue]) -> String {
        load.map { String(format: "%.2f", $0.double() / 65536.0) }.joined(separator: " · ")
    }

    public static func validIpv4(_ text: String) -> Bool { ipv4Bytes(text.trimmingCharacters(in: .whitespaces)) != nil }

    public static func validNetmask(_ text: String) -> Bool {
        guard let bytes = ipv4Bytes(text.trimmingCharacters(in: .whitespaces)) else { return false }
        let mask = bytes.reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
        let inverted = ~mask
        return inverted & (inverted &+ 1) == 0
    }

    public static func validAddressList(_ text: String, allowPrefix: Bool) -> Bool {
        let parts = text.split { $0 == " " || $0 == "," }.map(String.init)
        guard !parts.isEmpty else { return false }
        return parts.allSatisfy { part in
            let pieces = part.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
            guard pieces.count <= 2, validIpv4(pieces[0]) else { return false }
            if pieces.count == 2 {
                guard allowPrefix, let prefix = Int(pieces[1]), (0...32).contains(prefix) else { return false }
            }
            return true
        }
    }

    public static func validPort(_ text: String) -> Bool {
        let parts = text.trimmingCharacters(in: .whitespaces).split(whereSeparator: { $0 == "-" || $0 == ":" }).map(String.init)
        guard (1...2).contains(parts.count), parts.allSatisfy({ !$0.isEmpty && $0.count <= 5 && $0.allSatisfy(\.isNumber) }) else { return false }
        let first = Int(parts[0]) ?? 0
        let last = Int(parts.last ?? "") ?? 0
        return first >= 1 && last <= 65535 && first <= last
    }

    public static func ssidProblem(_ ssid: String) -> String? {
        let length = ssid.utf8.count
        if length == 0 { return "Give the Wi-Fi network a name." }
        if length > 32 { return "A Wi-Fi name can be at most 32 bytes long." }
        return nil
    }

    public static func wifiKeyProblem(encryption: String, key: String) -> String? {
        if encryption.isEmpty || encryption == "none" || encryption == "owe" { return nil }
        if encryption.hasPrefix("psk"), key.count == 64, key.allSatisfy(\.isHexDigit) { return nil }
        if key.count < 8 { return "The Wi-Fi password needs at least 8 characters." }
        if encryption.hasPrefix("psk") || encryption == "sae-mixed" {
            if key.count > 63 { return "A WPA2 password can be at most 63 characters." }
            if !key.unicodeScalars.allSatisfy({ (0x20...0x7e).contains($0.value) }) {
                return "A WPA2 password can only use printable ASCII characters."
            }
        }
        return nil
    }

    public static func hostnameProblem(_ hostname: String) -> String? {
        let allowed = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-")
        if hostname.isEmpty || hostname.count > 63 || hostname.hasPrefix("-") || hostname.hasSuffix("-")
            || !hostname.unicodeScalars.allSatisfy({ allowed.contains($0) }) {
            return "A hostname uses letters, digits and hyphens, at most 63 characters, and cannot start or end with a hyphen."
        }
        return nil
    }

    public static func passwordProblem(_ password: String, confirm: String) -> String? {
        if password.count < 8 { return "Use at least 8 characters for the router password." }
        if password != confirm { return "The two passwords do not match." }
        return nil
    }

    public static let encryptionChoices: [OpenWrtChoice] = [
        OpenWrtChoice(value: "sae-mixed", label: "WPA2/WPA3 (recommended)"),
        OpenWrtChoice(value: "sae", label: "WPA3 only"),
        OpenWrtChoice(value: "psk2", label: "WPA2"),
        OpenWrtChoice(value: "psk-mixed", label: "WPA/WPA2 (older devices)"),
        OpenWrtChoice(value: "owe", label: "Open, encrypted (OWE)"),
        OpenWrtChoice(value: "none", label: "Open, no password"),
    ]

    public static let protocolChoices: [OpenWrtChoice] = [
        OpenWrtChoice(value: "static", label: "Static address"),
        OpenWrtChoice(value: "dhcp", label: "DHCP client"),
        OpenWrtChoice(value: "dhcpv6", label: "DHCPv6 client"),
        OpenWrtChoice(value: "pppoe", label: "PPPoE"),
        OpenWrtChoice(value: "none", label: "Unmanaged"),
    ]

    public static let forwardProtocolChoices: [OpenWrtChoice] = [
        OpenWrtChoice(value: "tcp udp", label: "TCP and UDP"),
        OpenWrtChoice(value: "tcp", label: "TCP"),
        OpenWrtChoice(value: "udp", label: "UDP"),
    ]

    public static func choiceLabel(_ choices: [OpenWrtChoice], _ value: String) -> String {
        choices.first { $0.value == value }?.label ?? value
    }

    public static func certificateFingerprint(_ der: [UInt8]) -> String {
        Sha256.hash(der).map { String(format: "%02X", $0) }.joined(separator: ":")
    }

    public static func firmwareProblem(_ validation: JSONObject) -> String? {
        if validation.bool("valid") { return nil }
        let failed = validation.obj("tests").compactMap { key, value in value.isTrue ? nil : key }
        if failed.contains("fwtool_device_match") { return "This image is not for this router model. Flashing it could make the router unusable." }
        if !failed.isEmpty { return "The router rejected the image (\(failed.joined(separator: ", ")) failed)." }
        return "The router says this is not a valid firmware image for it."
    }

    public static func wifiBand(_ radio: UciSection) -> String {
        switch radio.option("band") {
        case "2g": return "2.4 GHz"
        case "5g": return "5 GHz"
        case "6g": return "6 GHz"
        case "60g": return "60 GHz"
        default:
            let hwmode = radio.option("hwmode")
            if hwmode == "11a" { return "5 GHz" }
            return hwmode.isEmpty ? "Radio" : "2.4 GHz"
        }
    }

    public static func sanitizedHostname(_ name: String, mac: String) -> String {
        var clean = String(name.map { $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "-") ? $0 : "-" }.prefix(63))
        while clean.hasPrefix("-") { clean.removeFirst() }
        while clean.hasSuffix("-") { clean.removeLast() }
        if clean.isEmpty { clean = "device-" + String(mac.replacingOccurrences(of: ":", with: "").suffix(6)).lowercased() }
        return clean
    }

    public static func interfaceEdit(
        _ section: UciSection, proto: String, ipaddr: String, netmask: String, gateway: String, dns: String,
        pppoeUser: String, pppoePassword: String
    ) throws -> UciEdit {
        var edit = UciEdit(values: ["proto": .string(proto)])
        let address = ipaddr.trimmingCharacters(in: .whitespaces)
        if proto == "static" {
            guard validAddressList(address, allowPrefix: true) else { throw OpenWrtProblem("Enter a valid IPv4 address, for example 192.168.1.1.") }
            let cidr = address.contains("/")
            if !cidr && !validNetmask(netmask) { throw OpenWrtProblem("Enter a valid netmask, for example 255.255.255.0.") }
            let gw = gateway.trimmingCharacters(in: .whitespaces)
            if !gw.isEmpty && !validIpv4(gw) { throw OpenWrtProblem("The gateway must be an IPv4 address.") }
            let list = section.isList("ipaddr") || address.contains(" ") || address.contains(",")
            edit.values["ipaddr"] = optionValue(address, list: list)
            if cidr { edit.removed.append("netmask") } else { edit.values["netmask"] = .string(netmask.trimmingCharacters(in: .whitespaces)) }
            if gw.isEmpty { edit.removed.append("gateway") } else { edit.values["gateway"] = .string(gw) }
        } else {
            edit.removed += ["ipaddr", "netmask", "gateway"]
        }
        if proto == "pppoe" {
            let user = pppoeUser.trimmingCharacters(in: .whitespaces)
            if user.isEmpty { throw OpenWrtProblem("PPPoE needs the user name from your internet provider.") }
            edit.values["username"] = .string(user)
            if !pppoePassword.isEmpty { edit.values["password"] = .string(pppoePassword) }
        } else {
            edit.removed += ["username", "password"]
        }
        let servers = dns.trimmingCharacters(in: .whitespaces)
        if servers.isEmpty || proto == "none" {
            edit.removed.append("dns")
        } else {
            let parts = servers.split { $0 == " " || $0 == "," }.map(String.init)
            guard parts.allSatisfy({ validIpv4($0) || $0.contains(":") }) else { throw OpenWrtProblem("DNS servers must be IP addresses separated by spaces.") }
            edit.values["dns"] = .array(parts.map(JSONValue.string))
        }
        edit.removed = edit.removed.filter { section.options.contains($0) }
        return edit
    }

    public static func dhcpEdit(enabled: Bool, start: Int, limit: Int, leaseTime: String) -> UciEdit {
        if !enabled { return UciEdit(values: ["ignore": "1"]) }
        let lease = leaseTime.trimmingCharacters(in: .whitespaces)
        return UciEdit(
            values: ["start": .string(String(start)), "limit": .string(String(limit)), "leasetime": .string(lease.isEmpty ? "12h" : lease)],
            removed: ["ignore"])
    }

    public static func radioEdit(_ radio: UciSection, enabled: Bool, channel: String, country: String, txpower: String) throws -> UciEdit {
        let channel = channel.trimmingCharacters(in: .whitespaces).lowercased()
        if !channel.isEmpty && channel != "auto" {
            guard let number = Int(channel), (1...233).contains(number) else { throw OpenWrtProblem("The channel is \"auto\" or a channel number.") }
        }
        let country = country.trimmingCharacters(in: .whitespaces).uppercased()
        if !country.isEmpty && (country.count != 2 || !country.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber) })) {
            throw OpenWrtProblem("The country is a two-letter code such as US or DE.")
        }
        let power = txpower.trimmingCharacters(in: .whitespaces)
        if !power.isEmpty {
            guard let dbm = Int(power), (1...36).contains(dbm) else {
                throw OpenWrtProblem("Transmit power is a number of dBm between 1 and 36, or empty for the driver default.")
            }
        }
        var edit = UciEdit(values: ["channel": .string(channel.isEmpty ? "auto" : channel)])
        if enabled { edit.removed.append("disabled") } else { edit.values["disabled"] = "1" }
        if country.isEmpty { edit.removed.append("country") } else { edit.values["country"] = .string(country) }
        if power.isEmpty { edit.removed.append("txpower") } else { edit.values["txpower"] = .string(power) }
        edit.removed = edit.removed.filter { radio.options.contains($0) }
        return edit
    }

    public static func wifiEdit(
        _ iface: UciSection, ssid: String, encryption: String, key: String, network: String, enabled: Bool, hidden: Bool, isolate: Bool
    ) throws -> UciEdit {
        if let problem = ssidProblem(ssid) ?? wifiKeyProblem(encryption: encryption, key: key) { throw OpenWrtProblem(problem) }
        var edit = UciEdit(values: ["ssid": .string(ssid), "encryption": .string(encryption)])
        if encryption == "none" || encryption == "owe" { edit.removed.append("key") } else { edit.values["key"] = .string(key) }
        if !network.isEmpty { edit.values["network"] = .string(network) }
        if enabled { edit.removed.append("disabled") } else { edit.values["disabled"] = "1" }
        if hidden { edit.values["hidden"] = "1" } else { edit.removed.append("hidden") }
        if isolate { edit.values["isolate"] = "1" } else { edit.removed.append("isolate") }
        edit.removed = edit.removed.filter { iface.options.contains($0) }
        return edit
    }

    public static func newWifiValues(radio: String, network: String, ssid: String, key: String) throws -> JSONObject {
        if let problem = ssidProblem(ssid) ?? wifiKeyProblem(encryption: "sae-mixed", key: key) { throw OpenWrtProblem(problem) }
        return ["device": .string(radio), "mode": "ap", "network": .string(network), "ssid": .string(ssid), "encryption": "sae-mixed", "key": .string(key)]
    }

    public static func forwardEdit(
        _ existing: UciSection?, name: String, proto: String, externalPort: String, ip: String, internalPort: String, enabled: Bool
    ) throws -> UciEdit {
        let name = name.trimmingCharacters(in: .whitespaces)
        let external = externalPort.trimmingCharacters(in: .whitespaces)
        let ip = ip.trimmingCharacters(in: .whitespaces)
        let target = internalPort.trimmingCharacters(in: .whitespaces)
        if name.isEmpty { throw OpenWrtProblem("Give the port forward a name.") }
        if !validPort(external) { throw OpenWrtProblem("The external port is a number from 1 to 65535, or a range such as 6000-6010.") }
        if !validIpv4(ip) { throw OpenWrtProblem("Enter the IPv4 address of the device on your network.") }
        if !target.isEmpty && !validPort(target) {
            throw OpenWrtProblem("The port on the device is a number from 1 to 65535, or empty to use the external port.")
        }
        var edit = UciEdit(values: ["name": .string(name), "proto": optionValue(proto, list: true), "src_dport": .string(external), "dest_ip": .string(ip)])
        if target.isEmpty { edit.removed.append("dest_port") } else { edit.values["dest_port"] = .string(target) }
        if enabled { edit.removed.append("enabled") } else { edit.values["enabled"] = "0" }
        if let existing {
            edit.removed = edit.removed.filter { existing.options.contains($0) }
        } else {
            edit.values["target"] = "DNAT"
            edit.values["src"] = "wan"
            edit.values["dest"] = "lan"
            edit.removed = []
        }
        return edit
    }

    public static func reservationValues(name: String, mac: String, ip: String) throws -> JSONObject {
        let mac = mac.trimmingCharacters(in: .whitespaces).lowercased()
        let ip = ip.trimmingCharacters(in: .whitespaces)
        let name = name.trimmingCharacters(in: .whitespaces)
        if !validMac(mac) { throw OpenWrtProblem("Enter the device's MAC address, for example aa:bb:cc:dd:ee:ff.") }
        if !validIpv4(ip) { throw OpenWrtProblem("Enter the IPv4 address to reserve.") }
        if !name.isEmpty, let problem = hostnameProblem(name) { throw OpenWrtProblem(problem) }
        return ["name": .string(sanitizedHostname(name, mac: mac)), "mac": .string(mac), "ip": .string(ip), "dns": "1"]
    }

    public static func isStorageDevice(_ name: String) -> Bool {
        func digits(_ text: Substring) -> Bool { !text.isEmpty && text.allSatisfy(\.isNumber) }
        for prefix in ["sd", "hd", "vd"] where name.hasPrefix(prefix) {
            let rest = name.dropFirst(2)
            let letters = rest.prefix { $0.isLowercase && $0.isLetter }
            let tail = rest.dropFirst(letters.count)
            return !letters.isEmpty && (tail.isEmpty || digits(tail))
        }
        if name.hasPrefix("mmcblk") {
            let rest = name.dropFirst(6)
            let parts = rest.split(separator: "p", omittingEmptySubsequences: false)
            return (parts.count == 1 && digits(parts[0])) || (parts.count == 2 && digits(parts[0]) && digits(parts[1]))
        }
        if name.hasPrefix("nvme") {
            let rest = String(name.dropFirst(4))
            let pieces = rest.split(separator: "n", omittingEmptySubsequences: false)
            guard pieces.count == 2, digits(pieces[0]) else { return false }
            let namespace = pieces[1].split(separator: "p", omittingEmptySubsequences: false)
            return (namespace.count == 1 && digits(namespace[0])) || (namespace.count == 2 && digits(namespace[0]) && digits(namespace[1]))
        }
        return false
    }

    public static func parseBlockDevices(_ devices: JSONObject) -> [OpenWrtBlockDevice] {
        devices.compactMap { key, value -> OpenWrtBlockDevice? in
            let raw = value.objectOrEmpty
            var block = OpenWrtBlockDevice()
            block.device = raw.str("dev", "/dev/\(key)")
            block.name = String(block.device.split(separator: "/").last ?? "")
            guard isStorageDevice(block.name) else { return nil }
            block.uuid = raw.str("uuid")
            block.label = raw.str("label")
            block.type = raw.str("type")
            block.mount = raw.str("mount")
            block.size = raw.int64("size")
            return block
        }.sorted { $0.name < $1.name }
    }

    public static func parseMountPoints(_ data: JSONObject) -> [OpenWrtMountPoint] {
        data.maps("result").compactMap { raw in
            let mount = raw.str("mount")
            guard !mount.isEmpty else { return nil }
            return OpenWrtMountPoint(device: raw.str("device"), mount: mount, type: raw.str("type"), size: raw.int64("size"), avail: raw.int64("avail"))
        }
    }

    public static func storageDisks(_ blocks: [OpenWrtBlockDevice], mounts: [OpenWrtMountPoint], fstab: [UciSection]) -> [OpenWrtDisk] {
        blocks.map { block in
            var disk = OpenWrtDisk(block: block, mount: block.mount)
            if let mount = mounts.first(where: { $0.device == block.device || (!disk.mount.isEmpty && $0.mount == disk.mount) }) {
                disk.mount = mount.mount
                disk.size = mount.size
                disk.avail = mount.avail
                disk.usageKnown = mount.size > 0
            }
            if !disk.usageKnown { disk.size = block.size }
            if let section = fstab.first(where: { section in
                guard section.type == "mount" else { return false }
                let uuid = section.option("uuid"), device = section.option("device"), label = section.option("label")
                return (!uuid.isEmpty && uuid.lowercased() == block.uuid.lowercased()) || (!device.isEmpty && device == block.device)
                    || (!label.isEmpty && label == block.label)
            }) {
                disk.fstabSection = section.name
                disk.bootTarget = section.option("target")
                disk.bootEnabled = section.flag("enabled", true)
            }
            return disk
        }
    }

    public static func mountPathProblem(_ path: String) -> String? {
        let problem = "Use a folder under /mnt, /media, /srv or /data, for example /mnt/usb, with letters, digits, dots, dashes and underscores."
        let parts = path.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        guard parts.count >= 3, parts[0].isEmpty, ["mnt", "media", "srv", "data"].contains(parts[1]) else { return problem }
        let allowed = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")
        for part in parts.dropFirst(2) {
            if part.isEmpty || part == "." || part == ".." || !part.unicodeScalars.allSatisfy({ allowed.contains($0) }) { return problem }
        }
        return nil
    }

    public static func suggestedMountPath(_ block: OpenWrtBlockDevice) -> String {
        var name = String(block.label.map { $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "." || $0 == "-" || $0 == "_") ? $0 : "_" })
        while name.hasPrefix(".") { name.removeFirst() }
        return "/mnt/" + (name.isEmpty ? block.name : name)
    }

    public static let usbStoragePackages = ["kmod-usb-storage", "kmod-usb-storage-uas", "block-mount", "kmod-fs-ext4", "kmod-fs-vfat", "kmod-fs-exfat"]
    public static let fstabPath = "/etc/config/fstab"
    public static let defaultFstab = "config global\n\toption anon_swap '0'\n\toption anon_mount '0'\n\toption auto_swap '1'\n\toption auto_mount '1'\n\toption delay_root '5'\n\toption check_fs '0'\n"

    public static func parsePartitions(_ text: String) -> [String] {
        var names: [String] = []
        for line in text.split(separator: "\n") {
            let fields = line.split(whereSeparator: { $0 == " " || $0 == "\t" }).map(String.init)
            if fields.count == 4, isStorageDevice(fields[3]), !names.contains(fields[3]) { names.append(fields[3]) }
        }
        return names
    }

    public static func usbControllerCount(_ entries: [String]) -> Int {
        entries.filter { $0.range(of: #"^usb\d+$"#, options: .regularExpression) != nil }.count
    }

    public static func usbDeviceEntries(_ entries: [String]) -> [String] {
        entries.filter { $0.range(of: #"^\d+-\d+(\.\d+)*$"#, options: .regularExpression) != nil }.sorted()
    }

    public static func storageKernelLines(_ dmesg: String, limit: Int = 20) -> [String] {
        let wanted = #"usb|uas|scsi|xhci|ehci|ohci|\bsd[a-z]+\d*\b|exfat|ext4|vfat|fat-fs|mmc\d|i/o error"#
        let lines = dmesg.split(separator: "\n").map { line in
            String(line).replacingOccurrences(of: #"^<\d+>"#, with: "", options: .regularExpression).trimmingCharacters(in: .whitespaces)
        }.filter { !$0.isEmpty && $0.range(of: wanted, options: [.regularExpression, .caseInsensitive]) != nil }
        return Array(lines.suffix(limit))
    }

    public static func missingStoragePackages(_ installed: [String]) -> [String] {
        usbStoragePackages.filter { !installed.contains($0) }
    }

    public static func fstabFromDetect(_ output: String) -> String {
        let hasGlobal = output.split(separator: "\n").contains { line in
            String(line).range(of: #"^config\s+'?global'?\s*$"#, options: .regularExpression) != nil
        }
        guard hasGlobal else { return defaultFstab }
        return output.hasSuffix("\n") ? output : output + "\n"
    }

    public static func outputTail(_ text: String, count: Int = 4) -> [String] {
        Array(text.split(separator: "\n").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }.suffix(count))
    }

    public static func storageFindings(_ check: OpenWrtStorageCheck) -> [String] {
        var findings: [String] = []
        let devices = check.usbDevices.joined(separator: ", ")
        let partitions = check.partitions.joined(separator: ", ")
        let missing = missingStoragePackages(check.installed)
        if !check.listProblem.isEmpty { findings.append("The router could not list its disks: \(check.listProblem)") }
        if check.usbKnown && check.usbControllers == 0 && check.partitions.isEmpty {
            findings.append("The router has no USB controller driver running, so it cannot see anything in its USB port. Install the driver for this router (usually kmod-usb2 or kmod-usb3), or check that the model has a USB port.")
        } else if check.usbKnown && check.usbDevices.isEmpty && check.partitions.isEmpty {
            findings.append("The router sees nothing plugged into its USB port. Push the drive in fully, try another port or cable, and give a hard disk that needs more power a powered USB hub.")
        } else if check.partitionsKnown && check.partitions.isEmpty && !check.usbDevices.isEmpty {
            if check.packagesKnown && !check.installed.contains("kmod-usb-storage") {
                findings.append("A USB device is plugged in (\(devices)), but the USB storage driver (kmod-usb-storage) is not installed, so no disk appears. Press Install USB storage support.")
            } else {
                findings.append("A USB device is plugged in (\(devices)), but no disk appeared for it. Unplug it and plug it back in, or restart the router; the kernel messages below may say why.")
            }
        }
        if !check.partitions.isEmpty && !check.blockTool {
            findings.append("The router sees the disk (\(partitions)), but block-mount is not installed, so it cannot list or mount it. Press Install USB storage support.")
        } else if !check.partitions.isEmpty && check.disks == 0 && check.listProblem.isEmpty {
            let drivers = missing.filter { $0.hasPrefix("kmod-fs-") }
            var text = "The router sees the disk (\(partitions)), but found no filesystem it can read on it. Format it on a computer as ext4, exFAT or FAT32."
            if check.packagesKnown && !drivers.isEmpty { text += " These filesystem drivers are not installed: \(drivers.joined(separator: ", "))." }
            findings.append(text)
        }
        if check.blockTool && !check.fstabExists {
            findings.append("The router has no storage settings file (/etc/config/fstab) yet. Turning on Mount any disk when it is plugged in creates it.")
        }
        if !check.packagesKnown {
            findings.append("ForkMesh cannot run the router's package manager, so it cannot tell which drivers are installed.")
        } else if !missing.isEmpty {
            findings.append("Not installed: \(missing.joined(separator: ", ")).")
        }
        let unmounted = check.disks - check.mounted
        if unmounted > 0 {
            let count = unmounted == 1 ? "1 disk is not mounted." : "\(unmounted) disks are not mounted."
            if !check.autoMount {
                findings.append(count + " Turn on Mount any disk when it is plugged in and apply, or choose Mount at boot for the disk; then press Mount disks now.")
            } else if check.autoMountPending {
                findings.append(count + " Apply your changes, then press Mount disks now.")
            } else {
                findings.append(count + " Press Mount disks now; if it stays unmounted, its filesystem driver may be missing.")
            }
        } else if check.disks > 0 {
            findings.append(check.disks == 1 ? "The disk is mounted." : "All \(check.disks) disks are mounted.")
        }
        if findings.isEmpty { findings.append("No problem found.") }
        return findings
    }

    public static func storageReport(_ check: OpenWrtStorageCheck) -> String {
        var lines = storageFindings(check).map { "• " + $0 }
        lines.append("")
        if check.usbKnown {
            lines.append("USB controllers: \(check.usbControllers)")
            lines.append("USB devices: \(check.usbDevices.isEmpty ? "none" : check.usbDevices.joined(separator: ", "))")
        } else {
            lines.append("USB devices: the router did not list them")
        }
        lines.append("Disks the kernel sees: \(!check.partitionsKnown ? "could not read /proc/partitions" : check.partitions.isEmpty ? "none" : check.partitions.joined(separator: ", "))")
        lines.append("Disks with a readable filesystem: \(check.disks), mounted: \(check.mounted)")
        lines.append("block-mount: \(check.blockTool ? "installed" : "not installed")")
        lines.append("Storage settings (/etc/config/fstab): \(check.fstabExists ? "present" : "missing")")
        lines.append("Mount any disk when it is plugged in: \(check.autoMount ? "on" : "off")\(check.autoMountPending ? ", waiting to be applied" : "")")
        let missing = missingStoragePackages(check.installed)
        lines.append(!check.packagesKnown ? "Storage packages: unknown"
                     : missing.isEmpty ? "Storage packages: all installed" : "Storage packages missing: \(missing.joined(separator: ", "))")
        if check.kernelLines.isEmpty {
            lines.append("Recent kernel messages: none about USB or disks")
        } else {
            lines.append("Recent kernel messages:")
            lines.append(contentsOf: check.kernelLines.map { "  " + $0 })
        }
        return lines.joined(separator: "\n")
    }

    public static func shareConfig(_ configs: [String]) -> String {
        if configs.contains("ksmbd") { return "ksmbd" }
        if configs.contains("samba4") { return "samba4" }
        return ""
    }

    public static func shareSectionType(_ config: String) -> String {
        switch config {
        case "ksmbd": return "share"
        case "samba4": return "sambashare"
        default: return ""
        }
    }

    public static func parseShares(_ sections: [UciSection]) -> [OpenWrtShare] {
        sections.filter { $0.type == "share" || $0.type == "sambashare" }.map { section in
            func yes(_ key: String) -> Bool { ["yes", "1", "true"].contains(section.option(key).lowercased()) }
            return OpenWrtShare(section: section.name, name: section.option("name", section.name), path: section.option("path"),
                                readOnly: yes("read_only"), guests: yes("guest_ok"))
        }
    }

    public static func shareNameProblem(_ name: String) -> String? {
        let allowed = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")
        guard let first = name.first, first.isASCII, first.isLetter || first.isNumber, name.count <= 32,
              name.unicodeScalars.allSatisfy({ allowed.contains($0) }) else {
            return "A share name uses up to 32 letters, digits, dots, dashes and underscores."
        }
        return nil
    }

    public static func shareValues(name: String, path: String, guests: Bool) -> JSONObject {
        ["name": .string(name), "path": .string(path), "read_only": "no", "guest_ok": .string(guests ? "yes" : "no"),
         "create_mask": "0666", "dir_mask": "0777"]
    }

    public static func mountSectionValues(_ block: OpenWrtBlockDevice, target: String) -> JSONObject {
        var values: JSONObject = ["target": .string(target), "enabled": "1"]
        if block.uuid.isEmpty { values["device"] = .string(block.device) } else { values["uuid"] = .string(block.uuid) }
        return values
    }
}
