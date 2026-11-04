import FMAccount
import FMBase
import FMPackages
import Foundation
import Network
import UIKit
import UserNotifications

final class ReticulumNetworkLink: ReticulumLink {
    private let connection: NWConnection
    private let queue = DispatchQueue(label: "com.forkmesh.reticulum.link")
    private var handler: ((ReticulumLinkEvent) -> Void)?
    private var ready = false
    private var closed = false

    init(host: String, port: Int) {
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        tcp.enableKeepalive = true
        tcp.connectionTimeout = 10
        let endpointPort = NWEndpoint.Port(rawValue: UInt16(clamping: port)) ?? 4242
        connection = NWConnection(host: NWEndpoint.Host(host), port: endpointPort, using: NWParameters(tls: nil, tcp: tcp))
    }

    func open(_ handler: @escaping (ReticulumLinkEvent) -> Void) {
        self.handler = handler
        connection.stateUpdateHandler = { [weak self] state in
            DispatchQueue.main.async { self?.stateChanged(state) }
        }
        connection.start(queue: queue)
    }

    private func stateChanged(_ state: NWConnection.State) {
        guard !closed else { return }
        switch state {
        case .ready:
            guard !ready else { return }
            ready = true
            handler?(.connected)
            receive()
        case .waiting(let error):
            if !ready { finish(.failed(Self.describe(error))) }
        case .failed(let error):
            finish(ready ? .closed : .failed(Self.describe(error)))
        default:
            break
        }
    }

    private func receive() {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 65_536) { [weak self] data, _, complete, error in
            DispatchQueue.main.async {
                guard let self, !self.closed else { return }
                if let data, !data.isEmpty { self.handler?(.received([UInt8](data))) }
                if complete || error != nil {
                    self.finish(.closed)
                } else {
                    self.receive()
                }
            }
        }
    }

    private func finish(_ event: ReticulumLinkEvent) {
        guard !closed else { return }
        closed = true
        let last = handler
        handler = nil
        connection.stateUpdateHandler = nil
        connection.cancel()
        last?(event)
    }

    func write(_ bytes: [UInt8]) -> Int {
        guard ready, !closed else { return -1 }
        connection.send(content: Data(bytes), completion: .contentProcessed { [weak self] error in
            guard error != nil else { return }
            DispatchQueue.main.async { self?.finish(.closed) }
        })
        return bytes.count
    }

    func close() {
        guard !closed else { return }
        closed = true
        handler = nil
        connection.stateUpdateHandler = nil
        connection.cancel()
    }

    static func describe(_ error: NWError) -> String {
        switch error {
        case .posix(let code):
            switch code {
            case .ECONNREFUSED: return "Connection refused"
            case .ETIMEDOUT: return "Connection timed out"
            case .ENETUNREACH, .EHOSTUNREACH: return "Network unreachable"
            case .ENETDOWN: return "The network is down"
            default: return String(cString: strerror(code.rawValue))
            }
        case .dns: return "Host not found"
        default: return error.localizedDescription
        }
    }
}

final class ReticulumNetworkConnector: ReticulumConnector {
    private let device: String

    init(device: String) { self.device = device }

    func tcp(host: String, port: Int) -> ReticulumLink? { ReticulumNetworkLink(host: host, port: port) }

    func unavailableReason(_ kind: ReticulumInterfaceKind) -> String? {
        switch kind {
        case .tcp: return nil
        case .shared: return "There is no Reticulum daemon (rnsd) on \(device). Connect to a node over TCP instead."
        case .udp: return "UDP interfaces are not available on \(device)."
        }
    }
}

@MainActor
enum ReticulumAppState {
    static let hub: ReticulumHub = {
        let device = UIDevice.current.userInterfaceIdiom == .pad ? "iPad" : "iPhone"
        let manager = FileManager.default
        let folder = manager.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            .map { AppProjectScope.router.scope.directory($0).appendingPathComponent("reticulum", isDirectory: true) }
            ?? manager.temporaryDirectory.appendingPathComponent("reticulum", isDirectory: true)
        let hub = ReticulumHub(
            prefs: Services.get(KeyValueStore.self) ?? UserDefaultsStore(),
            secure: Services.get(SecureStore.self) ?? KeychainStore(),
            connector: ReticulumNetworkConnector(device: device),
            scheduler: ReticulumDispatchScheduler(),
            accountName: Services.get(AuthService.self)?.session?.nodeName ?? "",
            device: device,
            disk: RSSReaderFileDisk(directory: folder))
        hub.onIncomingMessage = { message, name in
            Task { await notify(name: name, text: String(message.content.prefix(200))) }
        }
        return hub
    }()

    private static func notify(name: String, text: String) async {
        let center = UNUserNotificationCenter.current()
        let settings = await center.notificationSettings()
        guard settings.authorizationStatus == .authorized || settings.authorizationStatus == .provisional else { return }
        let content = UNMutableNotificationContent()
        content.title = "Reticulum message from \(name)"
        content.body = text
        content.sound = .default
        content.threadIdentifier = "reticulum"
        try? await center.add(UNNotificationRequest(identifier: "reticulum-\(UUID().uuidString)", content: content, trigger: nil))
    }
}
