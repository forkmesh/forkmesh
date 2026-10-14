import FMBase
import Foundation

/// The UDID of this iPhone, read through the web profile service.
///
/// iOS gives apps no API for the UDID. The app instead opens `/api/ios-udid?n=<nonce>` in Safari,
/// where a one-time configuration profile makes iOS send the UDID to that page; the page's "Open
/// in ForkMesh" button hands it back as `forkmesh://ios-udid?udid=…&n=<nonce>`. The nonce is what
/// makes the app accept only a result from a read it started itself.
public struct IosUdidLink: Equatable, Sendable {
    public static let scheme = "forkmesh"
    public static let host = "ios-udid"
    public static let pagePath = "/api/ios-udid"

    public let udid: String
    public let nonce: String
    /// Apple's model identifier, e.g. `iPhone15,2`. Empty when not sent.
    public let product: String
    /// The iOS build, e.g. `22A3354`. Empty when not sent.
    public let version: String

    public init(udid: String, nonce: String, product: String = "", version: String = "") {
        self.udid = udid
        self.nonce = nonce
        self.product = product
        self.version = version
    }

    /// The link the web page hands back, or nil for any other URL.
    public static func tryParse(_ url: URL) -> IosUdidLink? {
        guard let components = URLComponents(url: url, resolvingAgainstBaseURL: false),
              components.scheme == scheme, components.host == host else { return nil }
        // The last value wins for a repeated name.
        var q: [String: String] = [:]
        for item in components.queryItems ?? [] { q[item.name] = item.value ?? "" }
        let udid = (q["udid"] ?? "").dartTrimmed
        let nonce = (q["n"] ?? "").dartTrimmed
        guard isUdid(udid), isNonce(nonce) else { return nil }
        let product = (q["product"] ?? "").dartTrimmed
        let version = (q["version"] ?? "").dartTrimmed
        return IosUdidLink(
            udid: udid.uppercased(),
            nonce: nonce,
            product: isProduct(product) ? product : "",
            version: isVersion(version) ? version : ""
        )
    }

    /// `tryParse` of a URL string.
    public static func tryParse(_ text: String) -> IosUdidLink? {
        URL(string: text).flatMap { tryParse($0) }
    }

    /// A fresh nonce for one read: 24 random bytes, base64url without padding.
    public static func newNonce(_ bytes: [UInt8]? = nil) -> String {
        B64.encodeUrlNoPadding(bytes ?? Array(Bytes.random(24)))
    }

    /// The web page for `serverUrl`'s origin (the relay's `ws(s)://` URL).
    public static func pageUri(_ serverUrl: String, _ nonce: String) -> String {
        let ws = ServerURL.parse(serverUrl)
        let query = [("n", nonce)]
        if ws.host.isEmpty { return "https://app.forkmesh.com\(pagePath)?\(DartURI.queryString(query))" }
        return ws.http(pagePath, query: query)
    }

    // Current devices: 8 hex, dash, 16 hex. Devices before iPhone XS: 40 hex.
    static func isUdid(_ value: String) -> Bool {
        let b = Array(value.utf8)
        if b.count == 25 {
            return b[8] == 0x2d && b[0..<8].allSatisfy(isHex) && b[9...].allSatisfy(isHex)
        }
        return b.count == 40 && b.allSatisfy(isHex)
    }

    static func isProduct(_ value: String) -> Bool {
        let b = Array(value.utf8)
        return (1...32).contains(b.count) && b.allSatisfy { isAlnum($0) || $0 == 0x2c }
    }

    static func isVersion(_ value: String) -> Bool {
        let b = Array(value.utf8)
        return (1...16).contains(b.count) && b.allSatisfy { isAlnum($0) || $0 == 0x2e }
    }

    static func isNonce(_ value: String) -> Bool {
        let b = Array(value.utf8)
        return (16...64).contains(b.count) && b.allSatisfy { isAlnum($0) || $0 == 0x2d || $0 == 0x5f }
    }

    private static func isHex(_ c: UInt8) -> Bool {
        (c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x46) || (c >= 0x61 && c <= 0x66)
    }

    private static func isAlnum(_ c: UInt8) -> Bool {
        (c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x5a) || (c >= 0x61 && c <= 0x7a)
    }
}

/// Public documents hosted on a project's homepage.
/// Opening them is the view's `openURL`.
///
/// Every project (ForkMesh, ForkMesh Next, a self-hosted instance) publishes its own terms,
/// privacy policy, so `terms` and `privacy` follow the active project's
/// homepage (`activeHomepage`, set by the project runtime at launch and on every switch) instead
/// of always naming production's.
public enum LegalLinks {
    public static func termsFor(_ homepage: URL) -> URL { page(homepage, path: "/terms") }
    public static func privacyFor(_ homepage: URL) -> URL { page(homepage, path: "/privacy") }
    public static func appleHealthFor(_ homepage: URL) -> URL { page(homepage, path: "/privacy", fragment: "apple-health") }
    public static func designFor(_ homepage: URL) -> URL { page(homepage, path: "/design") }
    public static func desktopFor(_ homepage: URL) -> URL { page(homepage, path: "/desktop") }

    public static let productionHomepage = URL(string: "https://forkmesh.com/")!

    private final class Box: @unchecked Sendable {
        let lock = NSLock()
        var homepage: URL = LegalLinks.productionHomepage
    }

    private static let box = Box()

    public static var activeHomepage: URL {
        get {
            box.lock.lock()
            defer { box.lock.unlock() }
            return box.homepage
        }
        set {
            box.lock.lock()
            box.homepage = newValue
            box.lock.unlock()
        }
    }

    public static var terms: URL { termsFor(activeHomepage) }
    public static var privacy: URL { privacyFor(activeHomepage) }
    public static var appleHealth: URL { appleHealthFor(activeHomepage) }
    public static var design: URL { designFor(activeHomepage) }
    public static var desktop: URL { desktopFor(activeHomepage) }

    private static func page(_ homepage: URL, path: String, fragment: String? = nil) -> URL {
        guard var components = URLComponents(url: homepage, resolvingAgainstBaseURL: false) else { return homepage }
        components.path = path
        components.query = nil
        components.fragment = fragment
        return components.url ?? homepage
    }
}
