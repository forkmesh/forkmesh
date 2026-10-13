// A deliberately small, fail-closed reader of `.git/config`: anything that
// could run a program, redirect the transport or change the repository
// format is rejected rather than ignored.

import Foundation

/// A configured remote: `remote "<name>".url` and one fetch refspec.
public struct GitRemoteConfig: Sendable {
    public let name: String
    public let url: GitRemoteUrl
    public let fetch: GitFetchRefspec
}

/// A configured upstream: `branch "<name>".remote` and `.merge`.
public struct GitBranchConfig: Sendable {
    public let name: String
    public let remote: String
    public let merge: GitRefName
}

public struct RepositoryConfig: Sendable {
    public let fileMode: Bool
    public let remotes: [String: GitRemoteConfig]
    public let branches: [String: GitBranchConfig]

    public init(fileMode: Bool, remotes: [String: GitRemoteConfig] = [:], branches: [String: GitBranchConfig] = [:]) {
        self.fileMode = fileMode
        self.remotes = remotes
        self.branches = branches
    }

    public static func load(_ file: String) throws -> RepositoryConfig {
        try parse(try EngineFS.readBytes(file))
    }

    public static func parse(_ bytes: [UInt8]) throws -> RepositoryConfig {
        // Insertion-ordered so validation failures surface in file order.
        var remoteOrder = [String]()
        var remoteValues = [String: [String: [String]]]()
        var branchOrder = [String]()
        var branchValues = [String: [String: [String]]]()
        var fileMode = true
        for entry in try entries(bytes) {
            let section = entry.section
            let key = entry.key
            let value = entry.value
            if section == "core" && key == "repositoryformatversion" && !value.engSame("0") {
                throw GitException(.unsupportedRepositoryFormat, "repository format is unsupported")
            }
            if section == "core" && key == "bare" && !value.engSame("false") {
                throw GitException(.unsupportedRepositoryFormat, "bare repository is unsupported")
            }
            if section == "core" && key == "worktree" {
                throw GitException(.unsupportedRepositoryFormat, "external worktree is unsupported")
            }
            if section == "extensions" && key == "objectformat" && !value.engSame("sha1") {
                throw GitException(.unsupportedRepositoryFormat, "object format is unsupported")
            }
            if section == "core" && key == "filemode" {
                if !value.engSame("true") && !value.engSame("false") {
                    throw GitException(.unsupportedRepositoryFormat, "core.filemode is invalid")
                }
                fileMode = value.engSame("true")
            }
            if section == "remote", let subsection = entry.subsection {
                if remoteValues[subsection] == nil { remoteOrder.append(subsection) }
                remoteValues[subsection, default: [:]][key, default: []].append(value)
            }
            if section == "branch", let subsection = entry.subsection {
                if branchValues[subsection] == nil { branchOrder.append(subsection) }
                branchValues[subsection, default: [:]][key, default: []].append(value)
            }
        }
        var remotes = [String: GitRemoteConfig]()
        for name in remoteOrder {
            let values = remoteValues[name] ?? [:]
            try GitRemoteName.validate(name)
            let urls = values["url"] ?? []
            let fetches = values["fetch"] ?? []
            if urls.count != 1 || fetches.count > 1 {
                throw GitException(
                    .unsupportedRepositoryFormat,
                    "remote must have one url and at most one fetch refspec"
                )
            }
            let fetch = fetches.isEmpty
                ? try GitFetchRefspec.standard(name)
                : try GitFetchRefspec.parse(fetches[0])
            if fetch.isWildcard && !fetch.destination.engSame("refs/remotes/\(name)/*") {
                throw GitException(
                    .unsupportedRepositoryFormat,
                    "remote fetch refspec must target its own remote namespace"
                )
            }
            remotes[name] = GitRemoteConfig(name: name, url: try GitRemoteUrl.parse(urls[0]), fetch: fetch)
        }
        var branches = [String: GitBranchConfig]()
        for name in branchOrder {
            let values = branchValues[name] ?? [:]
            let remoteNames = values["remote"] ?? []
            let merges = values["merge"] ?? []
            if remoteNames.isEmpty && merges.isEmpty { continue }
            if remoteNames.count != 1 || merges.count != 1 {
                throw GitException(.unsupportedRepositoryFormat, "branch must have one remote and one merge ref")
            }
            _ = try GitRefName.branch("refs/heads/\(name)")
            try GitRemoteName.validate(remoteNames[0])
            branches[name] = GitBranchConfig(
                name: name,
                remote: remoteNames[0],
                merge: try GitRefName.branch(merges[0])
            )
        }
        return RepositoryConfig(fileMode: fileMode, remotes: remotes, branches: branches)
    }

    /// Rewrites `[remote "<name>"]` in `file` with exactly `url` and `fetch`.
    ///
    /// Other sections are preserved byte-for-byte; the write is staged in a
    /// lock file and atomically renamed.
    public static func writeRemote(_ file: String, _ name: String, url: GitRemoteUrl, fetch: GitFetchRefspec) throws {
        try GitRemoteName.validate(name)
        try replaceSection(file, "remote", name, [("url", url.value), ("fetch", fetch.value)])
    }

    /// Rewrites `[branch "<name>"]` in `file` with `remote` and `merge`.
    public static func writeBranchUpstream(_ file: String, _ name: String, remote: String, merge: GitRefName) throws {
        _ = try GitRefName.branch("refs/heads/\(name)")
        try GitRemoteName.validate(remote)
        try replaceSection(file, "branch", name, [("remote", remote), ("merge", merge.value)])
    }

    private static func replaceSection(
        _ file: String,
        _ section: String,
        _ subsection: String,
        _ values: [(String, String)]
    ) throws {
        if subsection.engContains("\"") || subsection.engContains("\\") || subsection.engContains("\n") {
            throw GitException(
                .unsupportedRepositoryFormat,
                "config subsection name needs quoting that is unsupported"
            )
        }
        let original = EngineFS.fileExists(file) ? try EngineFS.readBytes(file) : []
        // Validate before rewriting so a malformed file is never silently fixed.
        _ = try parse(original)
        let text = engUtf8Decode(original)
        var kept = [String]()
        var skipping = false
        for line in engSplitLines(text) {
            if let header = sectionHeader.firstMatch(line.engTrimLeft()) {
                skipping = (header[1] ?? "").lowercased() == section && (header[2] ?? nil)?.engSame(subsection) == true
                if skipping { continue }
            }
            if !skipping { kept.append(line) }
        }
        while let last = kept.last, last.isEmpty { kept.removeLast() }
        var buffer = ""
        for line in kept { buffer += line + "\n" }
        buffer += "[\(section) \"\(subsection)\"]\n"
        for (key, value) in values {
            if value.engContains("\n") || value.engContains("\r") {
                throw GitException(.unsupportedRepositoryFormat, "config value contains a line break")
            }
            buffer += "\t\(key) = \(value)\n"
        }
        let bytes = Array(buffer.utf8)
        _ = try parse(bytes)
        let lock = "\(file).lock"
        if !EngineFS.createFileExclusive(lock) {
            throw GitException(.refLocked, "config lock already exists")
        }
        defer {
            if EngineFS.fileExists(lock) { try? EngineFS.deleteFile(lock) }
        }
        try EngineFS.writeBytesFlushed(lock, bytes)
        try EngineFS.rename(lock, file)
    }

    /// `^\[([A-Za-z][A-Za-z0-9-]*)(?:[ \t]+"([^"\\\x00-\x1f]*)")?\]$`
    private static let sectionHeader = EngineRegex(
        "^\\[([A-Za-z][A-Za-z0-9-]*)(?:[ \\t]+\"([^\"\\\\\\x00-\\x1f]*)\")?\\]\\z"
    )

    /// `^([A-Za-z][A-Za-z0-9-]*)[ \t]*=[ \t]*(.*)$`
    private static let keyLine = EngineRegex("^([A-Za-z][A-Za-z0-9-]*)[ \\t]*=[ \\t]*(.*)\\z")

    private struct Entry {
        let section: String
        let subsection: String?
        let key: String
        let value: String
    }

    private static func entries(_ bytes: [UInt8]) throws -> [Entry] {
        if bytes.count > 1024 * 1024 {
            throw GitException(.resourceLimitExceeded, "config exceeds one MiB")
        }
        guard let text = engUtf8DecodeStrict(bytes) else {
            throw GitException(.unsupportedRepositoryFormat, "config is not valid UTF-8")
        }
        var output = [Entry]()
        var section: String?
        var subsection: String?
        for line in engSplitLines(text) {
            let trimmed = line.engTrimLeft()
            if trimmed.isEmpty || trimmed.engHasPrefix("#") || trimmed.engHasPrefix(";") { continue }
            if trimmed.engHasPrefix("[") {
                guard let match = sectionHeader.firstMatch(trimmed) else {
                    throw GitException(
                        .unsupportedRepositoryFormat,
                        "config section header is malformed or uses unsupported quoting"
                    )
                }
                let name = (match[1] ?? "").lowercased()
                section = name
                subsection = match[2] ?? nil
                if name == "include" || name == "includeif" {
                    throw GitException(.unsupportedRepositoryFormat, "config includes are unsupported")
                }
                if name == "alias" || name == "credential" || name == "url" {
                    throw GitException(.unsupportedRepositoryFormat, "config section is unsupported")
                }
                continue
            }
            guard let currentSection = section, let match = keyLine.firstMatch(trimmed) else {
                throw GitException(.unsupportedRepositoryFormat, "config line is invalid")
            }
            let key = (match[1] ?? "").lowercased()
            let value = match[2] ?? ""
            if value.engHasSuffix("\\") || value.engHasPrefix("\"") || value.engContains("\\\"") {
                throw GitException(
                    .unsupportedRepositoryFormat,
                    "config value quoting or continuation is unsupported"
                )
            }
            if key.contains("hook") || key.contains("helper") || key.contains("command")
                || key == "external" || key == "driver" || key == "pushurl" || key == "proxy"
                || key == "sslcainfo" || key == "sslcert" || key == "sslkey"
            {
                throw GitException(
                    .unsupportedRepositoryFormat,
                    "executable or transport-overriding config is unsupported"
                )
            }
            output.append(Entry(section: currentSection, subsection: subsection, key: key, value: value))
        }
        return output
    }
}
