import Foundation

public struct VaultOTPAuth: Equatable, Sendable {
    public enum Algorithm: String, Sendable {
        case sha1 = "SHA1"
        case sha256 = "SHA256"
        case sha512 = "SHA512"
    }

    public var secret: [UInt8]
    public var period: Int
    public var digits: Int
    public var algorithm: Algorithm
    public var issuer: String
    public var account: String

    public init(secret: [UInt8], period: Int = 30, digits: Int = 6, algorithm: Algorithm = .sha1, issuer: String = "", account: String = "") {
        self.secret = secret
        self.period = period
        self.digits = digits
        self.algorithm = algorithm
        self.issuer = issuer
        self.account = account
    }

    public init?(uri: String) {
        guard let components = URLComponents(string: uri.trimmingCharacters(in: .whitespacesAndNewlines)),
              components.scheme?.lowercased() == "otpauth", components.host?.lowercased() == "totp"
        else { return nil }
        var query: [String: String] = [:]
        for item in components.queryItems ?? [] { query[item.name.lowercased()] = item.value ?? "" }
        guard let secret = VaultBase32.decode(query["secret"] ?? ""), !secret.isEmpty else { return nil }
        let label = String(components.path.drop(while: { $0 == "/" }))
        var issuer = query["issuer"] ?? ""
        var account = label
        if let colon = label.firstIndex(of: ":") {
            if issuer.isEmpty { issuer = String(label[..<colon]) }
            account = String(label[label.index(after: colon)...]).trimmingCharacters(in: .whitespaces)
        }
        self.init(
            secret: secret,
            period: Int(query["period"] ?? "").flatMap { $0 > 0 ? $0 : nil } ?? 30,
            digits: Int(query["digits"] ?? "").flatMap { (6...10).contains($0) ? $0 : nil } ?? 6,
            algorithm: Algorithm(rawValue: (query["algorithm"] ?? "SHA1").uppercased()) ?? .sha1,
            issuer: issuer, account: account)
    }

    public var uri: String {
        var components = URLComponents()
        components.scheme = "otpauth"
        components.host = "totp"
        let label = issuer.isEmpty ? account : "\(issuer):\(account)"
        components.path = "/" + label
        var items = [URLQueryItem(name: "secret", value: VaultBase32.encode(secret))]
        if !issuer.isEmpty { items.append(URLQueryItem(name: "issuer", value: issuer)) }
        items.append(URLQueryItem(name: "algorithm", value: algorithm.rawValue))
        items.append(URLQueryItem(name: "digits", value: String(digits)))
        items.append(URLQueryItem(name: "period", value: String(period)))
        components.queryItems = items
        return components.string ?? ""
    }
}

public enum VaultBase32 {
    static let alphabet = Array("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567".utf8)

    public static func encode(_ bytes: [UInt8]) -> String {
        var out: [UInt8] = []
        var buffer = 0
        var bits = 0
        for byte in bytes {
            buffer = (buffer << 8) | Int(byte)
            bits += 8
            while bits >= 5 {
                out.append(alphabet[(buffer >> (bits - 5)) & 31])
                bits -= 5
            }
        }
        if bits > 0 { out.append(alphabet[(buffer << (5 - bits)) & 31]) }
        return String(decoding: out, as: UTF8.self)
    }

    public static func decode(_ text: String) -> [UInt8]? {
        var out: [UInt8] = []
        var buffer = 0
        var bits = 0
        for c in text.uppercased().utf8 where c != 0x3d && c != 0x20 && c != 0x2d {
            guard let value = alphabet.firstIndex(of: c) else { return nil }
            buffer = ((buffer << 5) | value) & 0xFFFF
            bits += 5
            if bits >= 8 {
                out.append(UInt8((buffer >> (bits - 8)) & 0xFF))
                bits -= 8
            }
        }
        return out
    }
}
