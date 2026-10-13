// The identity of this build, for
// anything that has to say which app produced it (bug reports, telemetry).

import Foundation

public struct AppBuild: Equatable, Sendable {
    /// `<marketing version>+<build number>` ("1.2.2+13").
    public var version: String
    /// Short platform name sent with a report: `ios`, `macos`, `linux`.
    public var platform: String
    /// Operating-system version string, or "" where it cannot be read.
    public var osVersion: String
    public var commit: String

    public init(
        version: String,
        platform: String = AppBuild.currentPlatform,
        osVersion: String = AppBuild.currentOSVersion,
        commit: String = ""
    ) {
        self.version = version
        self.platform = platform
        self.osVersion = osVersion
        self.commit = commit
    }

    /// From `Bundle.main.infoDictionary`: CFBundleShortVersionString and CFBundleVersion.
    public init(infoDictionary: [String: Any]?) {
        let short = infoDictionary?["CFBundleShortVersionString"] as? String ?? ""
        let build = infoDictionary?["CFBundleVersion"] as? String ?? ""
        let base = short.isEmpty ? "0.0.0" : short
        let commit = (infoDictionary?["FORKMESH_BUILD_COMMIT"] as? String ?? "").trimmingCharacters(in: .whitespaces)
        self.init(version: build.isEmpty ? base : "\(base)+\(build)", commit: commit.hasPrefix("$(") ? "" : commit)
    }

    public static var currentPlatform: String {
        #if os(iOS)
        return "ios"
        #elseif os(macOS)
        return "macos"
        #elseif os(Linux)
        return "linux"
        #else
        return "unknown"
        #endif
    }

    public static var currentOSVersion: String { ProcessInfo.processInfo.operatingSystemVersionString }
}

/// What the app target builds once at startup and hands to the services.
public struct AppConfig: Equatable, Sendable {
    public var env: AppEnv
    public var build: AppBuild

    public init(env: AppEnv = .prod, build: AppBuild) {
        self.env = env
        self.build = build
    }

    public init(infoDictionary: [String: Any]?) {
        self.init(env: AppEnv(infoDictionary: infoDictionary), build: AppBuild(infoDictionary: infoDictionary))
    }
}
