import Foundation
import NexusCore
import NexusDNS
import NexusConnectivity
#if canImport(Network)
import Network
#endif

public struct DiscoveryOptions: Sendable, Hashable {
    public var serviceTypes: [String]
    public var duration: Duration
    public var resolveAddresses: Bool

    public init(serviceTypes: [String] = ["_http._tcp", "_https._tcp", "_ssh._tcp", "_smb._tcp", "_airplay._tcp", "_raop._tcp", "_ipp._tcp", "_homekit._tcp"], duration: Duration = .seconds(8), resolveAddresses: Bool = true) {
        self.serviceTypes = serviceTypes
        self.duration = duration
        self.resolveAddresses = resolveAddresses
    }
}

public struct DiscoveryProgress: Sendable, Hashable {
    public var serviceType: String
    public var devices: [Device]
    public var completedServiceTypes: Int
    public var totalServiceTypes: Int

    public init(serviceType: String, devices: [Device], completedServiceTypes: Int, totalServiceTypes: Int) {
        self.serviceType = serviceType
        self.devices = devices
        self.completedServiceTypes = completedServiceTypes
        self.totalServiceTypes = totalServiceTypes
    }
}

public protocol BonjourBrowsing: Sendable {
    func browse(type: String, duration: Duration) async throws -> [BonjourService]
}

public struct DiscoveryEngine: Sendable {
    public let browser: any BonjourBrowsing

    public init(browser: any BonjourBrowsing = BonjourBrowser()) { self.browser = browser }

    public func discover(options: DiscoveryOptions = .init()) -> AsyncThrowingStream<DiscoveryProgress, Error> {
        AsyncThrowingStream { continuation in
            let task = Task {
                var inventory = DeviceInventory()
                do {
                    for (index, type) in options.serviceTypes.enumerated() {
                        try Task.checkCancellation()
                        let services = try await browser.browse(type: type, duration: options.duration)
                        for service in services { inventory.merge(service: service) }
                        continuation.yield(DiscoveryProgress(serviceType: type, devices: inventory.devices, completedServiceTypes: index + 1, totalServiceTypes: options.serviceTypes.count))
                    }
                    continuation.finish()
                } catch is CancellationError {
                    continuation.finish()
                } catch {
                    continuation.finish(throwing: error)
                }
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    public func session(options: DiscoveryOptions = .init(), progress: @escaping @Sendable (DiscoveryProgress) async -> Void) async throws -> DiscoverySession {
        var result = DiscoverySession(methods: [.bonjour], serviceTypes: options.serviceTypes)
        let stream = discover(options: options)
        do {
            for try await update in stream {
                result.devices = update.devices
                await progress(update)
            }
            result.status = Task.isCancelled ? .cancelled : .completed
        } catch {
            result.status = .failed(String(describing: error))
            throw error
        }
        result.finishedAt = Date()
        return result
    }
}

public struct BonjourBrowser: BonjourBrowsing {
    public init() {}

    public func browse(type: String, duration: Duration) async throws -> [BonjourService] {
        #if canImport(Network)
        return try await withCheckedThrowingContinuation { continuation in
            let browser = NWBrowser(for: .bonjour(type: type, domain: nil), using: .tcp)
            let state = Locked(Set<BonjourService>())
            let completed = OnceFlag()
            browser.browseResultsChangedHandler = { results, _ in
                state.withLock { found in
                    found = Set(results.compactMap { result in
                        guard case let .service(name, serviceType, domain, interface) = result.endpoint else { return nil }
                        return BonjourService(name: name, type: serviceType, domain: domain, interfaceName: interface?.name)
                    })
                }
            }
            browser.stateUpdateHandler = { value in
                if case .failed(let error) = value, completed.claim() { browser.cancel(); continuation.resume(throwing: ConnectionFailure.system(code: 0, message: error.debugDescription)) }
            }
            browser.start(queue: .global(qos: .utility))
            Task {
                do { try await Task.sleep(for: duration) } catch { }
                if completed.claim() { browser.cancel(); continuation.resume(returning: state.current.sorted { $0.id < $1.id }) }
            }
        }
        #else
        throw ConnectionFailure.unsupported("Bonjour browsing requires Network.framework on this platform.")
        #endif
    }
}

public struct DeviceInventory: Sendable {
    public private(set) var devices: [Device] = []

    public init(devices: [Device] = []) { self.devices = devices }

    public mutating func merge(service: BonjourService) {
        let matching = devices.indices.first { index in
            let device = devices[index]
            return !Set(device.addresses).isDisjoint(with: service.addresses) || device.hostnames.contains(service.hostName ?? "") || device.services.contains(where: { $0.id == service.id })
        }
        if let index = matching {
            devices[index].services.removeAll { $0.id == service.id }
            devices[index].services.append(service)
            devices[index].addresses = Array(Set(devices[index].addresses + service.addresses)).sorted()
            if let host = service.hostName, !devices[index].hostnames.contains(host) { devices[index].hostnames.append(host) }
            if !devices[index].sources.contains(.bonjour) { devices[index].sources.append(.bonjour) }
            devices[index].lastSeen = Date()
        } else {
            let address = service.addresses.first
            let id = address.map(Device.key(for:)) ?? "bonjour:\(service.id)"
            devices.append(Device(id: id, displayName: service.name, addresses: service.addresses, hostnames: service.hostName.map { [$0] } ?? [], services: [service], sources: [.bonjour]))
        }
    }

    public func changes(since previous: [Device]) -> [ObservedChange] {
        let old = Dictionary(uniqueKeysWithValues: previous.map { ($0.id, $0) })
        let fresh = Dictionary(uniqueKeysWithValues: devices.map { ($0.id, $0) })
        var changes = devices.filter { old[$0.id] == nil }.map { ObservedChange(kind: .deviceAppeared, deviceID: $0.id, subject: $0.title) }
        changes += previous.filter { fresh[$0.id] == nil }.map { ObservedChange(kind: .deviceDisappeared, deviceID: $0.id, subject: $0.title) }
        for device in devices {
            guard let prior = old[device.id] else { continue }
            let before = Set(prior.services.map(\.id)); let after = Set(device.services.map(\.id))
            changes += after.subtracting(before).map { ObservedChange(kind: .serviceAdded, deviceID: device.id, subject: device.title, after: $0) }
            changes += before.subtracting(after).map { ObservedChange(kind: .serviceRemoved, deviceID: device.id, subject: device.title, before: $0) }
        }
        return changes.sorted { $0.subject < $1.subject }
    }
}
