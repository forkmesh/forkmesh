import Foundation
import FMBase

/// One ForkMesh Ads creative, as returned by `GET /api/ads/serve`.
///
/// ForkMesh Ads is first-party, human-reviewed and text-only. Every field here
/// is plain text and must only ever reach a plain `Text` view: nothing in an
/// ad is markup, and the only thing an ad can do is open `url` in the external
/// browser. `parse` re-validates what the server already enforces, so a
/// malformed or hostile payload collapses the slot instead of rendering.
public struct SponsoredAd: Equatable, Hashable, Sendable, Identifiable {
    public static let maxHeadline = 60
    public static let maxBody = 160
    public static let maxCta = 24
    public static let maxSponsor = 48
    public static let maxCategoryLabel = 64
    public static let maxUrlLength = 2048
    public static let defaultCta = "Learn more"

    public var id: String
    public var headline: String
    public var body: String
    public var cta: String
    public var sponsor: String
    public var url: String
    public var categoryLabel: String

    public init(
        id: String, headline: String, url: String, body: String = "", cta: String = SponsoredAd.defaultCta,
        sponsor: String = "", categoryLabel: String = ""
    ) {
        self.id = id
        self.headline = headline
        self.url = url
        self.body = body
        self.cta = cta
        self.sponsor = sponsor
        self.categoryLabel = categoryLabel
    }

    /// The validated destination. Always `https` with a host.
    public var uri: URL? { URL(string: url) }

    private static let idPattern = try! NSRegularExpression(pattern: "^[0-9a-f]{32}$")

    /// Nil unless `json` is a well-formed ad: a 32-hex id, a non-empty
    /// headline and an `https` destination with a host and no embedded
    /// credentials. Text fields are whitespace-collapsed and clamped to the
    /// network's published limits.
    public static func parse(_ json: JSONValue?) -> SponsoredAd? {
        guard let object = json?.objectValue else { return nil }
        let id = string(object["id"])
        guard DartRegex.matches(idPattern, id) else { return nil }
        let headline = clamp(object["headline"], maxHeadline)
        if headline.isEmpty { return nil }
        let url = string(object["url"])
        guard isSafeUrl(url) else { return nil }
        let cta = clamp(object["cta"], maxCta)
        return SponsoredAd(
            id: id,
            headline: headline,
            url: url,
            body: clamp(object["body"], maxBody),
            cta: cta.isEmpty ? defaultCta : cta,
            sponsor: clamp(object["sponsor"], maxSponsor),
            categoryLabel: clamp(object["categoryLabel"], maxCategoryLabel)
        )
    }

    /// Whether `value` is a destination an ad is allowed to open.
    public static func isSafeUrl(_ value: String) -> Bool {
        if value.isEmpty || DartCore.length(value) > maxUrlLength { return false }
        if value.unicodeScalars.contains(where: DartCore.isWhitespace) || value.contains("\\") { return false }
        // The network stores destinations normalised to a lowercase scheme.
        if !value.hasPrefix("https://") { return false }
        guard let components = URLComponents(string: value) else { return false }
        if components.scheme != "https" || (components.host ?? "").isEmpty { return false }
        // `https://forkmesh.com@evil.example/` reads as one host and opens another.
        if components.user != nil || components.password != nil { return false }
        return true
    }

    private static func string(_ value: JSONValue?) -> String { value?.stringValue?.dartTrimmed ?? "" }

    /// Whitespace runs collapsed to one space, then at most `max` code points
    /// (never splitting a surrogate pair), right-trimmed when cut.
    private static func clamp(_ value: JSONValue?, _ max: Int) -> String {
        var clean = String.UnicodeScalarView()
        var inRun = false
        for scalar in string(value).unicodeScalars {
            if DartCore.isWhitespace(scalar) {
                if !inRun { clean.append(" ") }
                inRun = true
            } else {
                clean.append(scalar)
                inRun = false
            }
        }
        let text = String(clean)
        if DartCore.length(text) <= max || text.unicodeScalars.count <= max { return text }
        var cut = String(String.UnicodeScalarView(text.unicodeScalars.prefix(max)))
        while let last = cut.unicodeScalars.last, DartCore.isWhitespace(last) {
            cut.unicodeScalars.removeLast()
        }
        return cut
    }
}
