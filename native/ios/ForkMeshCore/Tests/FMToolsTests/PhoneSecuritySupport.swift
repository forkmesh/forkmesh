import FMTools
import Foundation

enum PhoneSecurityFixtures {
    static func hardened() -> PhoneSecurityFacts {
        PhoneSecurityFacts(
            passcodeSet: true, biometry: .faceID, biometryEnrolled: true, notificationPreviews: .whenAuthenticated,
            existingPaths: [], symbolicLinks: [],
            loadedImages: [
                "/usr/lib/libSystem.B.dylib", "/usr/lib/swift/libswiftCore.dylib",
                "/System/Library/Frameworks/UIKit.framework/UIKit",
                "/private/var/containers/Bundle/Application/X/ForkMesh.app/ForkMesh",
            ],
            environment: ["HOME": "/var/mobile", "TMPDIR": "/private/var/tmp"],
            canWriteOutsideSandbox: false, debuggerAttached: false, simulator: false,
            osVersion: "\(PhoneSecurityPolicy.newestIOSMajor).1", installSource: .appStore, debugBuild: false,
            fileProtection: .completeUntilFirstUserAuthentication, transportSecurity: PhoneTransportSecurity(),
            relayURL: "wss://forkmesh.com/api/repo/a/b/ws", screenCaptured: false, proxy: PhoneProxySettings(),
            vpnInterfaces: [],
            permissions: Dictionary(uniqueKeysWithValues: PhonePermission.allCases.map { ($0, PhonePermissionState.denied) }))
    }

    static func jailbroken() -> PhoneSecurityFacts {
        var facts: PhoneSecurityFacts = hardened()
        facts.existingPaths = ["/Applications/Sileo.app", "/var/jb", "/usr/lib/libhooker.dylib", "/usr/sbin/sshd"]
        facts.symbolicLinks = ["/Applications"]
        facts.loadedImages = (facts.loadedImages ?? []) + ["/var/jb/usr/lib/TweakInject.dylib"]
        facts.environment = ["DYLD_INSERT_LIBRARIES": "/var/jb/usr/lib/TweakLoader.dylib"]
        facts.canWriteOutsideSandbox = true
        return facts
    }

    static func finding(_ id: String, _ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        guard let finding = PhoneSecurityScanner.scan(facts).first(where: { $0.check.id == id }) else {
            fatalError("no finding \(id)")
        }
        return finding
    }

    static func status(_ id: String, _ facts: PhoneSecurityFacts) -> PhoneSecurityStatus { finding(id, facts).status }

    static func with(_ change: (inout PhoneSecurityFacts) -> Void) -> PhoneSecurityFacts {
        var facts: PhoneSecurityFacts = hardened()
        change(&facts)
        return facts
    }
}
