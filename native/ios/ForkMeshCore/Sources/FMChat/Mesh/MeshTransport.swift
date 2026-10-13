// The parts of the local mesh room that need the platform underneath: Bluetooth links, and the
// Wi-Fi multicast lock Android requires.
//
// The room's frames, keys and history are platform-independent, so the platform does one job:
// carry opaque byte frames between this device and another ForkMesh device under the service UUID
// the desktop advertises. Everything that could leak anything is encrypted before it gets here.

import Foundation

public let meshBluetoothServiceUuid = "7f9e5c10-4d3b-4a21-9c6f-2b7ad1e05f11"
public let meshBluetoothServiceName = "ForkMesh local mesh"

public enum MeshBluetoothEventKind: Sendable {
    case linkOpened, data, linkClosed
}

public struct MeshBluetoothEvent: Sendable {
    public let kind: MeshBluetoothEventKind
    public let linkId: Int
    public let outbound: Bool
    public let bytes: Data

    public init(kind: MeshBluetoothEventKind, linkId: Int, outbound: Bool = false, bytes: Data = Data()) {
        self.kind = kind
        self.linkId = linkId
        self.outbound = outbound
        self.bytes = bytes
    }

    /// The platform channel's map shape (`{type: link|data|closed, id, outbound, bytes}`), for a
    /// native bridge that speaks it; nil for anything else.
    public static func fromNative(_ raw: [String: Any]) -> MeshBluetoothEvent? {
        let linkId: Int
        if let id = raw["id"] as? Int {
            linkId = id
        } else if let text = raw["id"].map({ "\($0)" }), let parsed = Int(text) {
            linkId = parsed
        } else {
            return nil
        }
        if linkId < 0 { return nil }
        switch "\(raw["type"] ?? "")" {
        case "link":
            return MeshBluetoothEvent(kind: .linkOpened, linkId: linkId, outbound: raw["outbound"] as? Bool == true)
        case "data":
            guard let bytes = raw["bytes"] as? Data else { return nil }
            return MeshBluetoothEvent(kind: .data, linkId: linkId, bytes: bytes)
        case "closed":
            return MeshBluetoothEvent(kind: .linkClosed, linkId: linkId)
        default:
            return nil
        }
    }
}

/// Bluetooth links and the multicast lock (`MeshNativeTransport`).
@MainActor
public protocol MeshNativeTransport: AnyObject {
    var listening: Bool { get }
    /// True once the platform said it has no Bluetooth mesh support.
    var unavailable: Bool { get }
    /// Link events, in order. One consumer, for the transport's lifetime.
    var events: AsyncStream<MeshBluetoothEvent> { get }
    /// Starts advertising and looking for peers. False (never a throw) when there is no support,
    /// no adapter, Bluetooth is off, or the permission was declined.
    func startBluetooth() async -> Bool
    func stopBluetooth() async
    func sendBluetooth(_ linkId: Int, _ frame: Data) async -> Bool
    func closeBluetoothLink(_ linkId: Int) async
    func closeBluetoothLink(_ linkId: Int, duplicate: Bool) async
    func roomOpened() async
    func rescanBluetoothSoon() async
    func liftBluetoothHoldOff(_ linkId: Int) async
    /// Android drops multicast unless a lock is held. A no-op elsewhere.
    func acquireMulticastLock() async -> Bool
    func releaseMulticastLock() async
}

extension MeshNativeTransport {
    public func closeBluetoothLink(_ linkId: Int, duplicate: Bool) async {
        await closeBluetoothLink(linkId)
    }

    public func roomOpened() async {}

    public func rescanBluetoothSoon() async {}

    public func liftBluetoothHoldOff(_ linkId: Int) async {}
}

@MainActor
public final class UnavailableMeshTransport: MeshNativeTransport {
    public let events: AsyncStream<MeshBluetoothEvent>
    public private(set) var unavailable = false

    public init() {
        events = AsyncStream { $0.finish() }
    }

    public var listening: Bool { false }

    public func startBluetooth() async -> Bool {
        unavailable = true
        return false
    }

    public func stopBluetooth() async {}

    public func sendBluetooth(_ linkId: Int, _ frame: Data) async -> Bool { false }

    public func closeBluetoothLink(_ linkId: Int) async {}

    public func acquireMulticastLock() async -> Bool { false }

    public func releaseMulticastLock() async {}
}
