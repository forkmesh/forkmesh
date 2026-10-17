import FMTools
import Foundation
import XCTest

final class PhoneSecurityRulesTests: XCTestCase {
    private typealias F = PhoneSecurityFixtures

    func testAHardenedPhoneHasNothingToFix() {
        let findings: [PhoneSecurityFinding] = PhoneSecurityScanner.scan(F.hardened())
        XCTAssertEqual(findings.filter { $0.status != .pass }.map(\.check.id), [])
        XCTAssertTrue(findings.allSatisfy { $0.fix == nil })
    }

    func testAnEmptySnapshotIsUnavailableEverywhere() {
        let findings: [PhoneSecurityFinding] = PhoneSecurityScanner.scan(PhoneSecurityFacts())
        XCTAssertEqual(findings.count, PhoneSecurityCatalog.all.count)
        XCTAssertTrue(findings.allSatisfy { $0.status == .unavailable }, findings.filter { $0.status != .unavailable }.map(\.check.id).description)
        XCTAssertTrue(findings.allSatisfy { $0.fix == nil && !$0.detail.isEmpty })
    }

    func testAJailbrokenPhoneIsCriticalAcrossIntegrityChecks() {
        let critical: Set<String> = Set(PhoneSecurityScanner.scan(F.jailbroken()).filter { $0.status == .critical }.map(\.check.id))
        XCTAssertEqual(critical, [
            "ios.jailbreak-apps", "ios.jailbreak-tools", "ios.rootless-jailbreak", "ios.tweak-frameworks",
            "ios.injected-libraries", "ios.dyld-environment", "ios.sandbox", "ios.system-links",
        ])
    }

    func testIntegrityFindingsOfferNoFix() {
        for finding in PhoneSecurityScanner.scan(F.jailbroken()) where finding.check.category == .deviceIntegrity {
            XCTAssertNil(finding.fix, finding.check.id)
        }
    }

    func testNoPasscodeIsCriticalWithSteps() {
        let finding: PhoneSecurityFinding = F.finding("ios.passcode", F.with { $0.passcodeSet = false })
        XCTAssertEqual(finding.status, .critical)
        XCTAssertEqual(finding.detail, "No passcode is set.")
        XCTAssertEqual(finding.fix?.action, .steps("Settings → Face ID & Passcode → Turn Passcode On"))
        XCTAssertFalse(finding.canFix)
    }

    func testPasscodeSetPasses() {
        XCTAssertEqual(F.finding("ios.passcode", F.hardened()).detail, "A passcode is set.")
    }

    func testUnknownPasscodeIsUnavailable() {
        XCTAssertEqual(F.status("ios.passcode", F.with { $0.passcodeSet = nil }), .unavailable)
    }

    func testBiometricsEnrolledPassesWithItsName() {
        XCTAssertEqual(F.finding("ios.biometrics", F.hardened()).detail, "Face ID is set up.")
        XCTAssertEqual(F.finding("ios.biometrics", F.with { $0.biometry = .touchID }).detail, "Touch ID is set up.")
        XCTAssertEqual(F.finding("ios.biometrics", F.with { $0.biometry = .opticID }).detail, "Optic ID is set up.")
    }

    func testBiometricsNotEnrolledIsAReviewWithSetupSteps() {
        let face: PhoneSecurityFinding = F.finding("ios.biometrics", F.with { $0.biometryEnrolled = false })
        XCTAssertEqual(face.status, .review)
        XCTAssertEqual(face.fix?.action, .steps("Settings → Face ID & Passcode → Set Up Face ID"))
        let touch: PhoneSecurityFinding = F.finding("ios.biometrics", F.with {
            $0.biometry = .touchID
            $0.biometryEnrolled = false
        })
        XCTAssertEqual(touch.fix?.action, .steps("Settings → Touch ID & Passcode → Set Up Touch ID"))
    }

    func testBiometricsWithoutPasscodeAsksForThePasscodeFirst() {
        let finding: PhoneSecurityFinding = F.finding("ios.biometrics", F.with {
            $0.passcodeSet = false
            $0.biometryEnrolled = nil
        })
        XCTAssertEqual(finding.status, .review)
        XCTAssertEqual(finding.detail, "Face ID needs a passcode first.")
        XCTAssertEqual(finding.fix?.action, .steps("Settings → Face ID & Passcode → Turn Passcode On"))
    }

    func testNoBiometricHardwarePasses() {
        let finding: PhoneSecurityFinding = F.finding("ios.biometrics", F.with {
            $0.biometry = PhoneBiometry.none
            $0.biometryEnrolled = nil
        })
        XCTAssertEqual(finding.status, .pass)
        XCTAssertEqual(finding.detail, "This iPhone has no Face ID or Touch ID.")
    }

    func testUnknownEnrolmentIsUnavailable() {
        XCTAssertEqual(F.status("ios.biometrics", F.with { $0.biometryEnrolled = nil }), .unavailable)
        XCTAssertEqual(F.status("ios.biometrics", F.with { $0.biometry = nil }), .unavailable)
    }

    func testAlwaysShowingPreviewsIsAReviewThatOpensNotificationSettings() {
        let finding: PhoneSecurityFinding = F.finding("ios.notification-previews", F.with { $0.notificationPreviews = .always })
        XCTAssertEqual(finding.status, .review)
        XCTAssertEqual(finding.fix?.action, .open(.appNotifications))
        XCTAssertTrue(finding.canFix)
    }

    func testOtherPreviewSettingsPass() {
        for previews in [PhoneNotificationPreviews.whenAuthenticated, .never, .notificationsOff] {
            XCTAssertEqual(F.status("ios.notification-previews", F.with { $0.notificationPreviews = previews }), .pass, previews.rawValue)
        }
        XCTAssertEqual(F.finding("ios.notification-previews", F.with { $0.notificationPreviews = .notificationsOff }).detail,
                       "Notifications are off for ForkMesh.")
    }

    func testEveryJailbreakAppIsCritical() {
        for path in PhoneSecurityPaths.jailbreakApps {
            let finding: PhoneSecurityFinding = F.finding("ios.jailbreak-apps", F.with { $0.existingPaths = [path] })
            XCTAssertEqual(finding.status, .critical, path)
            XCTAssertEqual(finding.detail, "Found \(path).")
        }
    }

    func testEveryJailbreakToolIsCritical() {
        for path in PhoneSecurityPaths.jailbreakTools {
            XCTAssertEqual(F.status("ios.jailbreak-tools", F.with { $0.existingPaths = [path] }), .critical, path)
        }
    }

    func testEveryRootlessFolderIsCritical() {
        for path in PhoneSecurityPaths.rootlessJailbreak {
            XCTAssertEqual(F.status("ios.rootless-jailbreak", F.with { $0.existingPaths = [path] }), .critical, path)
        }
    }

    func testEveryTweakFrameworkIsCritical() {
        for path in PhoneSecurityPaths.tweakFrameworks {
            XCTAssertEqual(F.status("ios.tweak-frameworks", F.with { $0.existingPaths = [path] }), .critical, path)
        }
    }

    func testAPathOnlyTripsItsOwnCheck() {
        let facts: PhoneSecurityFacts = F.with { $0.existingPaths = ["/Applications/Cydia.app"] }
        XCTAssertEqual(F.status("ios.jailbreak-apps", facts), .critical)
        XCTAssertEqual(F.status("ios.jailbreak-tools", facts), .pass)
        XCTAssertEqual(F.status("ios.rootless-jailbreak", facts), .pass)
        XCTAssertEqual(F.status("ios.tweak-frameworks", facts), .pass)
    }

    func testFoundPathsKeepCatalogOrder() {
        let facts: PhoneSecurityFacts = F.with { $0.existingPaths = ["/usr/bin/ssh", "/bin/bash", "/etc/apt"] }
        XCTAssertEqual(F.finding("ios.jailbreak-tools", facts).detail, "Found /bin/bash, /usr/bin/ssh, /etc/apt.")
    }

    func testUnrelatedPathsPassAndCountWhatWasChecked() {
        let finding: PhoneSecurityFinding = F.finding("ios.jailbreak-apps", F.with { $0.existingPaths = ["/Applications/Safari.app"] })
        XCTAssertEqual(finding.status, .pass)
        XCTAssertEqual(finding.detail, "None found in \(PhoneSecurityPaths.jailbreakApps.count) places checked.")
    }

    func testPathChecksAreUnavailableWithoutAProbe() {
        let facts: PhoneSecurityFacts = F.with { $0.existingPaths = nil }
        for id in ["ios.jailbreak-apps", "ios.jailbreak-tools", "ios.rootless-jailbreak", "ios.tweak-frameworks"] {
            XCTAssertEqual(F.status(id, facts), .unavailable, id)
        }
    }

    func testEveryInjectedImageMarkerIsCaught() {
        for marker in PhoneSecurityPaths.injectedImageMarkers {
            let image: String = "/private/var/tmp/\(marker.uppercased()).dylib"
            XCTAssertEqual(PhoneSecurityScanner.injectedImages([image]), ["\(marker.uppercased()).dylib"], marker)
        }
    }

    func testRealInjectedImagesAreNamed() {
        let images: [String] = [
            "/usr/lib/libSystem.B.dylib",
            "/Library/MobileSubstrate/MobileSubstrate.dylib",
            "/usr/lib/FridaGadget.dylib",
            "/var/jb/usr/lib/libellekit.dylib",
            "/Library/MobileSubstrate/DynamicLibraries/SSLKillSwitch2.dylib",
            "/usr/lib/FridaGadget.dylib",
        ]
        XCTAssertEqual(PhoneSecurityScanner.injectedImages(images),
                       ["MobileSubstrate.dylib", "FridaGadget.dylib", "libellekit.dylib", "SSLKillSwitch2.dylib"])
        let finding: PhoneSecurityFinding = F.finding("ios.injected-libraries", F.with { $0.loadedImages = images })
        XCTAssertEqual(finding.status, .critical)
        XCTAssertEqual(finding.detail, "4 injected libraries are loaded into ForkMesh.")
        XCTAssertEqual(finding.usage, "Loaded: MobileSubstrate.dylib, FridaGadget.dylib, libellekit.dylib, SSLKillSwitch2.dylib.")
    }

    func testOneInjectedImageUsesTheSingular() {
        let finding: PhoneSecurityFinding = F.finding("ios.injected-libraries", F.with { $0.loadedImages = ["/x/libhooker.dylib"] })
        XCTAssertEqual(finding.detail, "1 injected library is loaded into ForkMesh.")
    }

    func testSystemLibrariesAreNotMistakenForInjection() {
        let images: [String] = [
            "/usr/lib/libobjc.A.dylib", "/usr/lib/system/libsystem_kernel.dylib", "/usr/lib/libsqlite3.dylib",
            "/System/Library/Frameworks/Security.framework/Security", "/System/Library/PrivateFrameworks/Substrate.framework",
            "/usr/lib/swift/libswiftFoundation.dylib", "/System/Library/Frameworks/CoreBluetooth.framework/CoreBluetooth",
            "/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices",
        ]
        XCTAssertEqual(PhoneSecurityScanner.injectedImages(images), [])
        let finding: PhoneSecurityFinding = F.finding("ios.injected-libraries", F.with { $0.loadedImages = images })
        XCTAssertEqual(finding.detail, "No injected libraries among 8 loaded.")
    }

    func testUnknownImagesAreUnavailable() {
        XCTAssertEqual(F.status("ios.injected-libraries", F.with { $0.loadedImages = nil }), .unavailable)
    }

    func testEveryInjectionVariableIsCritical() {
        for name in PhoneSecurityPaths.injectionVariables {
            let finding: PhoneSecurityFinding = F.finding("ios.dyld-environment", F.with { $0.environment = [name: "/tmp/x.dylib"] })
            XCTAssertEqual(finding.status, .critical, name)
            XCTAssertEqual(finding.detail, "Set: \(name).")
            XCTAssertEqual(finding.usage, "\(name)=/tmp/x.dylib")
        }
    }

    func testBlankInjectionVariablesAreIgnored() {
        XCTAssertEqual(PhoneSecurityScanner.injectionVariables(["DYLD_INSERT_LIBRARIES": "  ", "HOME": "/x"]), [])
        XCTAssertEqual(F.status("ios.dyld-environment", F.with { $0.environment = ["DYLD_INSERT_LIBRARIES": ""] }), .pass)
    }

    func testInjectionVariablesAreOnlyAReviewWhileDeveloping() {
        let env: [String: String] = ["DYLD_INSERT_LIBRARIES": "/usr/lib/libMainThreadChecker.dylib"]
        XCTAssertEqual(F.status("ios.dyld-environment", F.with {
            $0.environment = env
            $0.debugBuild = true
        }), .review)
        XCTAssertEqual(F.status("ios.dyld-environment", F.with {
            $0.environment = env
            $0.simulator = true
        }), .review)
    }

    func testLongInjectionValuesAreTrimmed() {
        let long: String = String(repeating: "a", count: 500)
        let finding: PhoneSecurityFinding = F.finding("ios.dyld-environment", F.with { $0.environment = ["DYLD_LIBRARY_PATH": long] })
        XCTAssertEqual(finding.usage?.count, "DYLD_LIBRARY_PATH=".count + 120)
    }

    func testSeveralInjectionVariablesAreListedInOrder() {
        let finding: PhoneSecurityFinding = F.finding("ios.dyld-environment", F.with {
            $0.environment = ["_SafeMode": "1", "DYLD_INSERT_LIBRARIES": "/a.dylib"]
        })
        XCTAssertEqual(finding.detail, "Set: DYLD_INSERT_LIBRARIES, _SafeMode.")
        XCTAssertEqual(finding.usage, "DYLD_INSERT_LIBRARIES=/a.dylib; _SafeMode=1")
    }

    func testSandboxEscapeIsCritical() {
        XCTAssertEqual(F.status("ios.sandbox", F.with { $0.canWriteOutsideSandbox = true }), .critical)
        XCTAssertEqual(F.status("ios.sandbox", F.hardened()), .pass)
        XCTAssertEqual(F.status("ios.sandbox", F.with { $0.canWriteOutsideSandbox = nil }), .unavailable)
    }

    func testEverySystemLinkIsCritical() {
        for path in PhoneSecurityPaths.systemLinks {
            let finding: PhoneSecurityFinding = F.finding("ios.system-links", F.with { $0.symbolicLinks = [path] })
            XCTAssertEqual(finding.status, .critical, path)
            XCTAssertEqual(finding.detail, "Replaced by links: \(path).")
        }
    }

    func testLinksOutsideTheListPass() {
        let finding: PhoneSecurityFinding = F.finding("ios.system-links", F.with { $0.symbolicLinks = ["/var", "/etc", "/tmp"] })
        XCTAssertEqual(finding.status, .pass)
        XCTAssertEqual(finding.detail, "None of \(PhoneSecurityPaths.systemLinks.count) system folders is a link.")
        XCTAssertEqual(F.status("ios.system-links", F.with { $0.symbolicLinks = nil }), .unavailable)
    }

    func testADebuggerWarnsOnAReleaseBuild() {
        let finding: PhoneSecurityFinding = F.finding("ios.debugger", F.with { $0.debuggerAttached = true })
        XCTAssertEqual(finding.status, .warning)
        XCTAssertNotNil(finding.usage)
    }

    func testADebuggerIsAReviewOnADebugBuild() {
        XCTAssertEqual(F.status("ios.debugger", F.with {
            $0.debuggerAttached = true
            $0.debugBuild = true
        }), .review)
    }

    func testNoDebuggerPasses() {
        XCTAssertEqual(F.status("ios.debugger", F.hardened()), .pass)
        XCTAssertEqual(F.status("ios.debugger", F.with { $0.debuggerAttached = nil }), .unavailable)
    }

    func testTheSimulatorIsAReview() {
        XCTAssertEqual(F.status("ios.simulator", F.with { $0.simulator = true }), .review)
        XCTAssertEqual(F.finding("ios.simulator", F.hardened()).detail, "Running on a real iPhone.")
        XCTAssertEqual(F.status("ios.simulator", F.with { $0.simulator = nil }), .unavailable)
    }

    func testMajorVersionParsing() {
        XCTAssertEqual(PhoneSecurityScanner.majorVersion("18.4.1"), 18)
        XCTAssertEqual(PhoneSecurityScanner.majorVersion(" 26 "), 26)
        XCTAssertEqual(PhoneSecurityScanner.majorVersion("17.0"), 17)
        XCTAssertNil(PhoneSecurityScanner.majorVersion(""))
        XCTAssertNil(PhoneSecurityScanner.majorVersion("beta"))
        XCTAssertNil(PhoneSecurityScanner.majorVersion("0.1"))
        XCTAssertNil(PhoneSecurityScanner.majorVersion("-3.1"))
        XCTAssertNil(PhoneSecurityScanner.majorVersion(nil))
    }

    func testTheNewestIOSPasses() {
        let newest: Int = PhoneSecurityPolicy.newestIOSMajor
        let finding: PhoneSecurityFinding = F.finding("ios.os-version", F.with { $0.osVersion = "\(newest).0.1" })
        XCTAssertEqual(finding.status, .pass)
        XCTAssertEqual(finding.detail, "iOS \(newest).0.1 is current.")
    }

    func testANewerIOSThanKnownPasses() {
        XCTAssertEqual(F.status("ios.os-version", F.with { $0.osVersion = "\(PhoneSecurityPolicy.newestIOSMajor + 1).0" }), .pass)
    }

    func testOneVersionBehindIsAReviewWithUpdateSteps() {
        let newest: Int = PhoneSecurityPolicy.newestIOSMajor
        let finding: PhoneSecurityFinding = F.finding("ios.os-version", F.with { $0.osVersion = "\(newest - 1).7" })
        XCTAssertEqual(finding.status, .review)
        XCTAssertEqual(finding.detail, "iOS \(newest - 1).7 is one version behind iOS \(newest).")
        XCTAssertEqual(finding.fix?.action, .steps("Settings → General → Software Update"))
    }

    func testTwoOrMoreVersionsBehindWarns() {
        let newest: Int = PhoneSecurityPolicy.newestIOSMajor
        let finding: PhoneSecurityFinding = F.finding("ios.os-version", F.with { $0.osVersion = "\(newest - 3).2" })
        XCTAssertEqual(finding.status, .warning)
        XCTAssertEqual(finding.detail, "iOS \(newest - 3).2 is 3 versions behind iOS \(newest).")
        XCTAssertEqual(F.status("ios.os-version", F.with { $0.osVersion = "\(newest - 2).0" }), .warning)
    }

    func testAnUnreadableVersionIsUnavailable() {
        XCTAssertEqual(F.status("ios.os-version", F.with { $0.osVersion = "x" }), .unavailable)
        XCTAssertEqual(F.status("ios.os-version", F.with { $0.osVersion = nil }), .unavailable)
    }

    func testInstallSources() {
        XCTAssertEqual(F.finding("ios.install-source", F.hardened()).detail, "Installed from the App Store.")
        XCTAssertEqual(F.status("ios.install-source", F.with { $0.installSource = .testFlight }), .pass)
        let dev: PhoneSecurityFinding = F.finding("ios.install-source", F.with { $0.installSource = .development })
        XCTAssertEqual(dev.status, .review)
        XCTAssertNotNil(dev.usage)
        XCTAssertEqual(F.status("ios.install-source", F.with { $0.installSource = nil }), .unavailable)
    }

    func testDebugBuildIsAReview() {
        XCTAssertEqual(F.status("ios.debug-build", F.with { $0.debugBuild = true }), .review)
        XCTAssertEqual(F.status("ios.debug-build", F.hardened()), .pass)
        XCTAssertEqual(F.status("ios.debug-build", F.with { $0.debugBuild = nil }), .unavailable)
    }

    func testFileProtectionClasses() {
        for protection in [PhoneFileProtection.complete, .completeUnlessOpen, .completeUntilFirstUserAuthentication] {
            XCTAssertEqual(F.status("ios.data-protection", F.with { $0.fileProtection = protection }), .pass, protection.rawValue)
        }
        XCTAssertEqual(F.status("ios.data-protection", F.with { $0.fileProtection = PhoneFileProtection.none }), .warning)
        XCTAssertEqual(F.status("ios.data-protection", F.with { $0.fileProtection = nil }), .unavailable)
    }

    func testArbitraryLoadsWarn() {
        let finding: PhoneSecurityFinding = F.finding("ios.transport-security", F.with {
            $0.transportSecurity = PhoneTransportSecurity(allowsArbitraryLoads: true, insecureDomains: ["a.example"])
        })
        XCTAssertEqual(finding.status, .warning)
    }

    func testInsecureDomainsAreAReviewListedSorted() {
        let finding: PhoneSecurityFinding = F.finding("ios.transport-security", F.with {
            $0.transportSecurity = PhoneTransportSecurity(insecureDomains: ["b.example", "a.example"])
        })
        XCTAssertEqual(finding.status, .review)
        XCTAssertEqual(finding.detail, "HTTP is allowed for a.example, b.example.")
    }

    func testStrictTransportSecurityPasses() {
        XCTAssertEqual(F.finding("ios.transport-security", F.hardened()).detail, "Every connection must use HTTPS.")
        XCTAssertEqual(F.status("ios.transport-security", F.with { $0.transportSecurity = nil }), .unavailable)
    }

    func testLocalHosts() {
        for host in ["localhost", "LOCALHOST", "dev.localhost", "mac.local", "127.0.0.1", "127.10.20.30", "::1", "[::1]"] {
            XCTAssertTrue(PhoneSecurityScanner.isLocalHost(host), host)
        }
        for host in ["forkmesh.com", "192.168.1.5", "10.0.0.2", "127.0.0", "127.0.0.256", "localhost.example.com", "1.2.3.4"] {
            XCTAssertFalse(PhoneSecurityScanner.isLocalHost(host), host)
        }
    }

    func testEncryptedRelaysPass() {
        for url in ["https://forkmesh.com", "wss://forkmesh.com/api/repo/a/b/ws", "WSS://relay.example:8443/ws"] {
            XCTAssertEqual(F.status("ios.relay-https", F.with { $0.relayURL = url }), .pass, url)
        }
        XCTAssertEqual(F.finding("ios.relay-https", F.hardened()).detail, "The relay at forkmesh.com uses an encrypted connection.")
    }

    func testPlainRelaysAreCritical() {
        for url in ["http://relay.example", "ws://192.168.1.4:8080/ws", "ws://forkmesh.com/api/repo/a/b/ws"] {
            let finding: PhoneSecurityFinding = F.finding("ios.relay-https", F.with { $0.relayURL = url })
            XCTAssertEqual(finding.status, .critical, url)
            XCTAssertNotNil(finding.fix)
        }
        XCTAssertEqual(F.finding("ios.relay-https", F.with { $0.relayURL = "ws://relay.example/ws" }).detail,
                       "The relay at relay.example is unencrypted (ws://).")
    }

    func testPlainLocalRelaysAreAReview() {
        for url in ["http://localhost:8787", "ws://127.0.0.1:8787/ws", "ws://studio.local/ws", "http://[::1]:9000"] {
            XCTAssertEqual(F.status("ios.relay-https", F.with { $0.relayURL = url }), .review, url)
        }
    }

    func testOddRelayAddressesAreUnavailable() {
        for url in [nil, "", "   ", "ftp://relay.example", "not a url", "https://"] as [String?] {
            XCTAssertEqual(F.status("ios.relay-https", F.with { $0.relayURL = url }), .unavailable, url ?? "nil")
        }
    }

    func testScreenCaptureWarns() {
        XCTAssertEqual(F.status("ios.screen-capture", F.with { $0.screenCaptured = true }), .warning)
        XCTAssertEqual(F.status("ios.screen-capture", F.hardened()), .pass)
        XCTAssertEqual(F.status("ios.screen-capture", F.with { $0.screenCaptured = nil }), .unavailable)
    }

    func testAProxyIsAReviewNamingIt() {
        let finding: PhoneSecurityFinding = F.finding("ios.http-proxy", F.with {
            $0.proxy = PhoneProxySettings(httpHost: "10.0.0.9", httpPort: 8888)
        })
        XCTAssertEqual(finding.status, .review)
        XCTAssertEqual(finding.usage, "Proxy: HTTP 10.0.0.9:8888. Keep it only if you or your organisation set it up.")
        XCTAssertEqual(finding.fix?.action, .steps("Settings → Wi-Fi → ⓘ next to your network → Configure Proxy → Off"))
    }

    func testAutomaticProxyIsAReview() {
        let finding: PhoneSecurityFinding = F.finding("ios.http-proxy", F.with {
            $0.proxy = PhoneProxySettings(autoConfigURL: "http://wpad/wpad.dat")
        })
        XCTAssertEqual(finding.status, .review)
        XCTAssertEqual(finding.usage, "Proxy: automatic from http://wpad/wpad.dat. Keep it only if you or your organisation set it up.")
    }

    func testNoProxyPasses() {
        XCTAssertEqual(F.finding("ios.http-proxy", F.hardened()).detail, "No proxy is set.")
        XCTAssertEqual(F.status("ios.http-proxy", F.with { $0.proxy = nil }), .unavailable)
    }

    func testVPNAlwaysPassesAndNamesTheInterface() {
        let on: PhoneSecurityFinding = F.finding("ios.vpn", F.with { $0.vpnInterfaces = ["utun4"] })
        XCTAssertEqual(on.status, .pass)
        XCTAssertEqual(on.detail, "A VPN is on (utun4).")
        XCTAssertEqual(F.finding("ios.vpn", F.hardened()).detail, "No VPN is on.")
        XCTAssertEqual(F.status("ios.vpn", F.with { $0.vpnInterfaces = nil }), .unavailable)
    }

    func testEveryAllowedPermissionIsAReviewThatOpensForkMeshSettings() {
        for permission in PhonePermission.allCases {
            let finding: PhoneSecurityFinding = F.finding("ios.permission.\(permission.rawValue)", F.with {
                $0.permissions[permission] = .allowed
            })
            XCTAssertEqual(finding.status, .review, permission.rawValue)
            XCTAssertEqual(finding.fix?.action, .open(.app), permission.rawValue)
            XCTAssertTrue(finding.usage?.hasPrefix(PhoneSecurityCatalog.permissionUse(permission)) ?? false, permission.rawValue)
        }
    }

    func testFullPhotoAccessSaysSo() {
        let finding: PhoneSecurityFinding = F.finding("ios.permission.photos", F.with { $0.permissions[.photos] = .allowed })
        XCTAssertEqual(finding.detail, "ForkMesh can see every photo and video.")
        XCTAssertEqual(finding.fix?.description, "Choose Limited Access or None under Photos.")
    }

    func testOtherPermissionStatesPass() {
        for state in [PhonePermissionState.limited, .denied, .restricted, .notAsked] {
            for permission in PhonePermission.allCases {
                let finding: PhoneSecurityFinding = F.finding("ios.permission.\(permission.rawValue)", F.with {
                    $0.permissions[permission] = state
                })
                XCTAssertEqual(finding.status, .pass, "\(permission.rawValue) \(state.rawValue)")
                XCTAssertNil(finding.fix)
                XCTAssertNotNil(finding.usage)
            }
        }
    }

    func testAMissingPermissionReadingIsUnavailable() {
        XCTAssertEqual(F.status("ios.permission.camera", F.with { $0.permissions = [:] }), .unavailable)
        XCTAssertEqual(F.finding("ios.permission.camera", F.with { $0.permissions = [:] }).detail,
                       "Could not read ForkMesh's camera access.")
    }

    func testAFixIsDroppedWhenAFindingPasses() {
        let fix = PhoneSecurityFix(label: "x", description: "y", action: .open(.app))
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.passcode
        XCTAssertNil(PhoneSecurityFinding(check: check, status: .pass, detail: "d", fix: fix).fix)
        XCTAssertNil(PhoneSecurityFinding(check: check, status: .unavailable, detail: "d", fix: fix).fix)
        for status in [PhoneSecurityStatus.review, .warning, .critical] {
            XCTAssertEqual(PhoneSecurityFinding(check: check, status: status, detail: "d", fix: fix).fix, fix)
        }
    }

    func testEveryAttentionFixHasALabelAndDescription() {
        var facts: PhoneSecurityFacts = F.jailbroken()
        facts.passcodeSet = false
        facts.notificationPreviews = .always
        facts.osVersion = "15.0"
        facts.relayURL = "ws://relay.example"
        facts.proxy = PhoneProxySettings(httpHost: "p", httpPort: 1)
        facts.permissions = Dictionary(uniqueKeysWithValues: PhonePermission.allCases.map { ($0, PhonePermissionState.allowed) })
        for finding in PhoneSecurityScanner.scan(facts) {
            guard let fix = finding.fix else { continue }
            XCTAssertFalse(fix.label.isEmpty, finding.check.id)
            XCTAssertTrue(fix.description.hasSuffix("."), finding.check.id)
            if case .steps(let steps) = fix.action { XCTAssertTrue(steps.contains("→"), finding.check.id) }
        }
    }
}
