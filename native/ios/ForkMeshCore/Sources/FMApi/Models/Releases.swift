import Foundation
import FMBase

public struct RepoReleaseAsset: Equatable, Hashable, Sendable {
    public var name: String
    public var sha256: String
    public var size: Int
    public var os: String
    public var arch: String
    public var downloads: Int

    public init(name: String, sha256: String, size: Int = 0, os: String = "", arch: String = "", downloads: Int = 0) {
        self.name = name
        self.sha256 = sha256
        self.size = size
        self.os = os
        self.arch = arch
        self.downloads = downloads
    }

    public init(json: JSONObject, downloads: Int = 0) {
        self.init(
            name: json.firstStr("name", "filename"),
            sha256: json.firstStr("sha256", "blob_sha256", "hash"),
            size: ModelJSON.int(ModelJSON.coalesce(json["size"], json["bytes"])),
            os: json.str("os"),
            arch: json.str("arch"),
            downloads: downloads
        )
    }

    public var shortSha: String { DartCore.length(sha256) > 12 ? DartCore.prefix(sha256, 12) : sha256 }

    public var platformLabel: String {
        let parts = [os, arch].filter { !$0.dartTrimmed.isEmpty }
        return parts.isEmpty ? "artifact" : parts.joined(separator: " / ")
    }

    /// "512 B", "2.0 KB", "14.3 MB", or "unknown size".
    public var sizeLabel: String {
        if size <= 0 { return "unknown size" }
        let units = ["B", "KB", "MB", "GB"]
        var value = Double(size)
        var unit = 0
        while value >= 1024 && unit < units.count - 1 {
            value /= 1024
            unit += 1
        }
        return "\(DartCore.toStringAsFixed(value, unit == 0 ? 0 : 1)) \(units[unit])"
    }

    public var downloadLabel: String { "\(downloads) \(downloads == 1 ? "download" : "downloads")" }
}

public struct RepoRelease: Equatable, Hashable, Sendable {
    public var tag: String
    public var channel: String
    public var tagCommit: String
    public var createdAtMs: Int
    public var assets: [RepoReleaseAsset]

    public init(tag: String, channel: String = "", tagCommit: String = "", createdAtMs: Int = 0, assets: [RepoReleaseAsset] = []) {
        self.tag = tag
        self.channel = channel
        self.tagCommit = tagCommit
        self.createdAtMs = createdAtMs
        self.assets = assets
    }

    /// `downloads` counts downloads by asset sha256.
    public init(json: JSONObject, downloads: [String: Int] = [:]) {
        let assets = json.maps("assets").map { map -> RepoReleaseAsset in
            let sha = map.firstStr("sha256", "blob_sha256", "hash")
            return RepoReleaseAsset(json: map, downloads: downloads[sha] ?? 0)
        }.filter { !$0.name.isEmpty || !$0.sha256.isEmpty }
        self.init(
            tag: json.firstStr("tag", "name"),
            channel: json.str("channel"),
            tagCommit: json.firstStr("tag_commit", "tagCommit", "commit"),
            createdAtMs: ModelJSON.int(ModelJSON.coalesce(json["created_at"], json["createdAt"], json["ts"])),
            assets: assets
        )
    }

    public var shortCommit: String { DartCore.length(tagCommit) > 8 ? DartCore.prefix(tagCommit, 8) : tagCommit }

    /// The local yyyy-MM-dd the release was cut, or "".
    public func createdDate(timeZone: TimeZone = .current) -> String {
        createdAtMs <= 0 ? "" : ModelJSON.localDate(createdAtMs, timeZone: timeZone)
    }

    public var createdDate: String { createdDate(timeZone: .current) }

    public var assetSummary: String {
        assets.isEmpty ? "No published artifacts yet" : "\(assets.count) \(assets.count == 1 ? "artifact" : "artifacts")"
    }
}
