import AVFoundation
import AudioToolbox
import CoreBluetooth
import CoreHaptics
import CoreMotion
import FMBase
import FMTools
import Observation
import UIKit
import UserNotifications

@MainActor
@Observable
final class GlassesWatcher: NSObject {
    static let shared: GlassesWatcher = GlassesWatcher()
    static let ringSeconds: UInt64 = 30
    static let refreshSeconds: UInt64 = 5
    static let homingNanoseconds: UInt64 = 500_000_000

    private(set) var watching: Bool = false
    private(set) var ringing: GlassesSighting?
    private(set) var nearby: [GlassesSighting] = []
    private(set) var problem: String?
    private(set) var homing: GlassesHomingReading?
    private(set) var others: [NearbyDevice] = []
    private(set) var tracked: GlassesHomingReading?
    private(set) var heading: Double?
    var range: GlassesRange = .defaultRange {
        didSet { prefs?.set(range.rawValue, forKey: GlassesAlertPrefs.range) }
    }
    var keepAwake: Bool = true {
        didSet {
            prefs?.set(keepAwake, forKey: GlassesAlertPrefs.keepAwake)
            applyIdleTimer()
        }
    }

    @ObservationIgnored private var central: CBCentralManager?
    @ObservationIgnored private var tracker: GlassesTracker = GlassesTracker()
    @ObservationIgnored private var homingState: GlassesHoming = GlassesHoming()
    @ObservationIgnored private var census: GlassesCensus = GlassesCensus()
    @ObservationIgnored private var trackState: GlassesHoming = GlassesHoming()
    @ObservationIgnored private var trackedDevice: NearbyDevice?
    @ObservationIgnored private let motion: CMMotionManager = CMMotionManager()
    @ObservationIgnored private var homingTicker: Task<Void, Never>?
    @ObservationIgnored private var player: AVAudioPlayer?
    @ObservationIgnored private var haptics: CHHapticEngine?
    @ObservationIgnored private var hapticPlayer: CHHapticAdvancedPatternPlayer?
    @ObservationIgnored private var buzzer: Task<Void, Never>?
    @ObservationIgnored private var timeout: Task<Void, Never>?
    @ObservationIgnored private var refresher: Task<Void, Never>?
    @ObservationIgnored private var wantsScan: Bool = false

    private var prefs: KeyValueStore? { Services.get(KeyValueStore.self) }

    override init() {
        super.init()
        if let prefs = Services.get(KeyValueStore.self) {
            range = GlassesRange.of(prefs.string(forKey: GlassesAlertPrefs.range))
            keepAwake = prefs.bool(forKey: GlassesAlertPrefs.keepAwake) ?? true
        }
    }

    var bluetoothDenied: Bool {
        CBManager.authorization == .denied || CBManager.authorization == .restricted
    }

    var resumesOnOpen: Bool { prefs?.bool(forKey: GlassesAlertPrefs.watching) ?? false }

    func start() {
        prefs?.set(true, forKey: GlassesAlertPrefs.watching)
        wantsScan = true
        problem = nil
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound, .badge]) { _, _ in }
        if let central = central {
            beginScan(central)
        } else {
            central = CBCentralManager(delegate: self, queue: .main, options: [CBCentralManagerOptionShowPowerAlertKey: true])
        }
    }

    func stop() {
        prefs?.set(false, forKey: GlassesAlertPrefs.watching)
        wantsScan = false
        central?.stopScan()
        watching = false
        refresher?.cancel()
        refresher = nil
        tracker.reset()
        nearby = []
        clearHoming()
        silence()
        applyIdleTimer()
    }

    func track(_ device: NearbyDevice) {
        trackState.reset()
        trackedDevice = device
        publishHoming()
    }

    func untrack() {
        trackState.reset()
        trackedDevice = nil
        tracked = nil
        publishHoming()
    }

    func test() {
        ring(GlassesSighting(id: "test", label: "Test alarm", maker: .luxottica, namedAsGlasses: true, rssi: -58, seenAt: Date()))
    }

    func hush() {
        guard player != nil else { return }
        player?.stop()
        player = nil
        if !PromptDictation.isListening && DesktopAlarm.shared.ringing == nil {
            OtherAppCall.deactivateSession()
        }
    }

    func silence() {
        timeout?.cancel()
        timeout = nil
        buzzer?.cancel()
        buzzer = nil
        player?.stop()
        player = nil
        try? hapticPlayer?.stop(atTime: CHHapticTimeImmediate)
        hapticPlayer = nil
        haptics?.stop(completionHandler: nil)
        haptics = nil
        ringing = nil
        if !PromptDictation.isListening && DesktopAlarm.shared.ringing == nil {
            OtherAppCall.deactivateSession()
        }
    }

    private func beginScan(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            central.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
            watching = true
            tracker.reset()
            nearby = []
            clearHoming()
            applyIdleTimer()
            startRefreshing()
        case .unauthorized:
            failed("Allow Bluetooth for ForkMesh in Settings so Glasses Alert can scan.")
        case .poweredOff:
            failed("Turn Bluetooth on to watch for smart glasses and cameras.")
        case .unsupported:
            failed("This device has no Bluetooth LE.")
        default:
            break
        }
    }

    private func failed(_ message: String) {
        watching = false
        problem = message
        applyIdleTimer()
    }

    private func startRefreshing() {
        refresher?.cancel()
        refresher = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: GlassesWatcher.refreshSeconds * 1_000_000_000)
                guard let self = self, !Task.isCancelled else { return }
                self.nearby = self.tracker.nearby(Date())
                self.homingState.follow(self.nearby, now: Date())
            }
        }
        homingTicker?.cancel()
        homingTicker = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: GlassesWatcher.homingNanoseconds)
                guard let self = self, !Task.isCancelled else { return }
                self.publishHoming()
            }
        }
    }

    private func publishHoming() {
        let now: Date = Date()
        let reading: GlassesHomingReading? = homingState.reading(now)
        if reading != homing { homing = reading }
        let heard: [NearbyDevice] = census.devices(now)
        if heard != others { others = heard }
        if let wanted = trackedDevice {
            let device: NearbyDevice = heard.first { $0.id == wanted.id } ?? wanted
            trackState.follow(
                [GlassesSighting(id: device.id, label: device.label, maker: nil, namedAsGlasses: false, rssi: device.rssi, seenAt: now)],
                now: now)
        }
        let followed: GlassesHomingReading? = trackState.reading(now)
        if followed != tracked { tracked = followed }
        if reading != nil || followed != nil { startCompass() } else { stopCompass() }
    }

    private func clearHoming() {
        homingTicker?.cancel()
        homingTicker = nil
        homingState.reset()
        census.reset()
        trackState.reset()
        trackedDevice = nil
        homing = nil
        others = []
        tracked = nil
        stopCompass()
    }

    private func startCompass() {
        guard !motion.isDeviceMotionActive, motion.isDeviceMotionAvailable,
            CMMotionManager.availableAttitudeReferenceFrames().contains(.xMagneticNorthZVertical)
        else { return }
        motion.deviceMotionUpdateInterval = 1.0 / 20.0
        motion.startDeviceMotionUpdates(using: .xMagneticNorthZVertical, to: .main) { [weak self] (data: CMDeviceMotion?, _: Error?) in
            guard let data = data, data.magneticField.accuracy != .uncalibrated else { return }
            let gravity: CMAcceleration = data.gravity
            let field: CMMagneticField = data.magneticField.field
            MainActor.assumeIsolated {
                self?.turned(up: [-gravity.x, -gravity.y, -gravity.z], magnetic: [field.x, field.y, field.z])
            }
        }
    }

    private func stopCompass() {
        if motion.isDeviceMotionActive { motion.stopDeviceMotionUpdates() }
        if heading != nil { heading = nil }
    }

    private func turned(up: [Double], magnetic: [Double]) {
        guard let rotation = GlassesHoming.rotation(up: up, magnetic: magnetic),
            let next = GlassesHoming.heading(rotation: rotation)
        else { return }
        heading = GlassesHoming.smoothHeading(heading, next)
    }

    private func applyIdleTimer() {
        UIApplication.shared.isIdleTimerDisabled = watching && keepAwake
    }

    fileprivate func handle(identifier: String, name: String?, rssi: Int, manufacturerData: Data?, services: [String]) {
        guard watching else { return }
        let ids: [Int] = manufacturerData.flatMap { GlassesSignatures.companyId(manufacturerData: [UInt8]($0)) }.map { [$0] } ?? []
        let advert: GlassesAdvert = GlassesAdvert(
            identifier: identifier, name: name, rssi: rssi, companyIds: ids, serviceUuids: services.compactMap(NearbyKinds.shortUuid))
        let now: Date = Date()
        if let sighting = GlassesSignatures.match(advert, range: range, now: now) {
            if tracker.observe(sighting) {
                ring(sighting)
            }
            nearby = tracker.nearby(now)
            let had: Bool = homingState.target != nil
            homingState.follow(nearby, now: now)
            if !had && homingState.target != nil { publishHoming() }
        } else if GlassesSignatures.kind(advert) == nil,
            let device = NearbyKinds.device(advert, now: now)
        {
            census.observe(device)
        }
        homingState.hear(identifier, rssi: rssi, heading: heading, now: now)
        trackState.hear(identifier, rssi: rssi, heading: heading, now: now)
    }

    fileprivate func stateChanged(_ central: CBCentralManager) {
        if wantsScan {
            beginScan(central)
        }
        if central.state != .poweredOn {
            watching = false
            refresher?.cancel()
            clearHoming()
            applyIdleTimer()
        }
    }

    private func ring(_ sighting: GlassesSighting) {
        let already: Bool = ringing != nil
        ringing = sighting
        notify(sighting)
        if !already {
            startSound()
            startHaptics()
        }
        timeout?.cancel()
        timeout = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: GlassesWatcher.ringSeconds * 1_000_000_000)
            guard !Task.isCancelled else { return }
            self?.silence()
        }
    }

    private func notify(_ sighting: GlassesSighting) {
        let content: UNMutableNotificationContent = UNMutableNotificationContent()
        content.title = sighting.kind.title
        content.body = GlassesSignatures.alertText(sighting)
        content.sound = UNNotificationSound(named: UNNotificationSoundName("\(DesktopAlarm.soundName).wav"))
        content.interruptionLevel = .timeSensitive
        content.threadIdentifier = "glasses-alert"
        let request: UNNotificationRequest = UNNotificationRequest(identifier: "glasses-alert", content: content, trigger: nil)
        UNUserNotificationCenter.current().add(request) { _ in }
    }

    private func startSound() {
        guard player == nil, !OtherAppCall.inProgress,
            let url = Bundle.main.url(forResource: DesktopAlarm.soundName, withExtension: "wav"),
            let sound = try? AVAudioPlayer(contentsOf: url)
        else { return }
        if !PromptDictation.isListening {
            let session: AVAudioSession = AVAudioSession.sharedInstance()
            try? session.setCategory(.playback, mode: .default, options: [.duckOthers])
            try? session.setActive(true)
        }
        sound.numberOfLoops = -1
        sound.volume = 1
        sound.prepareToPlay()
        sound.play()
        player = sound
    }

    private func startHaptics() {
        if CHHapticEngine.capabilitiesForHardware().supportsHaptics, let engine = try? CHHapticEngine() {
            engine.isAutoShutdownEnabled = false
            engine.playsHapticsOnly = true
            if (try? engine.start()) != nil, let pattern = try? GlassesWatcher.alarmPattern(),
                let advanced = try? engine.makeAdvancedPlayer(with: pattern)
            {
                advanced.loopEnabled = true
                if (try? advanced.start(atTime: CHHapticTimeImmediate)) != nil {
                    haptics = engine
                    hapticPlayer = advanced
                    return
                }
            }
        }
        buzzer = Task { @MainActor in
            while !Task.isCancelled {
                AudioServicesPlaySystemSound(kSystemSoundID_Vibrate)
                try? await Task.sleep(nanoseconds: 1_200_000_000)
            }
        }
    }

    private static func alarmPattern() throws -> CHHapticPattern {
        let strong: [CHHapticEventParameter] = [
            CHHapticEventParameter(parameterID: .hapticIntensity, value: 1),
            CHHapticEventParameter(parameterID: .hapticSharpness, value: 0.8),
        ]
        let events: [CHHapticEvent] = [0.0, 1.1, 2.2].flatMap { (at: TimeInterval) -> [CHHapticEvent] in
            [
                CHHapticEvent(eventType: .hapticTransient, parameters: strong, relativeTime: at),
                CHHapticEvent(eventType: .hapticContinuous, parameters: strong, relativeTime: at, duration: 0.85),
            ]
        }
        return try CHHapticPattern(events: events, parameters: [])
    }
}

extension GlassesWatcher: CBCentralManagerDelegate {
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        MainActor.assumeIsolated {
            self.stateChanged(central)
        }
    }

    nonisolated func centralManager(
        _ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String: Any], rssi RSSI: NSNumber
    ) {
        let identifier: String = peripheral.identifier.uuidString
        let name: String? = (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? peripheral.name
        let data: Data? = advertisementData[CBAdvertisementDataManufacturerDataKey] as? Data
        let listed: [CBUUID] = (advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID]) ?? []
        let carried: [CBUUID] = ((advertisementData[CBAdvertisementDataServiceDataKey] as? [CBUUID: Data]) ?? [:]).map(\.key)
        let services: [String] = (listed + carried).map(\.uuidString)
        let strength: Int = RSSI.intValue
        MainActor.assumeIsolated {
            self.handle(identifier: identifier, name: name, rssi: strength, manufacturerData: data, services: services)
        }
    }
}
