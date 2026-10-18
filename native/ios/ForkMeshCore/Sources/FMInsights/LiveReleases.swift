import FMBase
import FMCrypto
import Foundation

public struct ReleasePlatformAsset: Equatable, Sendable {
    public let name: String
    public let arch: String

    public init(_ name: String, _ arch: String) {
        self.name = name
        self.arch = arch
    }
}

public struct ReleasePlatform: Equatable, Sendable {
    public let id: String
    public let title: String
    public let kind: String
    public let assets: [ReleasePlatformAsset]
    public let required: Bool
}

public struct ReleaseAsset: Equatable, Sendable {
    public var name: String
    public var sha256: String
    public var size: Int64
    public var build: Int

    public var valid: Bool {
        size > 0 && sha256.count == 64 && sha256.allSatisfy { ("0"..."9").contains($0) || ("a"..."f").contains($0) }
    }
}

public struct SignedRelease: Equatable, Sendable {
    public var tag: String
    public var assets: [String: ReleaseAsset]

    public func asset(_ name: String) -> ReleaseAsset? {
        guard let asset = assets[name], asset.valid else { return nil }
        return asset
    }
}

public struct PlatformBuild: Equatable, Sendable {
    public var arch: String
    public var tag: String
    public var build: Int
}

public struct PlatformRelease: Equatable, Sendable {
    public var platform: ReleasePlatform
    public var builds: [PlatformBuild]

    public var newestTag: String {
        builds.map(\.tag).reduce("") { best, tag in best.isEmpty || LiveReleases.isNewer(tag, than: best) ? tag : best }
    }
}

public struct LiveReleaseSummary: Equatable, Sendable {
    public var latest: String
    public var newest: String
    public var platforms: [PlatformRelease]

    public var downloads: Int { platforms.reduce(0) { $0 + $1.builds.count } }
}

public enum LiveReleases {
    public static let latestId = "release-latest"
    public static let iosId = "release-ios"
    public static let repoBlobs = "/api/repo/forkmesh/forkmesh/blobs"
    public static let channelRoot = ".forkmesh/releases/"
    public static let versionFile = "desktop/CMakeLists.txt"
    public static let appStoreStatus = "/api/app-store-status"
    public static let lookback = 6
    public static let publicKeySPKI = "MCowBQYDK2VwAyEAHBYOCW4qnyZkYAnEoqUrYxPiDRszjfJa+xJUeyQPUS0="
    static let liveStates: Set<String> = ["READY_FOR_SALE", "READY_FOR_DISTRIBUTION"]

    public static let publicKey: [UInt8] = Array(((try? B64.decode(publicKeySPKI)) ?? Data()).suffix(Ed25519.publicKeyBytes))

    public static let platforms: [ReleasePlatform] = [
        ReleasePlatform(id: "release-android", title: "Android", kind: "APK",
                        assets: [ReleasePlatformAsset("forkmesh-android-universal.apk", "Universal")], required: true),
        ReleasePlatform(id: "release-macos", title: "macOS", kind: "DMG",
                        assets: [ReleasePlatformAsset("forkmesh-macos-arm64.dmg", "Apple silicon"),
                                 ReleasePlatformAsset("forkmesh-macos-x86_64.dmg", "Intel")],
                        required: false),
        ReleasePlatform(id: "release-windows", title: "Windows", kind: "Installer",
                        assets: [ReleasePlatformAsset("forkmesh-windows-x86_64-setup.exe", "x86_64")], required: false),
        ReleasePlatform(id: "release-linux", title: "Linux", kind: "AppImage",
                        assets: [ReleasePlatformAsset("forkmesh-linux-x86_64.AppImage", "x86_64")], required: true),
    ]

    public static let homepageIds: Set<String> = Set([latestId] + platforms.map(\.id))

    public static let catalog: [StatsTileCatalogEntry] = [
        StatsTileCatalogEntry(id: latestId, title: "Latest release", group: "Releases",
                              hint: "The newest signed ForkMesh release the homepage announces", symbol: "arrow.down.circle"),
        StatsTileCatalogEntry(id: "release-android", title: "Android version", group: "Releases",
                              hint: "The Android APK the homepage offers for download", symbol: "candybarphone"),
        StatsTileCatalogEntry(id: "release-macos", title: "macOS version", group: "Releases",
                              hint: "The macOS DMGs the homepage offers for download", symbol: "laptopcomputer"),
        StatsTileCatalogEntry(id: "release-windows", title: "Windows version", group: "Releases",
                              hint: "The Windows installer the homepage offers for download", symbol: "pc"),
        StatsTileCatalogEntry(id: "release-linux", title: "Linux version", group: "Releases",
                              hint: "The Linux AppImage the homepage offers for download", symbol: "terminal"),
        StatsTileCatalogEntry(id: iosId, title: "iOS version", group: "Releases",
                              hint: "The iPhone version live on the App Store, from your App Store status", symbol: "iphone"),
    ]

    public static let defaults: [StatsTilePlacement] = catalog.map { StatsTilePlacement(id: $0.id) }

    public static func versionParts(_ tag: String) -> [Int]? {
        var text = tag.trimmingCharacters(in: .whitespaces)
        if text.hasPrefix("v") { text.removeFirst() }
        let parts = text.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 3 else { return nil }
        let numbers = parts.compactMap { part -> Int? in
            part.isEmpty || !part.allSatisfy(\.isASCII) || !part.allSatisfy(\.isNumber) ? nil : Int(part)
        }
        return numbers.count == 3 ? numbers : nil
    }

    public static func isNewer(_ tag: String, than other: String) -> Bool {
        guard let a = versionParts(tag), let b = versionParts(other) else { return false }
        for index in 0..<3 where a[index] != b[index] { return a[index] > b[index] }
        return false
    }

    public static func projectVersion(_ text: String) -> String? {
        guard let regex = try? NSRegularExpression(pattern: #"project\(\s*ForkMesh\s+VERSION\s+(\d+\.\d+\.\d+)"#),
              let match = regex.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)),
              let range = Range(match.range(at: 1), in: text) else { return nil }
        return String(text[range])
    }

    public static func manifestPaths(_ channel: String) -> [String] {
        ["\(channelRoot)\(channel)/release.json", "\(channelRoot)\(channel)/release.json.sig"]
    }

    public static func blobsPath(_ paths: [String]) -> String {
        var allowed = CharacterSet.alphanumerics
        allowed.insert(charactersIn: "-._~")
        let query = paths.map { "path=" + ($0.addingPercentEncoding(withAllowedCharacters: allowed) ?? $0) }
        return repoBlobs + "?" + query.joined(separator: "&") + "&ref=main"
    }

    public static func earlierChannels(_ tag: String) -> [String] {
        guard let parts = versionParts(tag), parts[2] > 0 else { return [] }
        return stride(from: parts[2] - 1, through: max(0, parts[2] - lookback), by: -1).map {
            "candidate-\(parts[0]).\(parts[1]).\($0)"
        }
    }

    public static func blobBytes(_ blob: JSONValue?) -> [UInt8]? {
        guard let object = blob?.objectValue, let content = object.optStr("content") else { return nil }
        if object.str("encoding") == "base64" {
            guard let data = try? B64.decode(content.trimmingCharacters(in: .whitespacesAndNewlines)) else { return nil }
            return Array(data)
        }
        return Array(content.utf8)
    }

    public static func verified(_ blobs: JSONObject, channel: String, key: [UInt8] = publicKey) -> SignedRelease? {
        let paths = manifestPaths(channel)
        guard let manifest = blobBytes(blobs[paths[0]]), let signature = blobBytes(blobs[paths[1]]),
              signature.count == Ed25519.signatureBytes,
              Ed25519.verify(publicKey: key, message: manifest, signature: signature),
              let json = (try? JSON.parse(Data(manifest)))?.objectValue,
              let tag = json.optStr("tag"),
              json.str("schema") == "forkmesh-release-v2", json.str("repo") == "forkmesh/forkmesh",
              json["assets"]?.arrayValue != nil else { return nil }
        var assets: [String: ReleaseAsset] = [:]
        for asset in json.maps("assets") {
            let name = asset.str("name")
            if !name.isEmpty, assets[name] == nil {
                assets[name] = ReleaseAsset(name: name, sha256: asset.str("blob_sha256"), size: asset.int64("size"),
                                            build: asset.int("build"))
            }
        }
        return SignedRelease(tag: tag, assets: assets)
    }

    @MainActor
    public static func load(_ transport: InsightsRequesting, key: [UInt8] = publicKey) async throws -> LiveReleaseSummary {
        let first = try await transport.call("GET", blobsPath(manifestPaths("latest") + [versionFile]), nil).obj("blobs")
        guard let latest = verified(first, channel: "latest", key: key) else {
            throw InsightsError("release_unverified", "The signed release manifest is missing or does not verify.")
        }
        let project = blobBytes(first[versionFile]).flatMap { projectVersion(String(decoding: $0, as: UTF8.self)) }
        let newest = project.flatMap { isNewer($0, than: latest.tag) ? $0 : nil }
        let missing = platforms.contains { platform in
            !platform.required && platform.assets.contains { latest.asset($0.name) == nil }
        }
        let channels = (newest.map { ["candidate-\($0)"] } ?? []) + (missing ? earlierChannels(latest.tag) : [])
        var more = JSONObject()
        if !channels.isEmpty {
            more = (try? await transport.call("GET", blobsPath(channels.flatMap(manifestPaths)), nil).obj("blobs")) ?? JSONObject()
        }
        return summarize(latest, newest: newest, blobs: more, key: key)
    }

    public static func summarize(_ latest: SignedRelease, newest: String?, blobs: JSONObject,
                                 key: [UInt8] = publicKey) -> LiveReleaseSummary {
        let earlier = earlierChannels(latest.tag).map { channel -> SignedRelease? in
            let version = String(channel.dropFirst("candidate-".count))
            guard let release = verified(blobs, channel: channel, key: key), release.tag == "v\(version)" else { return nil }
            return release
        }
        let rows = platforms.map { platform in
            PlatformRelease(platform: platform, builds: platform.assets.compactMap { asset -> PlatformBuild? in
                if let found = latest.asset(asset.name) {
                    return PlatformBuild(arch: asset.arch, tag: latest.tag, build: found.build)
                }
                if platform.required { return nil }
                for case let release? in earlier {
                    if let found = release.asset(asset.name) {
                        return PlatformBuild(arch: asset.arch, tag: release.tag, build: found.build)
                    }
                }
                return nil
            })
        }
        var shown = latest.tag
        if let newest, let candidate = verified(blobs, channel: "candidate-\(newest)", key: key),
           candidate.tag == "v\(newest)",
           platforms.contains(where: { platform in platform.assets.contains { candidate.asset($0.name) != nil } }) {
            shown = candidate.tag
        }
        return LiveReleaseSummary(latest: latest.tag, newest: shown, platforms: rows)
    }

    public static func tiles(_ summary: LiveReleaseSummary) -> [String: StatsTileData] {
        let count = summary.downloads
        var tiles: [String: StatsTileData] = [
            latestId: StatsTileData(
                value: summary.newest,
                note: summary.newest != summary.latest
                    ? "downloads are \(summary.latest) until \(summary.newest) builds are published"
                    : "\(count) verified download\(count == 1 ? "" : "s")",
                status: summary.newest == summary.latest ? .good : .none),
        ]
        for row in summary.platforms { tiles[row.platform.id] = platformTile(row, latest: summary.latest) }
        return tiles
    }

    static func platformTile(_ row: PlatformRelease, latest: String) -> StatsTileData {
        let platform = row.platform
        if row.builds.isEmpty {
            return StatsTileData(note: platform.required ? "Not in \(latest)" : "Coming soon", available: false)
        }
        let tag = row.newestTag
        let mixed = row.builds.contains { $0.tag != tag }
        let archs = platform.assets.count < 2 ? ""
            : mixed ? row.builds.map { "\($0.arch) \($0.tag)" }.joined(separator: ", ")
            : row.builds.map(\.arch).joined(separator: ", ")
        let build = row.builds.filter { $0.tag == tag }.map(\.build).max() ?? 0
        let behind = row.builds.contains { $0.tag != latest }
        let note = [platform.kind, archs, build > 1 ? "build \(build)" : "", behind ? "behind \(latest)" : ""]
            .filter { !$0.isEmpty }.joined(separator: " · ")
        return StatsTileData(value: tag, note: note, status: behind ? .warning : .good)
    }

    public static func appStore(_ reply: JSONObject) -> StatsTileData {
        guard let snapshot = reply["snapshot"]?.objectValue else {
            return StatsTileData(note: "App Store status is not published yet", available: false)
        }
        let versions = snapshot.maps("versions")
        let liveIndex = versions.firstIndex { liveStates.contains($0.obj("status").str("state").trimmingCharacters(in: .whitespaces)) }
        let live = liveIndex.map { versions[$0] }
        var pending = ""
        if let next = versions.first, liveIndex != 0, !next.str("version").isEmpty {
            let status = next.obj("status")
            let label = status.str("label").isEmpty ? status.str("state") : status.str("label")
            pending = "\(tagFor(next.str("version"))) \(label.lowercased())"
        }
        guard let live, !live.str("version").isEmpty else {
            return StatsTileData(note: pending.isEmpty ? "No version is live on the App Store yet" : pending, available: false)
        }
        let build = live.str("build")
        let note = ["App Store", build.isEmpty ? "" : "build \(build)", pending].filter { !$0.isEmpty }.joined(separator: " · ")
        return StatsTileData(value: tagFor(live.str("version")), note: note, status: .good)
    }

    static func tagFor(_ version: String) -> String {
        version.first?.isNumber == true ? "v\(version)" : version
    }
}
