import AVFoundation
import CoreBluetooth
import CoreMotion
import Darwin
import FMAccount
import FMBase
import FMTools
import Foundation
import LocalAuthentication
import MachO
import Photos
import Speech
import UIKit
import UserNotifications

enum PhoneSecurityProbe {
    @MainActor
    static func facts() async -> PhoneSecurityFacts {
        var facts = PhoneSecurityFacts()
        facts.passcodeSet = passcodeSet()
        let (biometry, enrolled) = biometrics()
        facts.biometry = biometry
        facts.biometryEnrolled = enrolled
        facts.notificationPreviews = await notificationPreviews()
        facts.screenCaptured = UIScreen.main.isCaptured
        facts.relayURL = Services.get(SettingsService.self)?.relays.active.serverUrl
        facts.osVersion = UIDevice.current.systemVersion
        facts.permissions = permissions()
        let background: PhoneSecurityFacts = await Task.detached(priority: .utility) { () -> PhoneSecurityFacts in
            var off = PhoneSecurityFacts()
            off.existingPaths = Set(PhoneSecurityPaths.probed.filter { FileManager.default.fileExists(atPath: $0) })
            off.symbolicLinks = Set(PhoneSecurityPaths.systemLinks.filter(PhoneSecurityProbe.isSymbolicLink))
            off.loadedImages = PhoneSecurityProbe.loadedImages()
            off.environment = PhoneSecurityProbe.injectionEnvironment()
            off.canWriteOutsideSandbox = PhoneSecurityProbe.canWriteOutsideSandbox()
            off.debuggerAttached = PhoneSecurityProbe.debuggerAttached()
            off.fileProtection = PhoneSecurityProbe.fileProtection()
            off.transportSecurity = PhoneSecurityProbe.transportSecurity()
            let (proxy, vpn) = PhoneSecurityProbe.proxyAndVPN()
            off.proxy = proxy
            off.vpnInterfaces = vpn
            return off
        }.value
        facts.existingPaths = background.existingPaths
        facts.symbolicLinks = background.symbolicLinks
        facts.loadedImages = background.loadedImages
        facts.environment = background.environment
        facts.canWriteOutsideSandbox = background.canWriteOutsideSandbox
        facts.debuggerAttached = background.debuggerAttached
        facts.fileProtection = background.fileProtection
        facts.transportSecurity = background.transportSecurity
        facts.proxy = background.proxy
        facts.vpnInterfaces = background.vpnInterfaces
        #if targetEnvironment(simulator)
        facts.simulator = true
        #else
        facts.simulator = false
        #endif
        #if DEBUG
        facts.debugBuild = true
        #else
        facts.debugBuild = false
        #endif
        facts.installSource = installSource()
        return facts
    }

    private static func passcodeSet() -> Bool? {
        var error: NSError?
        if LAContext().canEvaluatePolicy(.deviceOwnerAuthentication, error: &error) { return true }
        if error.flatMap({ LAError.Code(rawValue: $0.code) }) == .passcodeNotSet { return false }
        return nil
    }

    private static func biometrics() -> (PhoneBiometry?, Bool?) {
        let context = LAContext()
        var error: NSError?
        let ready: Bool = context.canEvaluatePolicy(.deviceOwnerAuthenticationWithBiometrics, error: &error)
        let biometry: PhoneBiometry
        switch context.biometryType {
        case .faceID: biometry = .faceID
        case .touchID: biometry = .touchID
        case .opticID: biometry = .opticID
        default: biometry = .none
        }
        if ready { return (biometry, true) }
        switch error.flatMap({ LAError.Code(rawValue: $0.code) }) {
        case .biometryNotEnrolled: return (biometry, false)
        default: return (biometry, nil)
        }
    }

    private static func notificationPreviews() async -> PhoneNotificationPreviews? {
        let settings: UNNotificationSettings = await UNUserNotificationCenter.current().notificationSettings()
        switch settings.authorizationStatus {
        case .denied, .notDetermined: return .notificationsOff
        default: break
        }
        switch settings.showPreviewsSetting {
        case .always: return .always
        case .whenAuthenticated: return .whenAuthenticated
        case .never: return .never
        @unknown default: return nil
        }
    }

    private static func permissions() -> [PhonePermission: PhonePermissionState] {
        var out: [PhonePermission: PhonePermissionState] = [:]
        out[.camera] = capture(AVCaptureDevice.authorizationStatus(for: .video))
        out[.microphone] = capture(AVCaptureDevice.authorizationStatus(for: .audio))
        switch PHPhotoLibrary.authorizationStatus(for: .readWrite) {
        case .authorized: out[.photos] = .allowed
        case .limited: out[.photos] = .limited
        case .denied: out[.photos] = .denied
        case .restricted: out[.photos] = .restricted
        case .notDetermined: out[.photos] = .notAsked
        @unknown default: break
        }
        switch SFSpeechRecognizer.authorizationStatus() {
        case .authorized: out[.speech] = .allowed
        case .denied: out[.speech] = .denied
        case .restricted: out[.speech] = .restricted
        case .notDetermined: out[.speech] = .notAsked
        @unknown default: break
        }
        switch CBManager.authorization {
        case .allowedAlways: out[.bluetooth] = .allowed
        case .denied: out[.bluetooth] = .denied
        case .restricted: out[.bluetooth] = .restricted
        case .notDetermined: out[.bluetooth] = .notAsked
        @unknown default: break
        }
        switch CMMotionActivityManager.authorizationStatus() {
        case .authorized: out[.motion] = .allowed
        case .denied: out[.motion] = .denied
        case .restricted: out[.motion] = .restricted
        case .notDetermined: out[.motion] = .notAsked
        @unknown default: break
        }
        return out
    }

    private static func capture(_ status: AVAuthorizationStatus) -> PhonePermissionState? {
        switch status {
        case .authorized: return .allowed
        case .denied: return .denied
        case .restricted: return .restricted
        case .notDetermined: return .notAsked
        @unknown default: return nil
        }
    }

    private static func isSymbolicLink(_ path: String) -> Bool {
        var info = stat()
        guard lstat(path, &info) == 0 else { return false }
        return (info.st_mode & S_IFMT) == S_IFLNK
    }

    private static func loadedImages() -> [String] {
        (0..<_dyld_image_count()).compactMap { index in _dyld_get_image_name(index).map { String(cString: $0) } }
    }

    private static func injectionEnvironment() -> [String: String] {
        let environment: [String: String] = ProcessInfo.processInfo.environment
        return environment.filter { PhoneSecurityPaths.injectionVariables.contains($0.key) }
    }

    private static func canWriteOutsideSandbox() -> Bool {
        let path: String = "/private/forkmesh-sandbox-\(UUID().uuidString)"
        let descriptor: Int32 = open(path, O_WRONLY | O_CREAT | O_EXCL, 0o600)
        guard descriptor >= 0 else { return false }
        close(descriptor)
        unlink(path)
        return true
    }

    private static func debuggerAttached() -> Bool? {
        var info = kinfo_proc()
        var size: Int = MemoryLayout<kinfo_proc>.stride
        var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()]
        guard sysctl(&mib, UInt32(mib.count), &info, &size, nil, 0) == 0 else { return nil }
        return (info.kp_proc.p_flag & P_TRACED) != 0
    }

    private static func fileProtection() -> PhoneFileProtection? {
        let library: URL = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
        let bundle: String = Bundle.main.bundleIdentifier ?? "com.forkmesh.forkmesh"
        let candidates: [URL] = [
            library.appendingPathComponent("Preferences/\(bundle).plist"),
            FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0],
            FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0],
        ]
        for url in candidates {
            guard let attributes = try? FileManager.default.attributesOfItem(atPath: url.path),
                  let protection = attributes[.protectionKey] as? FileProtectionType else { continue }
            switch protection {
            case .complete: return .complete
            case .completeUnlessOpen: return .completeUnlessOpen
            case .completeUntilFirstUserAuthentication: return .completeUntilFirstUserAuthentication
            case .none: return PhoneFileProtection.none
            default: continue
            }
        }
        return nil
    }

    private static func transportSecurity() -> PhoneTransportSecurity {
        let ats: [String: Any] = Bundle.main.object(forInfoDictionaryKey: "NSAppTransportSecurity") as? [String: Any] ?? [:]
        let arbitrary: Bool = ats["NSAllowsArbitraryLoads"] as? Bool ?? false
        let exceptions: [String: Any] = ats["NSExceptionDomains"] as? [String: Any] ?? [:]
        let insecure: [String] = exceptions.compactMap { domain, value in
            let rules: [String: Any] = value as? [String: Any] ?? [:]
            return (rules["NSExceptionAllowsInsecureHTTPLoads"] as? Bool ?? false) ? domain : nil
        }
        return PhoneTransportSecurity(allowsArbitraryLoads: arbitrary, insecureDomains: insecure)
    }

    private static func proxyAndVPN() -> (PhoneProxySettings?, [String]?) {
        guard let settings = CFNetworkCopySystemProxySettings()?.takeRetainedValue() as? [String: Any] else { return (nil, nil) }
        let scoped: [String] = (settings["__SCOPED__"] as? [String: Any]).map { Array($0.keys) } ?? []
        return (PhoneProxySettings.parse(settings), PhoneProxySettings.vpnInterfaces(scoped: scoped))
    }

    private static func installSource() -> PhoneInstallSource? {
        #if targetEnvironment(simulator)
        return .development
        #else
        if Bundle.main.path(forResource: "embedded", ofType: "mobileprovision") != nil { return .development }
        if Bundle.main.appStoreReceiptURL?.lastPathComponent == "sandboxReceipt" { return .testFlight }
        return .appStore
        #endif
    }
}
