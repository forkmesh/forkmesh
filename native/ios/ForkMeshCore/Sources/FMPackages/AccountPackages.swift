import FMApi
import FMBase
import Foundation
import Observation

public struct AccountPackageInfo: Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var defaultOn: Bool
    public var installedBefore: String
    public var dependencies: [String]
    public var settings: [PackageSetting]
    public var monitors: PackageMonitors

    public init(
        id: String, name: String = "", defaultOn: Bool = true, installedBefore: String = "", dependencies: [String] = [],
        settings: [PackageSetting] = [], monitors: PackageMonitors = PackageMonitors()
    ) {
        self.id = id
        self.name = name.isEmpty ? id : name
        self.defaultOn = defaultOn
        self.installedBefore = installedBefore
        self.dependencies = dependencies
        self.settings = settings
        self.monitors = monitors
    }

    public init(package: MarketPackage) {
        self.init(
            id: package.id, name: package.name, defaultOn: package.defaultOn, installedBefore: package.installedBefore,
            dependencies: package.dependencies, settings: package.settings, monitors: package.monitors)
    }

    public init?(json: JSONObject) {
        let id = json.str("id")
        guard PackagesText.isPackageId(id) else { return nil }
        self.init(
            id: id, name: PackagesText.bounded(json.str("name"), 80), defaultOn: json.bool("default"),
            installedBefore: PackagesText.installDate(json.str("installedBefore")),
            dependencies: json.strings("dependencies").filter(PackagesText.isPackageId),
            settings: json.maps("settings").compactMap(PackageSetting.init(json:)),
            monitors: PackageMonitors(json: json.obj("monitors")))
    }

    public var json: JSONObject {
        [
            "id": .string(id), "name": .string(name), "default": .bool(defaultOn), "installedBefore": .string(installedBefore),
            "dependencies": .array(dependencies.map(JSONValue.string)),
            "settings": .array(settings.map { .object($0.json) }), "monitors": .object(monitors.json),
        ]
    }
}

public struct AccountPackageEntry: Equatable, Sendable, Identifiable {
    public var id: String
    public var installed: Bool
    public var settings: JSONObject
    public var legacy: Bool
    public var places: [PackagePlace]
    public var availablePlaces: [PackagePlace]

    public init(
        id: String, installed: Bool, settings: JSONObject = [:], legacy: Bool = false,
        places: [PackagePlace] = PackagePlace.allCases, availablePlaces: [PackagePlace] = PackagePlace.allCases
    ) {
        self.id = id
        self.installed = installed
        self.settings = settings
        self.legacy = legacy
        self.places = PackagePlace.ordered(places)
        self.availablePlaces = PackagePlace.ordered(availablePlaces)
    }

    public init?(json: JSONObject) {
        let id = json.str("id")
        guard PackagesText.isPackageId(id) else { return nil }
        self.init(
            id: id, installed: json.bool("installed"), settings: json.obj("settings"), legacy: json.bool("legacy"),
            places: PackagePlace.list(json, "places"), availablePlaces: PackagePlace.list(json, "availablePlaces"))
    }

    public func shows(on place: PackagePlace) -> Bool { places.contains(place) }

    public var json: JSONObject {
        var out: JSONObject = [
            "id": .string(id), "installed": .bool(installed), "settings": .object(settings),
            "places": PackagePlace.json(places), "availablePlaces": PackagePlace.json(availablePlaces),
        ]
        if legacy { out["legacy"] = .bool(true) }
        return out
    }
}

public struct AccountPackagesState: Equatable, Sendable {
    public var packages: [AccountPackageEntry]
    public var changed: [String]

    public init(packages: [AccountPackageEntry], changed: [String] = []) {
        self.packages = packages
        self.changed = changed
    }

    public init(json: JSONObject) {
        self.init(
            packages: json.maps("packages").prefix(200).compactMap(AccountPackageEntry.init(json:)),
            changed: json.strings("changed").filter(PackagesText.isPackageId))
    }

    public func entry(_ id: String) -> AccountPackageEntry? { packages.first { $0.id == id } }

    public var json: JSONObject {
        ["packages": .array(packages.map { .object($0.json) })]
    }
}

public struct StatusMonitor: Equatable, Sendable, Identifiable {
    nonisolated public static let maxName = 40
    nonisolated public static let maxURL = 300

    public var id: String
    public var name: String
    public var url: String

    public init(id: String, name: String, url: String) {
        self.id = id
        self.name = name
        self.url = url
    }

    public init?(json: JSONObject) {
        let id = json.str("id")
        guard PackagesText.isHexId(id), StatusMonitor.problem(name: json.str("name"), url: json.str("url")) == nil else {
            return nil
        }
        self.init(id: id, name: json.str("name"), url: json.str("url"))
    }

    public var json: JSONObject { ["id": .string(id), "name": .string(name), "url": .string(url)] }

    public var address: URL? { URL(string: url) }

    public static func problem(name: String, url: String) -> String? {
        let name = name.trimmingCharacters(in: .whitespacesAndNewlines)
        if name.isEmpty || name.count > maxName { return "invalid_name" }
        let url = url.trimmingCharacters(in: .whitespacesAndNewlines)
        guard url.count <= maxURL, let parsed = URLComponents(string: url),
            let scheme = parsed.scheme?.lowercased(), scheme == "http" || scheme == "https",
            let host = parsed.host, !host.isEmpty, parsed.user == nil, parsed.password == nil
        else { return "invalid_url" }
        return nil
    }
}

public struct StatusMonitorList: Equatable, Sendable {
    public var monitors: [StatusMonitor]
    public var max: Int
    public var installed: Bool

    public init(monitors: [StatusMonitor], max: Int = AccountPackagesStore.defaultMonitorLimit, installed: Bool = true) {
        self.monitors = monitors
        self.max = max
        self.installed = installed
    }

    public init(json: JSONObject, fallbackMax: Int = AccountPackagesStore.defaultMonitorLimit) {
        self.init(
            monitors: json.maps("monitors").prefix(100).compactMap(StatusMonitor.init(json:)),
            max: json.int("max", fallbackMax), installed: json.optBool("installed") ?? true)
    }
}

extension PackagesClient {
    public func accountPackages() async throws -> AccountPackagesState {
        AccountPackagesState(json: try await call("GET", "/api/plugins/@account"))
    }

    public func installForAccount(_ packageId: String, places: [PackagePlace]? = nil) async throws -> AccountPackagesState {
        guard PackagesText.isPackageId(packageId) else { throw PackagesError("plugin_not_found", status: 404) }
        var body: JSONObject = [:]
        if let places, !places.isEmpty { body["places"] = PackagePlace.json(PackagePlace.ordered(places)) }
        return AccountPackagesState(json: try await call("POST", "/api/plugins/@account/\(packageId)/install", body: body))
    }

    public func setAccountPlaces(_ packageId: String, _ places: [PackagePlace]) async throws -> AccountPackagesState {
        guard PackagesText.isPackageId(packageId) else { throw PackagesError("plugin_not_found", status: 404) }
        guard !places.isEmpty else { throw PackagesError("invalid_places", status: 400) }
        return AccountPackagesState(
            json: try await call(
                "POST", "/api/plugins/@account/\(packageId)/places", body: ["places": PackagePlace.json(PackagePlace.ordered(places))]))
    }

    public func uninstallForAccount(_ packageId: String) async throws -> AccountPackagesState {
        guard PackagesText.isPackageId(packageId) else { throw PackagesError("plugin_not_found", status: 404) }
        return AccountPackagesState(json: try await call("POST", "/api/plugins/@account/\(packageId)/uninstall", body: [:]))
    }

    public func saveAccountSettings(_ packageId: String, _ settings: JSONObject) async throws -> AccountPackagesState {
        guard PackagesText.isPackageId(packageId) else { throw PackagesError("plugin_not_found", status: 404) }
        return AccountPackagesState(
            json: try await call("POST", "/api/plugins/@account/\(packageId)/settings", body: ["settings": .object(settings)]))
    }

    public func statusMonitors() async throws -> StatusMonitorList {
        StatusMonitorList(json: try await call("GET", "/api/status-monitors"))
    }

    public func addStatusMonitor(name: String, url: String) async throws -> StatusMonitorList {
        StatusMonitorList(
            json: try await call("POST", "/api/status-monitors/monitors", body: ["name": .string(name), "url": .string(url)]))
    }

    public func updateStatusMonitor(_ id: String, name: String, url: String) async throws -> StatusMonitorList {
        StatusMonitorList(
            json: try await call(
                "PUT", "/api/status-monitors/monitors/" + Self.item(id), body: ["name": .string(name), "url": .string(url)]))
    }

    public func deleteStatusMonitor(_ id: String) async throws -> StatusMonitorList {
        StatusMonitorList(json: try await call("DELETE", "/api/status-monitors/monitors/" + Self.item(id)))
    }
}

@MainActor
@Observable
public final class AccountPackagesStore {
    nonisolated public static let statusPage = "status-page"
    nonisolated public static let stateKey = "packages/account/state"
    nonisolated public static let catalogKey = "packages/account/catalog"
    nonisolated public static let monitorsKey = "packages/status-page/monitors"
    nonisolated public static let projectInstallsKey = "packages/project-installs"
    nonisolated public static let defaultMonitorLimit = 20
    nonisolated public static let defaultRefreshMinutes = 15

    public private(set) var state: AccountPackagesState?
    public private(set) var catalog: [AccountPackageInfo]
    public private(set) var monitors: [StatusMonitor]
    public private(set) var monitorLimit: Int = AccountPackagesStore.defaultMonitorLimit
    public private(set) var busy: Set<String> = []
    public private(set) var monitorsBusy = false
    public private(set) var errorText = ""
    public private(set) var monitorError = ""
    public private(set) var projectRevision = 0

    @ObservationIgnored public let client: PackagesClient
    @ObservationIgnored private let prefs: KeyValueStore
    @ObservationIgnored private let signedIn: () -> Bool
    @ObservationIgnored private let legacyProject: () -> Bool
    @ObservationIgnored private let projectNamespace: () -> String
    @ObservationIgnored private var choiceCache: (namespace: String, values: [String: Bool])?
    @ObservationIgnored private var catalogLoaded = false
    @ObservationIgnored private var refreshing = false
    @ObservationIgnored public var onChange: (() -> Void)?

    public init(
        client: PackagesClient, prefs: KeyValueStore, signedIn: @escaping () -> Bool,
        legacyProject: @escaping () -> Bool = { true }, projectNamespace: @escaping () -> String = { "" }
    ) {
        self.client = client
        self.prefs = prefs
        self.signedIn = signedIn
        self.legacyProject = legacyProject
        self.projectNamespace = projectNamespace
        let cachedState = JSON.parseObject(prefs.string(forKey: AccountPackagesStore.stateKey) ?? "")
        state = cachedState.contains("packages") ? AccountPackagesState(json: cachedState) : nil
        catalog = JSON.parseObject(prefs.string(forKey: AccountPackagesStore.catalogKey) ?? "").maps("packages")
            .compactMap(AccountPackageInfo.init(json:))
        monitors = JSON.parseObject(prefs.string(forKey: AccountPackagesStore.monitorsKey) ?? "").maps("monitors")
            .compactMap(StatusMonitor.init(json:))
    }

    public func info(_ id: String) -> AccountPackageInfo? { catalog.first { $0.id == id } }

    public func isAccountSynced(_ id: String) -> Bool { info(id) != nil || state?.entry(id) != nil }

    public func isInstalled(_ id: String) -> Bool { installed(id, depth: 0) }

    public var isLegacyProject: Bool { legacyProject() }

    public var ownsInstalls: Bool { !legacyProject() }

    public func fallbackInstalled(_ info: AccountPackageInfo) -> Bool {
        info.defaultOn || (!info.installedBefore.isEmpty && legacyProject())
    }

    public var projectChoices: [String: Bool] {
        _ = projectRevision
        let namespace = projectNamespace()
        if let cache = choiceCache, cache.namespace == namespace { return cache.values }
        let stored = JSON.parseObject(prefs.string(forKey: namespace + AccountPackagesStore.projectInstallsKey) ?? "")
        var values: [String: Bool] = [:]
        for (key, value) in stored where PackagesText.isPackageId(key) {
            if let on = value.boolValue { values[key] = on }
        }
        choiceCache = (namespace, values)
        return values
    }

    private func choose(_ ids: [String], installed: Bool) {
        guard !ids.isEmpty else { return }
        var values = projectChoices
        for id in ids { values[id] = installed }
        let namespace = projectNamespace()
        var saved = JSONObject()
        for (key, value) in values { saved[key] = .bool(value) }
        prefs.set(JSON.encode(saved), forKey: namespace + AccountPackagesStore.projectInstallsKey)
        choiceCache = (namespace, values)
        projectRevision &+= 1
        onChange?()
    }

    public func places(_ id: String) -> [PackagePlace] { state?.entry(id)?.places ?? PackagePlace.allCases }

    public func availablePlaces(_ id: String, fallback: [PackagePlace] = PackagePlace.allCases) -> [PackagePlace] {
        state?.entry(id)?.availablePlaces ?? fallback
    }

    public func isPlaced(_ id: String, on place: PackagePlace = PackagePlace.here) -> Bool {
        state?.entry(id)?.shows(on: place) ?? true
    }

    public func isShownHere(_ id: String) -> Bool { isInstalled(id) && isPlaced(id) }

    private func installed(_ id: String, depth: Int) -> Bool {
        let own: Bool
        let entry = state?.entry(id)
        if entry == nil && info(id) == nil {
            return PackageGate.isGated(id) ? legacyProject() : true
        }
        if ownsInstalls {
            let accountKeeps = entry?.installed ?? true
            switch projectChoices[id] {
            case .some(true): own = accountKeeps
            case .some(false): own = false
            case .none: own = (info(id)?.defaultOn ?? false) && accountKeeps
            }
        } else if let entry {
            own = entry.installed
        } else if let info = info(id) {
            own = fallbackInstalled(info)
        } else {
            own = false
        }
        if !own { return false }
        if depth > 8 { return true }
        return (info(id)?.dependencies ?? []).allSatisfy { installed($0, depth: depth + 1) }
    }

    public var installedGatedPackages: Set<String> {
        Set(PackageGate.packages.filter { !PackageGate.organizationPackages.contains($0) && isShownHere($0) })
    }

    public func isBusy(_ id: String) -> Bool { busy.contains(id) }

    public var statusPageOn: Bool { isInstalled(AccountPackagesStore.statusPage) }

    public var statusPageShown: Bool { isShownHere(AccountPackagesStore.statusPage) }

    public func settings(_ id: String) -> JSONObject {
        let declared = info(id)?.settings ?? []
        var out = JSONObject()
        for setting in declared { out[setting.key] = setting.defaultValue }
        for (key, value) in state?.entry(id)?.settings ?? [:] {
            if let setting = declared.first(where: { $0.key == key }), !setting.accepts(value) { continue }
            out[key] = value
        }
        return out
    }

    public var onlyProblems: Bool { settings(AccountPackagesStore.statusPage).optBool("onlyProblems") ?? false }

    public var refreshMinutes: Int {
        let minutes = Int(settings(AccountPackagesStore.statusPage).str("refreshMinutes")) ?? AccountPackagesStore.defaultRefreshMinutes
        return min(max(minutes, 1), 24 * 60)
    }

    public var monitorRules: PackageMonitors? {
        let watchers = catalog.filter { !$0.monitors.isEmpty }
        if watchers.isEmpty { return nil }
        var rules = PackageMonitors()
        for info in watchers where isInstalled(info.id) {
            for system in info.monitors.systems where !rules.systems.contains(system) { rules.systems.append(system) }
            for prefix in info.monitors.prefixes where !rules.prefixes.contains(prefix) { rules.prefixes.append(prefix) }
        }
        return rules
    }

    public func dependents(of id: String) -> [AccountPackageInfo] {
        var found: [String] = []
        var queue: [String] = [id]
        while let next = queue.popLast() {
            for info in catalog where info.id != id && !found.contains(info.id) && info.dependencies.contains(next) {
                found.append(info.id)
                queue.append(info.id)
            }
        }
        return catalog.filter { found.contains($0.id) && isInstalled($0.id) }
    }

    public func missingDependencies(of id: String) -> [AccountPackageInfo] {
        var found: [String] = []
        var queue: [String] = info(id)?.dependencies ?? []
        while let next = queue.popLast() {
            if next == id || found.contains(next) { continue }
            found.append(next)
            queue.append(contentsOf: info(next)?.dependencies ?? [])
        }
        return found.filter { !isInstalled($0) }.map { info($0) ?? AccountPackageInfo(id: $0) }
    }

    public func adoptCatalog(_ packages: [MarketPackage]) {
        guard !packages.isEmpty else { return }
        catalog = packages.filter(\.accountSync).map(AccountPackageInfo.init(package:))
        catalogLoaded = true
        let saved: JSONObject = ["packages": .array(catalog.map { .object($0.json) })]
        prefs.set(JSON.encode(saved), forKey: AccountPackagesStore.catalogKey)
        onChange?()
    }

    public func refresh(catalog forceCatalog: Bool = false) async {
        if refreshing { return }
        refreshing = true
        defer { refreshing = false }
        if forceCatalog || !catalogLoaded, let packages = try? await client.packages() {
            adoptCatalog(packages)
        }
        guard signedIn() else { return }
        do {
            adopt(try await client.accountPackages())
        } catch {
            return
        }
        await syncMonitors()
    }

    @discardableResult
    public func install(_ id: String) async -> [String]? {
        let brought = ownsInstalls ? [id] + missingDependencies(of: id).map(\.id) : []
        let changed = await change(id) { try await self.client.installForAccount(id) }
        if changed != nil { choose(brought, installed: true) }
        return changed
    }

    @discardableResult
    public func uninstall(_ id: String, everywhere: Bool = false) async -> [String]? {
        guard ownsInstalls else { return await change(id) { try await self.client.uninstallForAccount(id) } }
        let going = [id] + dependents(of: id).map(\.id)
        if everywhere {
            guard await change(id, { try await self.client.uninstallForAccount(id) }) != nil else { return nil }
        }
        choose(going, installed: false)
        errorText = ""
        await syncMonitors()
        return going
    }

    @discardableResult
    public func setPlaces(_ id: String, _ places: [PackagePlace]) async -> Bool {
        let wanted = PackagePlace.ordered(places).filter(availablePlaces(id).contains)
        guard !wanted.isEmpty else {
            errorText = PackagesError.sentence("invalid_places")
            return false
        }
        guard isInstalled(id) else {
            errorText = PackagesError.sentence("plugin_not_installed_for_account")
            return false
        }
        if wanted == self.places(id) { return true }
        return await change(id) {
            do {
                return try await self.client.setAccountPlaces(id, wanted)
            } catch let error as PackagesError where error.code == "plugin_not_installed" {
                throw PackagesError("plugin_not_installed_for_account", status: error.status)
            }
        } != nil
    }

    @discardableResult
    public func setPlace(_ id: String, _ place: PackagePlace, on: Bool) async -> Bool {
        var next = places(id)
        if on {
            if !next.contains(place) { next.append(place) }
        } else {
            next.removeAll { $0 == place }
        }
        return await setPlaces(id, next)
    }

    private func change(_ id: String, _ call: () async throws -> AccountPackagesState) async -> [String]? {
        guard !busy.contains(id) else { return nil }
        busy.insert(id)
        defer { busy.remove(id) }
        do {
            let next = try await call()
            adopt(next)
            errorText = ""
            await syncMonitors()
            return next.changed
        } catch {
            errorText = PackagesError.describe(error)
            return nil
        }
    }

    @discardableResult
    public func setSetting(_ id: String, key: String, value: JSONValue) async -> Bool {
        if let setting = info(id)?.settings.first(where: { $0.key == key }), !setting.accepts(value) { return false }
        let before = state
        var next = state ?? AccountPackagesState(packages: [])
        if let index = next.packages.firstIndex(where: { $0.id == id }) {
            next.packages[index].settings[key] = value
        } else {
            next.packages.append(AccountPackageEntry(id: id, installed: isInstalled(id), settings: [key: value]))
        }
        state = next
        busy.insert(id)
        defer { busy.remove(id) }
        do {
            adopt(try await client.saveAccountSettings(id, [key: value]))
            errorText = ""
            return true
        } catch {
            state = before
            errorText = PackagesError.describe(error)
            return false
        }
    }

    private func adopt(_ next: AccountPackagesState) {
        state = AccountPackagesState(packages: next.packages)
        prefs.set(JSON.encode(next.json), forKey: AccountPackagesStore.stateKey)
        onChange?()
    }

    private func syncMonitors() async {
        if statusPageOn {
            await refreshMonitors()
        } else if !monitors.isEmpty {
            setMonitors(StatusMonitorList(monitors: [], max: monitorLimit))
        }
    }

    public func refreshMonitors() async {
        guard signedIn() else { return }
        do {
            setMonitors(try await client.statusMonitors())
            monitorError = ""
        } catch let error as PackagesError where error.code == "status_page_not_installed" {
            setMonitors(StatusMonitorList(monitors: [], max: monitorLimit))
        } catch {
            return
        }
    }

    @discardableResult
    public func addMonitor(name: String, url: String) async -> Bool {
        let name = name.trimmingCharacters(in: .whitespacesAndNewlines)
        let url = url.trimmingCharacters(in: .whitespacesAndNewlines)
        if let problem = StatusMonitor.problem(name: name, url: url) {
            monitorError = PackagesError.sentence(problem)
            return false
        }
        return await editMonitors { try await self.client.addStatusMonitor(name: name, url: url) }
    }

    @discardableResult
    public func updateMonitor(_ id: String, name: String, url: String) async -> Bool {
        let name = name.trimmingCharacters(in: .whitespacesAndNewlines)
        let url = url.trimmingCharacters(in: .whitespacesAndNewlines)
        if let problem = StatusMonitor.problem(name: name, url: url) {
            monitorError = PackagesError.sentence(problem)
            return false
        }
        return await editMonitors { try await self.client.updateStatusMonitor(id, name: name, url: url) }
    }

    @discardableResult
    public func removeMonitor(_ id: String) async -> Bool {
        await editMonitors { try await self.client.deleteStatusMonitor(id) }
    }

    private func editMonitors(_ call: () async throws -> StatusMonitorList) async -> Bool {
        guard !monitorsBusy else { return false }
        monitorsBusy = true
        defer { monitorsBusy = false }
        do {
            setMonitors(try await call())
            monitorError = ""
            return true
        } catch {
            monitorError = PackagesError.describe(error)
            return false
        }
    }

    private func setMonitors(_ list: StatusMonitorList) {
        monitors = list.installed ? list.monitors : []
        monitorLimit = list.max
        let saved: JSONObject = ["monitors": .array(monitors.map { .object($0.json) })]
        prefs.set(JSON.encode(saved), forKey: AccountPackagesStore.monitorsKey)
    }
}
