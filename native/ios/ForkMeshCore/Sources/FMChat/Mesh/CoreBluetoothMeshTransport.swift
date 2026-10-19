#if canImport(CoreBluetooth)
import CoreBluetooth
import Foundation

@MainActor
public final class CoreBluetoothMeshTransport: NSObject, MeshNativeTransport {
    public let events: AsyncStream<MeshBluetoothEvent>
    public private(set) var unavailable = false
    public private(set) var listening = false

    private let continuation: AsyncStream<MeshBluetoothEvent>.Continuation
    private let token = MeshBle.newToken()
    private let serviceId = CBUUID(string: meshBleServiceUuid)
    private let inboxId = CBUUID(string: meshBleInboxUuid)
    private let outboxId = CBUUID(string: meshBleOutboxUuid)

    private var centralManager: CBCentralManager?
    private var peripheralManager: CBPeripheralManager?
    private var outbox: CBMutableCharacteristic?
    private var serviceAdded = false
    private var serviceAdding = false
    private var rescanTimer: Timer?
    private var nextLinkId = 1
    private var promptAllowed = false
    private var callInProgress = false

    private final class DialedLink {
        let id: Int
        let peripheral: CBPeripheral
        var inbox: CBCharacteristic?
        var open = false
        var writing = false
        var queue = MeshBleOutbox()

        init(id: Int, peripheral: CBPeripheral) {
            self.id = id
            self.peripheral = peripheral
        }
    }

    private final class SubscribedLink {
        let id: Int
        let central: CBCentral
        var queue = MeshBleOutbox()

        init(id: Int, central: CBCentral) {
            self.id = id
            self.central = central
        }
    }

    private var dialed: [UUID: DialedLink] = [:]
    private var subscribed: [UUID: SubscribedLink] = [:]
    private var closedCentrals: Set<UUID> = []
    private var quietUntil: [UUID: Date] = [:]
    private var heldOff: [Int: UUID] = [:]

    public override init() {
        var cont: AsyncStream<MeshBluetoothEvent>.Continuation!
        events = AsyncStream { cont = $0 }
        continuation = cont
        super.init()
    }

    public func startBluetooth() async -> Bool {
        if listening { return true }
        if unavailable { return false }
        switch CBManager.authorization {
        case .denied, .restricted:
            return false
        case .notDetermined:
            if !promptAllowed { return false }
        default:
            break
        }
        if centralManager == nil {
            centralManager = CBCentralManager(
                delegate: self, queue: nil, options: [CBCentralManagerOptionShowPowerAlertKey: false])
        }
        if peripheralManager == nil {
            peripheralManager = CBPeripheralManager(
                delegate: self, queue: nil, options: [CBPeripheralManagerOptionShowPowerAlertKey: false])
        }
        for _ in 0..<50 {
            if settled(centralManager?.state) && settled(peripheralManager?.state) { break }
            try? await Task.sleep(nanoseconds: 100_000_000)
        }
        if listening { return true }
        let centralState = centralManager?.state ?? .unknown
        let peripheralState = peripheralManager?.state ?? .unknown
        if centralState == .unsupported && peripheralState == .unsupported {
            unavailable = true
            return false
        }
        if centralState != .poweredOn && peripheralState != .poweredOn { return false }
        listening = true
        advertise()
        scan()
        rescanTimer?.invalidate()
        rescanTimer = Timer.scheduledTimer(withTimeInterval: MeshBle.rescanSeconds, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.rescan() }
        }
        return true
    }

    public func stopBluetooth() async {
        shutDown()
        quietUntil.removeAll()
        heldOff.removeAll()
    }

    public func sendBluetooth(_ linkId: Int, _ frame: Data) async -> Bool {
        if frame.isEmpty { return true }
        if let link = dialed.values.first(where: { $0.id == linkId && $0.open }) {
            link.queue.enqueue(frame)
            pump(link)
            return true
        }
        if let link = subscribed.values.first(where: { $0.id == linkId }) {
            link.queue.enqueue(frame)
            pump(link)
            return true
        }
        return false
    }

    public func closeBluetoothLink(_ linkId: Int) async {
        await closeBluetoothLink(linkId, duplicate: false)
    }

    public func closeBluetoothLink(_ linkId: Int, duplicate: Bool) async {
        if let entry = dialed.first(where: { $0.value.id == linkId }) {
            if duplicate {
                quietUntil[entry.key] = Date().addingTimeInterval(MeshBle.quietAfterDuplicateSeconds)
                heldOff[linkId] = entry.key
                scheduleRescan(after: MeshBle.quietAfterDuplicateSeconds + 1)
            }
            dropDialed(entry.key, entry.value, cancel: true)
            return
        }
        if let entry = subscribed.first(where: { $0.value.id == linkId }) {
            if let outbox {
                _ = peripheralManager?.updateValue(Data(), for: outbox, onSubscribedCentrals: [entry.value.central])
            }
            closedCentrals.insert(entry.key)
            subscribed[entry.key] = nil
            continuation.yield(MeshBluetoothEvent(kind: .linkClosed, linkId: entry.value.id))
        }
    }

    public func setCallInProgress(_ on: Bool) {
        guard on != callInProgress else { return }
        callInProgress = on
        for link in subscribed.values {
            peripheralManager?.setDesiredConnectionLatency(linkLatency, for: link.central)
        }
        if on {
            if centralManager?.state == .poweredOn { centralManager?.stopScan() }
        } else {
            rescan()
        }
    }

    private var linkLatency: CBPeripheralManagerConnectionLatency { callInProgress ? .high : .low }

    public func roomOpened() async {
        promptAllowed = true
    }

    public func rescanBluetoothSoon() async {
        scheduleRescan(after: MeshBle.rescanAfterDropSeconds)
    }

    public func liftBluetoothHoldOff(_ linkId: Int) async {
        guard let key = heldOff.removeValue(forKey: linkId) else { return }
        quietUntil[key] = nil
        rescan()
    }

    public func acquireMulticastLock() async -> Bool { false }

    public func releaseMulticastLock() async {}

    private func settled(_ state: CBManagerState?) -> Bool {
        guard let state else { return true }
        return state != .unknown && state != .resetting
    }

    private func takeLinkId() -> Int {
        let id = nextLinkId
        nextLinkId += 1
        return id
    }

    private func shutDown() {
        listening = false
        rescanTimer?.invalidate()
        rescanTimer = nil
        if centralManager?.state == .poweredOn { centralManager?.stopScan() }
        for (key, link) in dialed { dropDialed(key, link, cancel: true) }
        for (_, link) in subscribed {
            if let outbox, peripheralManager?.state == .poweredOn {
                _ = peripheralManager?.updateValue(Data(), for: outbox, onSubscribedCentrals: [link.central])
            }
            continuation.yield(MeshBluetoothEvent(kind: .linkClosed, linkId: link.id))
        }
        subscribed.removeAll()
        closedCentrals.removeAll()
        if peripheralManager?.state == .poweredOn {
            peripheralManager?.stopAdvertising()
            peripheralManager?.removeAllServices()
        }
        outbox = nil
        serviceAdded = false
        serviceAdding = false
    }

    private func advertise() {
        guard listening, let manager = peripheralManager, manager.state == .poweredOn else { return }
        if !serviceAdded {
            if serviceAdding { return }
            let inbox = CBMutableCharacteristic(
                type: inboxId, properties: [.write], value: nil, permissions: [.writeable])
            let out = CBMutableCharacteristic(
                type: outboxId, properties: [.notify], value: nil, permissions: [.readable])
            let service = CBMutableService(type: serviceId, primary: true)
            service.characteristics = [inbox, out]
            outbox = out
            serviceAdding = true
            manager.add(service)
            return
        }
        if manager.isAdvertising { return }
        manager.startAdvertising([
            CBAdvertisementDataServiceUUIDsKey: [serviceId],
            CBAdvertisementDataLocalNameKey: MeshBle.advertisedName(token: token),
        ])
    }

    private func scan() {
        guard listening, !callInProgress, let manager = centralManager, manager.state == .poweredOn else { return }
        manager.scanForPeripherals(
            withServices: [serviceId], options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
    }

    private func rescan() {
        guard listening else { return }
        if centralManager?.state == .poweredOn { centralManager?.stopScan() }
        scan()
        advertise()
    }

    private func scheduleRescan(after seconds: TimeInterval) {
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
            self?.rescan()
        }
    }

    private func dropDialed(_ key: UUID, _ link: DialedLink, cancel: Bool) {
        dialed[key] = nil
        if cancel, centralManager?.state == .poweredOn {
            if link.open, let outbox = link.peripheral.services?
                .first(where: { $0.uuid == serviceId })?.characteristics?
                .first(where: { $0.uuid == outboxId }) {
                link.peripheral.setNotifyValue(false, for: outbox)
            }
            centralManager?.cancelPeripheralConnection(link.peripheral)
        }
        if link.open {
            link.open = false
            continuation.yield(MeshBluetoothEvent(kind: .linkClosed, linkId: link.id))
        }
    }

    private func pump(_ link: DialedLink) {
        guard link.open, !link.writing, let inbox = link.inbox else { return }
        let size = MeshBle.chunkBytes(link.peripheral.maximumWriteValueLength(for: .withoutResponse))
        guard let chunk = link.queue.peek(max: size) else { return }
        link.writing = true
        link.peripheral.writeValue(chunk, for: inbox, type: .withResponse)
    }

    private func pump(_ link: SubscribedLink) {
        guard let outbox, let manager = peripheralManager else { return }
        let size = MeshBle.chunkBytes(link.central.maximumUpdateValueLength)
        while let chunk = link.queue.peek(max: size) {
            if !manager.updateValue(chunk, for: outbox, onSubscribedCentrals: [link.central]) { return }
            link.queue.advance()
        }
    }

    private func dialedLink(_ peripheral: CBPeripheral) -> DialedLink? {
        guard let link = dialed[peripheral.identifier], link.peripheral === peripheral else { return nil }
        return link
    }

    private func subscribedLink(for central: CBCentral) -> SubscribedLink {
        if let link = subscribed[central.identifier] { return link }
        let link = SubscribedLink(id: takeLinkId(), central: central)
        subscribed[central.identifier] = link
        peripheralManager?.setDesiredConnectionLatency(linkLatency, for: central)
        continuation.yield(MeshBluetoothEvent(kind: .linkOpened, linkId: link.id, outbound: false))
        return link
    }

    fileprivate func centralStateChanged() {
        guard let manager = centralManager else { return }
        if manager.state == .poweredOn {
            scan()
            return
        }
        for (key, link) in dialed { dropDialed(key, link, cancel: false) }
        if manager.state == .unsupported && peripheralManager?.state == .unsupported { unavailable = true }
        if manager.state != .unknown && manager.state != .resetting && peripheralManager?.state != .poweredOn {
            shutDown()
        }
    }

    fileprivate func peripheralStateChanged() {
        guard let manager = peripheralManager else { return }
        if manager.state == .poweredOn {
            advertise()
            return
        }
        for (_, link) in subscribed {
            continuation.yield(MeshBluetoothEvent(kind: .linkClosed, linkId: link.id))
        }
        subscribed.removeAll()
        closedCentrals.removeAll()
        outbox = nil
        serviceAdded = false
        serviceAdding = false
        if manager.state != .unknown && manager.state != .resetting && centralManager?.state != .poweredOn {
            shutDown()
        }
    }

    fileprivate func discovered(_ peripheral: CBPeripheral, name: String?) {
        guard listening, let manager = centralManager else { return }
        let key = peripheral.identifier
        if dialed[key] != nil || subscribed[key] != nil { return }
        if let quiet = quietUntil[key], quiet > Date() { return }
        if !MeshBle.shouldDial(ownToken: token, peerName: name) { return }
        let link = DialedLink(id: takeLinkId(), peripheral: peripheral)
        dialed[key] = link
        peripheral.delegate = self
        manager.connect(peripheral, options: nil)
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(MeshBle.openTimeoutSeconds * 1_000_000_000))
            guard let self, let current = self.dialed[key], current === link, !link.open else { return }
            self.dropDialed(key, link, cancel: true)
        }
    }

    fileprivate func connected(_ peripheral: CBPeripheral) {
        guard dialedLink(peripheral) != nil else {
            centralManager?.cancelPeripheralConnection(peripheral)
            return
        }
        peripheral.discoverServices([serviceId])
    }

    fileprivate func disconnected(_ peripheral: CBPeripheral) {
        guard let link = dialedLink(peripheral) else { return }
        dropDialed(peripheral.identifier, link, cancel: false)
    }

    fileprivate func servicesFound(_ peripheral: CBPeripheral, failed: Bool) {
        guard let link = dialedLink(peripheral) else { return }
        guard !failed, let service = peripheral.services?.first(where: { $0.uuid == serviceId }) else {
            dropDialed(peripheral.identifier, link, cancel: true)
            return
        }
        peripheral.discoverCharacteristics([inboxId, outboxId], for: service)
    }

    fileprivate func characteristicsFound(_ peripheral: CBPeripheral, service: CBService, failed: Bool) {
        guard let link = dialedLink(peripheral), service.uuid == serviceId else { return }
        let inbox = service.characteristics?.first { $0.uuid == inboxId }
        let out = service.characteristics?.first { $0.uuid == outboxId }
        guard !failed, let inbox, let out else {
            dropDialed(peripheral.identifier, link, cancel: true)
            return
        }
        link.inbox = inbox
        peripheral.setNotifyValue(true, for: out)
    }

    fileprivate func notifying(_ peripheral: CBPeripheral, characteristic: CBCharacteristic, failed: Bool) {
        guard let link = dialedLink(peripheral), characteristic.uuid == outboxId else { return }
        if failed {
            dropDialed(peripheral.identifier, link, cancel: true)
            return
        }
        if !characteristic.isNotifying || link.open { return }
        link.open = true
        continuation.yield(MeshBluetoothEvent(kind: .linkOpened, linkId: link.id, outbound: true))
        pump(link)
    }

    fileprivate func received(_ peripheral: CBPeripheral, characteristic: CBCharacteristic, failed: Bool) {
        guard let link = dialedLink(peripheral), link.open, characteristic.uuid == outboxId, !failed else { return }
        let value = characteristic.value ?? Data()
        if value.isEmpty {
            dropDialed(peripheral.identifier, link, cancel: true)
            return
        }
        continuation.yield(MeshBluetoothEvent(kind: .data, linkId: link.id, bytes: value))
    }

    fileprivate func wrote(_ peripheral: CBPeripheral, failed: Bool) {
        guard let link = dialedLink(peripheral) else { return }
        if failed {
            dropDialed(peripheral.identifier, link, cancel: true)
            return
        }
        link.writing = false
        link.queue.advance()
        pump(link)
    }

    fileprivate func serviceAddedResult(failed: Bool) {
        serviceAdding = false
        if failed {
            outbox = nil
            return
        }
        serviceAdded = true
        advertise()
    }

    fileprivate func didSubscribe(_ central: CBCentral, characteristic: CBCharacteristic) {
        guard listening, characteristic.uuid == outboxId else { return }
        closedCentrals.remove(central.identifier)
        let link = subscribedLink(for: central)
        pump(link)
    }

    fileprivate func didUnsubscribe(_ central: CBCentral, characteristic: CBCharacteristic) {
        guard characteristic.uuid == outboxId else { return }
        closedCentrals.remove(central.identifier)
        guard let link = subscribed.removeValue(forKey: central.identifier) else { return }
        continuation.yield(MeshBluetoothEvent(kind: .linkClosed, linkId: link.id))
    }

    fileprivate func writes(_ requests: [CBATTRequest]) {
        guard let manager = peripheralManager, let first = requests.first else { return }
        guard listening, requests.allSatisfy({ $0.characteristic.uuid == inboxId }) else {
            manager.respond(to: first, withResult: .requestNotSupported)
            return
        }
        guard let value = MeshBle.joinWrites(requests.map { (offset: $0.offset, value: $0.value ?? Data()) }) else {
            manager.respond(to: first, withResult: .invalidOffset)
            return
        }
        manager.respond(to: first, withResult: .success)
        let central = first.central
        if closedCentrals.contains(central.identifier) { return }
        if value.isEmpty {
            if let link = subscribed.removeValue(forKey: central.identifier) {
                closedCentrals.insert(central.identifier)
                continuation.yield(MeshBluetoothEvent(kind: .linkClosed, linkId: link.id))
            }
            return
        }
        let link = subscribedLink(for: central)
        continuation.yield(MeshBluetoothEvent(kind: .data, linkId: link.id, bytes: value))
    }

    fileprivate func readyToUpdate() {
        for link in subscribed.values { pump(link) }
    }
}

extension CoreBluetoothMeshTransport: CBCentralManagerDelegate {
    public nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        MainActor.assumeIsolated { self.centralStateChanged() }
    }

    public nonisolated func centralManager(
        _ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any], rssi RSSI: NSNumber
    ) {
        let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        MainActor.assumeIsolated { self.discovered(peripheral, name: name) }
    }

    public nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        MainActor.assumeIsolated { self.connected(peripheral) }
    }

    public nonisolated func centralManager(
        _ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?
    ) {
        MainActor.assumeIsolated { self.disconnected(peripheral) }
    }

    public nonisolated func centralManager(
        _ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?
    ) {
        MainActor.assumeIsolated { self.disconnected(peripheral) }
    }
}

extension CoreBluetoothMeshTransport: CBPeripheralDelegate {
    public nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        let failed = error != nil
        MainActor.assumeIsolated { self.servicesFound(peripheral, failed: failed) }
    }

    public nonisolated func peripheral(
        _ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?
    ) {
        let failed = error != nil
        MainActor.assumeIsolated { self.characteristicsFound(peripheral, service: service, failed: failed) }
    }

    public nonisolated func peripheral(
        _ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?
    ) {
        let failed = error != nil
        MainActor.assumeIsolated { self.notifying(peripheral, characteristic: characteristic, failed: failed) }
    }

    public nonisolated func peripheral(
        _ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?
    ) {
        let failed = error != nil
        MainActor.assumeIsolated { self.received(peripheral, characteristic: characteristic, failed: failed) }
    }

    public nonisolated func peripheral(
        _ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?
    ) {
        let failed = error != nil
        MainActor.assumeIsolated { self.wrote(peripheral, failed: failed) }
    }
}

extension CoreBluetoothMeshTransport: CBPeripheralManagerDelegate {
    public nonisolated func peripheralManagerDidUpdateState(_ peripheral: CBPeripheralManager) {
        MainActor.assumeIsolated { self.peripheralStateChanged() }
    }

    public nonisolated func peripheralManager(
        _ peripheral: CBPeripheralManager, didAdd service: CBService, error: Error?
    ) {
        let failed = error != nil
        MainActor.assumeIsolated { self.serviceAddedResult(failed: failed) }
    }

    public nonisolated func peripheralManager(
        _ peripheral: CBPeripheralManager, central: CBCentral, didSubscribeTo characteristic: CBCharacteristic
    ) {
        MainActor.assumeIsolated { self.didSubscribe(central, characteristic: characteristic) }
    }

    public nonisolated func peripheralManager(
        _ peripheral: CBPeripheralManager, central: CBCentral, didUnsubscribeFrom characteristic: CBCharacteristic
    ) {
        MainActor.assumeIsolated { self.didUnsubscribe(central, characteristic: characteristic) }
    }

    public nonisolated func peripheralManager(
        _ peripheral: CBPeripheralManager, didReceiveWrite requests: [CBATTRequest]
    ) {
        MainActor.assumeIsolated { self.writes(requests) }
    }

    public nonisolated func peripheralManagerIsReady(toUpdateSubscribers peripheral: CBPeripheralManager) {
        MainActor.assumeIsolated { self.readyToUpdate() }
    }
}
#endif
