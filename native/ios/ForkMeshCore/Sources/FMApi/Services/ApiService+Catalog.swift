// ApiService, part 3: the catalog and network endpoints
// (repositories, publish and import, network stats and leaderboards with their shape checks,
// ForkMesh Ads).

import FMAccount
import FMBase
import FMNet
import Foundation

extension ApiService {
    // MARK: Catalog

    /// `GET /api/repositories`, mirrors collapsed (`collapseRepositoryMirrors`), cached for
    /// `catalogTtl`. Also learns which public owner each serving node's row routes under.
    public func repositories() async throws -> [Repository] {
        try await cached(repositoriesCache, "", ttl: ApiService.catalogTtl) {
            let data = try await self.getJson(self.url("/api/repositories"))
            let repos = collapseRepositoryMirrors(
                ApiService.asList(data).compactMap(\.objectValue).map(Repository.init(json:))
                    .filter { !$0.owner.isEmpty && !$0.name.isEmpty })
            for repo in repos {
                let key = "\(repo.owner.lowercased())/\(repo.name.lowercased())"
                if !repo.publicOwner.isEmpty && repo.publicOwner.lowercased() != repo.owner.lowercased() {
                    self.publicRouteOwners[key] = repo.publicOwner
                } else {
                    self.publicRouteOwners.removeValue(forKey: key)
                }
            }
            return repos
        }
    }

    /// Publish (or refresh) a catalog record: `POST /api/repositories`. `payload` already carries
    /// the normalized record plus `catalogSig` and `catalogSigVersion`; the status is returned
    /// so the caller can act on `202 mirror:true` and `409 stale_update`.
    public func publishRepository(_ payload: JSONObject) async throws -> ApiJsonResult {
        try await postJsonResult(url("/api/repositories"), payload)
    }

    /// Import external repository metadata: `POST /api/repository-imports`. `providerToken` is
    /// used for that single request and never stored.
    public func createRepositoryImport(
        sourceUrl: String, targetOwner: String, mode: String = "import", providerToken: String = ""
    ) async throws -> ApiJsonResult {
        var body: JSONObject = [
            "sourceUrl": .string(sourceUrl), "targetOwner": .string(targetOwner), "mode": .string(mode),
        ]
        if !providerToken.isEmpty { body["providerToken"] = .string(providerToken) }
        if hasSession { body["sessionToken"] = .string(sessionToken) }
        return try await postJsonResult(url("/api/repository-imports"), body)
    }

    // MARK: Network

    /// `GET /api/network/stats`, cached for `catalogTtl`. Throws
    /// "Invalid network stats response" for an answer of the wrong shape.
    public func networkStats() async throws -> NetworkStats {
        try await cached(networkStatsCache, "", ttl: ApiService.catalogTtl) {
            let data = try await self.getJson(self.url("/api/network/stats"))
            if let map = data.objectValue, NetworkShape.hasNetworkStatsShape(map) { return NetworkStats(json: map) }
            throw ApiException("Invalid network stats response")
        }
    }

    /// `GET /api/network/leaderboards`, cached for `catalogTtl`.
    public func networkLeaderboards() async throws -> NetworkLeaderboards {
        try await cached(networkLeaderboardsCache, "", ttl: ApiService.catalogTtl) {
            let data = try await self.getJson(self.url("/api/network/leaderboards"))
            if let map = data.objectValue, NetworkShape.hasNetworkLeaderboardsShape(map) {
                return NetworkLeaderboards(json: map)
            }
            throw ApiException("Invalid network leaderboards response")
        }
    }

    // MARK: ForkMesh Ads

    /// The current ForkMesh Ads creative for the `mobile` placement, or nil.
    ///
    /// The serve call carries no viewer input (no session, no account, only the placement) and
    /// bypasses the performance monitor. It never throws: a failure, a null ad or a payload that
    /// fails `SponsoredAd.parse` all mean "render nothing". The outcome is held for
    /// `sponsoredAdTtl` (`sponsoredAdFailureTtl` after a failure) and not retried in between.
    public func sponsoredAd() async -> SponsoredAd? {
        let now = clock.now
        if let task = sponsoredAdCache.fresh("", ttl: sponsoredAdCacheTtl, now: now, epoch: relayEpoch) {
            return (try? await task.value) ?? nil
        }
        sponsoredAdCacheTtl = ApiService.sponsoredAdTtl
        let target = url("/api/ads/serve", [("placement", "mobile")])
        let task = sponsoredAdCache.load("", now: now, epoch: relayEpoch) {
            do {
                let response = try await self.http.send(HTTPRequest(
                    method: "GET", url: target, headers: ["Accept": "application/json"],
                    timeout: ApiService.sponsoredAdTimeout))
                if response.status != 200 { throw ApiException("ads \(response.status)") }
                let data = try JSON.parse(response.body)
                guard let map = data.objectValue, map["ok"] == .bool(true) else { throw ApiException("ads payload") }
                return SponsoredAd.parse(map["ad"])
            } catch {
                self.sponsoredAdCacheTtl = ApiService.sponsoredAdFailureTtl
                return nil
            }
        }
        return (try? await task.value) ?? nil
    }

    /// Tell the ad network the user followed `id`'s link. Fire-and-forget, unauthenticated and
    /// silent on every failure. Ids that are not 32 lower-case hex characters are ignored.
    public func sponsoredAdClicked(_ id: String) async {
        let bytes = Array(id.utf8)
        guard bytes.count == 32, bytes.allSatisfy({ ($0 >= 0x30 && $0 <= 0x39) || ($0 >= 0x61 && $0 <= 0x66) }) else {
            return
        }
        _ = try? await http.send(HTTPRequest(
            method: "POST", url: url("/api/ads/\(id)/click"),
            headers: ["Accept": "application/json", "Content-Type": "application/json"],
            body: JSON.encodeData(["placement": "mobile"]), timeout: ApiService.sponsoredAdTimeout))
    }
}

/// The response-shape checks behind `networkStats()` and `networkLeaderboards()`
/// (`_hasNetworkStatsShape`, `_hasNetworkLeaderboardsShape` and their helpers).
public enum NetworkShape {
    public static func hasNetworkStatsShape(_ data: JSONObject) -> Bool {
        let hasWorkerShape = ["repos", "hosts", "clients", "onlineNodes"].allSatisfy { data.contains($0) }
        if hasWorkerShape {
            return ["clients", "hosts", "repos"].allSatisfy { isIntegerLike(data[$0]) }
                && isStringList(data["onlineNodes"])
        }
        return ["nodesOnline", "hostsOnline", "repos"].allSatisfy { data.contains($0) && isIntegerLike(data[$0]) }
    }

    public static func hasNetworkLeaderboardsShape(_ data: JSONObject) -> Bool {
        ["fundsMainnodes", "fundsContributors", "fundsProjects"].allSatisfy {
            data.contains($0) && isFundsReceivedList(data[$0])
        }
    }

    /// A finite, non-negative whole number, or a string `int.tryParse` reads as one.
    static func isIntegerLike(_ value: JSONValue?) -> Bool {
        switch value {
        case .int(let n)?: return n >= 0
        case .double(let d)?: return d.isFinite && d >= 0 && d == d.rounded(.towardZero)
        case .string(let s)?:
            guard let parsed = DartCore.intTryParse(s.dartTrimmed) else { return false }
            return parsed >= 0
        default: return false
        }
    }

    static func isStringList(_ value: JSONValue?) -> Bool {
        guard let list = value?.arrayValue else { return false }
        return list.allSatisfy { $0.stringValue != nil }
    }

    static func isFundsReceivedList(_ value: JSONValue?) -> Bool {
        guard let list = value?.arrayValue else { return false }
        return list.allSatisfy(isFundsReceivedEntry)
    }

    static func isFundsReceivedEntry(_ value: JSONValue) -> Bool {
        guard let map = value.objectValue else { return false }
        let hasName = ["name", "node", "account", "project", "repo"].contains { key in
            guard let text = map[key]?.stringValue else { return false }
            return !text.dartTrimmed.isEmpty
        }
        if !hasName || !map.contains("lamports") { return false }
        if !isIntegerLike(map["lamports"]) { return false }
        guard let sol = map["sol"], !sol.isNull else { return true }
        return isFiniteNonNegativeNumber(sol)
    }

    static func isFiniteNonNegativeNumber(_ value: JSONValue) -> Bool {
        switch value {
        case .int(let n): return n >= 0
        case .double(let d): return d.isFinite && d >= 0
        case .string(let s):
            guard let parsed = DartCore.doubleTryParse(s.dartTrimmed) else { return false }
            return parsed.isFinite && parsed >= 0
        default: return false
        }
    }
}
