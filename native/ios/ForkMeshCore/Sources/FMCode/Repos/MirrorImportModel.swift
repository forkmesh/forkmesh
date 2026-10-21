// The mirror import logic: import
// external repository metadata from GitHub, GitLab, Codeberg or Gitea
// (`POST /api/repository-imports`, mode "import"). The entry is never routed,
// cloned or described as mirrored until a ForkMesh mirror is linked to it.
// A provider token is used for that one request and never stored.

import FMAccount
import FMApi
import FMBase
import Foundation
import Observation

/// `_MirrorImportSheetState`.
@MainActor
@Observable
public final class MirrorImportModel {
    /// Canonical provider hosts the Worker accepts.
    public static let providers = ["github.com", "gitlab.com", "codeberg.org", "gitea.com"]

    public static let title = "Mirror from GitHub/GitLab/Codeberg/Gitea"
    public static let metadataNote =
        "Imports metadata only — issues, branches, releases and the like. ForkMesh does not own or control the source repository, and the entry is not a mirror until one is published and verified."
    public static let tokenHelper = "Used for this request only; never stored."

    public var url = ""
    public var token = ""
    /// The URL field's error, or nil.
    public private(set) var problem: String?
    /// The busy line or the refusal, "" when there is nothing to say.
    public private(set) var status = ""
    public private(set) var busy = false

    @ObservationIgnored private let client: RepoClient
    @ObservationIgnored private let session: @MainActor () -> AuthSession?

    public init(client: RepoClient, session: @escaping @MainActor () -> AuthSession?) {
        self.client = client
        self.session = session
    }

    /// Editing the URL clears its error.
    public func urlChanged() {
        if problem != nil { problem = nil }
    }

    /// `_submit`: the notice to close the sheet with, or nil while it stays
    /// open (a problem or a refusal is on screen).
    public func submit() async -> String? {
        if let problem = MirrorImportModel.validateSourceUrl(url) {
            self.problem = problem
            return nil
        }
        guard let session = session(), !session.nodeName.isEmpty else {
            status = "Sign in to import a repository."
            return nil
        }
        busy = true
        status = "Reading repository metadata…"
        defer { busy = false }
        do {
            let answer = try await client.createRepositoryImport(
                sourceUrl: url.dartTrimmed, targetOwner: session.nodeName, mode: "import",
                providerToken: token.dartTrimmed)
            if answer.ok {
                let created = answer.body["created"] == .bool(true)
                let notice = (answer.body["notice"].flatMap { $0.isNull ? nil : $0 })?.text ?? ""
                if !notice.isEmpty { return notice }
                return created
                    ? "Imported the repository metadata. It is listed as an external entry until a ForkMesh mirror is published."
                    : "Refreshed the existing external listing."
            }
            status = MirrorImportModel.explain(answer)
        } catch {
            status = "Import failed: \(dartErrorText(error))"
        }
        return nil
    }

    /// `_explain`: a Worker refusal in words.
    public static func explain(_ answer: ApiJsonResult) -> String {
        switch answer.error {
        case "invalid_session":
            return "Your session expired. Sign in again."
        case "target_owner_forbidden":
            return "You cannot import into that owner. Pick an account you administer."
        case "source_url_required", "https_provider_url_required", "invalid_provider_url", "invalid_repository_path":
            return "That does not look like a canonical repository URL."
        case "github_owner_and_repository_required":
            return "Use the full https://github.com/owner/repository URL."
        case "unsupported_provider":
            return "Only GitHub, GitLab, Codeberg, and Gitea repositories can be imported."
        case "provider_request_failed":
            return "The provider did not answer. Try again in a moment."
        case "rate_limited":
            return "The provider is rate-limiting; try again later."
        case "":
            return "Import failed (HTTP \(answer.statusCode))."
        default:
            return "Import failed (\(answer.error))."
        }
    }

    /// `validateSourceUrl`: a local pre-check mirroring the Worker's
    /// `parse_provider_source`, so an obvious typo is caught before a round
    /// trip. Nil when the URL looks importable.
    public static func validateSourceUrl(_ value: String) -> String? {
        let raw = value.dartTrimmed
        if raw.isEmpty { return "Paste a repository URL." }
        let withScheme = raw.contains("://") ? raw : "https://\(raw)"
        guard let uri = SimpleURI(withScheme), uri.scheme == "https", uri.userInfo.isEmpty else {
            return "Use an https:// repository URL."
        }
        var host = uri.host.lowercased()
        if host.hasPrefix("www.") { host = String(host.dropFirst(4)) }
        if !providers.contains(host) {
            return "Only \(providers.joined(separator: ", ")) repositories can be imported."
        }
        if uri.pathSegments.filter({ !$0.isEmpty }).count < 2 {
            return "Include the owner and the repository name."
        }
        return nil
    }
}

/// The parts of a URI the import check reads, for an
/// absolute `scheme://authority/path` string.
struct SimpleURI {
    let scheme: String
    let userInfo: String
    let host: String
    let pathSegments: [String]

    init?(_ text: String) {
        guard let separator = text.range(of: "://") else { return nil }
        let scheme = String(text[..<separator.lowerBound])
        guard let first = scheme.unicodeScalars.first, CharacterSet.letters.contains(first),
              scheme.unicodeScalars.allSatisfy({ CharacterSet.alphanumerics.contains($0) || "+-.".unicodeScalars.contains($0) })
        else { return nil }
        self.scheme = scheme.lowercased()
        var rest = Substring(text[separator.upperBound...])
        if let cut = rest.firstIndex(where: { $0 == "?" || $0 == "#" }) { rest = rest[..<cut] }
        let authority: Substring
        let path: Substring
        if let slash = rest.firstIndex(of: "/") {
            authority = rest[..<slash]
            path = rest[slash...]
        } else {
            authority = rest
            path = ""
        }
        var hostPort = authority
        if let at = authority.lastIndex(of: "@") {
            userInfo = String(authority[..<at])
            hostPort = authority[authority.index(after: at)...]
        } else {
            userInfo = ""
        }
        if hostPort.hasPrefix("[") {
            guard let close = hostPort.firstIndex(of: "]") else { return nil }
            host = String(hostPort[...close])
        } else if let colon = hostPort.lastIndex(of: ":") {
            let port = hostPort[hostPort.index(after: colon)...]
            guard port.allSatisfy({ $0.isASCII && $0.isNumber }) else { return nil }
            host = String(hostPort[..<colon])
        } else {
            host = String(hostPort)
        }
        pathSegments = path.isEmpty ? [] : path.dropFirst().components(separatedBy: "/")
    }
}
