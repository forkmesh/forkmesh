// The slice of ProjectScope the on-device repositories and the node-sync
// applier use: where a project's files live, and its preference-key namespace.
//
// WS1 (docs/native/PARITY.md, section 3) ports the whole `ProjectScope` into
// FMAccount. Until it lands this protocol is the shim: its requirements are
// the fields and methods WS1's contract names (id, namespace, directory), and
// every method has the ProjectScope behaviour as a default, so the port conforms with
// an empty `extension ProjectScope: LocalRepoScope {}` in the app target.

import Foundation

/// Where one project keeps its local repositories.
public protocol LocalRepoScope: Sendable {
    /// The project the scope belongs to; empty for the unpinned legacy scope.
    var projectId: String { get }
    /// Key prefix: empty for the legacy namespace, else `p/<projectId>/`.
    var namespace: String { get }
}

extension LocalRepoScope {
    public var isLegacy: Bool { namespace.isEmpty }

    /// A preferences key in this project's namespace.
    public func key(_ name: String) -> String { namespace + name }

    /// `base` for the legacy namespace, `<base>/projects/<slug>` otherwise.
    public func directory(_ base: URL) -> URL {
        if isLegacy { return base }
        return base.appendingPathComponent("projects", isDirectory: true)
            .appendingPathComponent(LegacyLocalRepoScope.slugFor(projectId), isDirectory: true)
    }
}

/// The pre-projects layout: the unprefixed keys
/// and the original directories. Also a pinned project's scope when built
/// with a project id and its namespace.
public struct LegacyLocalRepoScope: LocalRepoScope, Equatable {
    public let projectId: String
    public let namespace: String

    public init(projectId: String = "", namespace: String = "") {
        self.projectId = projectId
        self.namespace = namespace
    }

    /// A scope for a project created after the upgrade: `p/<projectId>/`.
    public static func project(_ projectId: String) -> LegacyLocalRepoScope {
        LegacyLocalRepoScope(projectId: projectId, namespace: namespaceFor(projectId))
    }

    public static func namespaceFor(_ projectId: String) -> String { "p/\(projectId)/" }

    /// Lower-cased, every UTF-16 code unit outside
    /// `[a-z0-9._-]` replaced by `_` (`host:port` and IPv6 literals carry
    /// characters some filesystems refuse).
    public static func slugFor(_ projectId: String) -> String {
        var units: [UInt16] = []
        for unit in projectId.lowercased().utf16 {
            let allowed = (unit >= 0x61 && unit <= 0x7a) || (unit >= 0x30 && unit <= 0x39)
                || unit == 0x2e || unit == 0x5f || unit == 0x2d
            units.append(allowed ? unit : 0x5f)
        }
        return String(decoding: units, as: UTF16.self)
    }
}
