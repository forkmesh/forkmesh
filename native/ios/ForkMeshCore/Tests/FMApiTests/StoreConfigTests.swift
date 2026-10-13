import Foundation
import XCTest

final class StoreConfigTests: XCTestCase {
    static let appBundleId = "com.forkmesh.forkmesh"

    static let iosRoot: URL = {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<4 { url.deleteLastPathComponent() }
        return url
    }()

    static let repoRoot = iosRoot.deletingLastPathComponent().deletingLastPathComponent()

    static let requiredReasonAPIs: [(category: String, tokens: [String])] = [
        ("NSPrivacyAccessedAPICategoryUserDefaults", ["UserDefaults"]),
        ("NSPrivacyAccessedAPICategoryFileTimestamp", [
            "[.modificationDate]", "[.creationDate]", "ModificationDateKey", "creationDateKey",
        ]),
        ("NSPrivacyAccessedAPICategorySystemBootTime", ["systemUptime", "mach_absolute_time"]),
        ("NSPrivacyAccessedAPICategoryDiskSpace", [
            "volumeAvailableCapacity", "volumeTotalCapacity", "systemFreeSize", ".systemSize]", "statfs(",
        ]),
        ("NSPrivacyAccessedAPICategoryActiveKeyboards", ["activeInputModes"]),
    ]

    func text(_ relative: String, in root: URL = iosRoot) throws -> String {
        try String(contentsOf: root.appendingPathComponent(relative), encoding: .utf8)
    }

    func plist(_ relative: String) throws -> [String: Any] {
        let data = try Data(contentsOf: Self.iosRoot.appendingPathComponent(relative))
        let value = try PropertyListSerialization.propertyList(from: data, options: [], format: nil)
        return try XCTUnwrap(value as? [String: Any], relative)
    }

    func appConfigurations() throws -> [String: String] {
        let project = try text("ForkMesh.xcodeproj/project.pbxproj")
        var configurations: [String: String] = [:]
        for chunk in project.components(separatedBy: "isa = XCBuildConfiguration;").dropFirst() {
            let block = chunk.components(separatedBy: "\n\t\t};").first ?? chunk
            guard block.contains("PRODUCT_BUNDLE_IDENTIFIER = \(Self.appBundleId);"),
                  let name = setting("name", in: block) else { continue }
            configurations[name] = block
        }
        return configurations
    }

    func setting(_ key: String, in block: String) -> String? {
        for line in block.components(separatedBy: "\n") {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            guard trimmed.hasPrefix("\(key) = "), trimmed.hasSuffix(";") else { continue }
            let value = trimmed.dropFirst(key.count + 3).dropLast()
            return value.trimmingCharacters(in: CharacterSet(charactersIn: "\""))
        }
        return nil
    }

    func entitlements(of block: String) throws -> String {
        let path = try XCTUnwrap(setting("CODE_SIGN_ENTITLEMENTS", in: block))
        let variant = try XCTUnwrap(setting("FORKMESH_APP_ENTITLEMENTS", in: block))
        return path.replacingOccurrences(of: "$(FORKMESH_APP_ENTITLEMENTS)", with: variant)
    }

    func swiftSources(_ directories: [String]) throws -> String {
        var combined = ""
        for directory in directories {
            let root = Self.iosRoot.appendingPathComponent(directory)
            let enumerator = try XCTUnwrap(FileManager.default.enumerator(atPath: root.path), directory)
            while let relative = enumerator.nextObject() as? String {
                guard relative.hasSuffix(".swift") else { continue }
                combined += try text(relative, in: root)
            }
        }
        return combined
    }

    func testTheAppTargetBuildsTheStoreBundleIdInEveryConfiguration() throws {
        XCTAssertEqual(Set(try appConfigurations().keys), ["Debug", "Release"])
    }

    func testReleaseSignsWithProductionApnsAndDebugWithTheSandbox() throws {
        let configurations = try appConfigurations()
        let release = try entitlements(of: try XCTUnwrap(configurations["Release"]))
        let debug = try entitlements(of: try XCTUnwrap(configurations["Debug"]))
        XCTAssertEqual(release, "Config/ForkMesh-Release.entitlements")
        XCTAssertEqual(debug, "Config/ForkMesh-Debug.entitlements")
        XCTAssertEqual(try plist(release)["aps-environment"] as? String, "production")
        XCTAssertEqual(try plist(debug)["aps-environment"] as? String, "development")
    }

    func testTheAppAndItsNotificationExtensionShareOneKeychainGroup() throws {
        let expected = ["$(AppIdentifierPrefix)\(Self.appBundleId)"]
        for file in [
            "ForkMesh-Release", "ForkMesh-Debug", "NotificationService", "ForkMeshShare", "ForkMeshAutoFill", "ForkMeshWidgets",
        ] {
            let groups = try plist("Config/\(file).entitlements")["keychain-access-groups"] as? [String]
            XCTAssertEqual(groups, expected, file)
        }
    }

    func testEveryKeychainItemStaysOnThisDevice() throws {
        let pattern = try NSRegularExpression(pattern: "kSecAttrAccessible[A-Za-z]+")
        var seen: [String] = []
        let files = FileManager.default.enumerator(at: Self.iosRoot, includingPropertiesForKeys: nil)
        while let url = files?.nextObject() as? URL {
            let name = url.lastPathComponent
            if name == "Tests" || name.hasPrefix(".build") || name.hasSuffix(".xcodeproj") {
                files?.skipDescendants()
                continue
            }
            guard url.pathExtension == "swift" else { continue }
            let source = try String(contentsOf: url, encoding: .utf8)
            for match in pattern.matches(in: source, range: NSRange(source.startIndex..., in: source)) {
                guard let range = Range(match.range, in: source) else { continue }
                let constant = String(source[range])
                seen.append(constant)
                XCTAssertTrue(constant.hasSuffix("ThisDeviceOnly"), "\(url.path): \(constant)")
            }
        }
        XCTAssertTrue(seen.contains("kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly"))
    }

    func testTheServerSendsPushesToThisBundleId() throws {
        let apns = try text("app/src/apns.py", in: Self.repoRoot)
        let line = apns.components(separatedBy: "\n").first { $0.hasPrefix("APNS_BUNDLE_ID = ") }
        XCTAssertEqual(line, "APNS_BUNDLE_ID = \"\(Self.appBundleId)\"")
    }

    func testTheShareExtensionShipsWithoutAnUnprovisionedAppGroup() throws {
        let share = try plist("Config/ForkMeshShare.entitlements")
        XCTAssertNil(share["com.apple.security.application-groups"])
        XCTAssertTrue(try text("ForkMesh.xcodeproj/project.pbxproj").contains("ForkMeshShare.appex"))
    }

    func testTheAutoFillExtensionShipsWithoutAnUnprovisionedAppGroup() throws {
        let autofill = try plist("Config/ForkMeshAutoFill.entitlements")
        XCTAssertNil(autofill["com.apple.security.application-groups"])
        let key = "com.apple.developer.authentication-services.autofill-credential-provider"
        for file in ["ForkMeshAutoFill", "ForkMesh-Release", "ForkMesh-Debug"] {
            XCTAssertEqual(try plist("Config/\(file).entitlements")[key] as? Bool, true, file)
        }
        let project = try text("ForkMesh.xcodeproj/project.pbxproj")
        XCTAssertTrue(project.contains("ForkMeshAutoFill.appex in Embed Foundation Extensions"))
        XCTAssertTrue(project.contains("PRODUCT_BUNDLE_IDENTIFIER = \(Self.appBundleId).AutoFill;"))
        let ids = try text("tools/asc_bundle_ids.py")
        XCTAssertTrue(ids.contains("(APP_BUNDLE_ID + \".AutoFill\", \"ForkMesh AutoFill\", [\"AUTOFILL_CREDENTIAL_PROVIDER\"]),"))
        let info = try plist("Config/ForkMeshAutoFill-Info.plist")
        let attributes = (info["NSExtension"] as? [String: Any])?["NSExtensionAttributes"] as? [String: Any]
        let capabilities = try XCTUnwrap(attributes?["ASCredentialProviderExtensionCapabilities"] as? [String: Any])
        XCTAssertEqual(capabilities["ProvidesPasswords"] as? Bool, true)
    }

    func testTheWidgetsAndTheWatchAppShipWithoutAnUnprovisionedAppGroup() throws {
        for file in ["ForkMesh-Release", "ForkMesh-Debug", "ForkMeshWidgets", "ForkMeshWatch", "ForkMeshWatchWidgets"] {
            XCTAssertNil(try plist("Config/\(file).entitlements")["com.apple.security.application-groups"], file)
        }
        let watchGroup = ["$(AppIdentifierPrefix)\(Self.appBundleId).watchkitapp"]
        for file in ["ForkMeshWatch", "ForkMeshWatchWidgets"] {
            XCTAssertEqual(try plist("Config/\(file).entitlements")["keychain-access-groups"] as? [String], watchGroup, file)
        }
        let project = try text("ForkMesh.xcodeproj/project.pbxproj")
        XCTAssertTrue(project.contains("ForkMeshWidgets.appex in Embed Foundation Extensions"))
        XCTAssertTrue(project.contains("ForkMeshWatch.app in Embed Watch Content"))
        XCTAssertTrue(project.contains("ForkMeshWatchWidgets.appex in Embed Foundation Extensions"))
        let info = try plist("Config/ForkMesh-Info.plist")
        XCTAssertEqual(info["NSSupportsLiveActivities"] as? Bool, true)
        let workflows = try text("codemagic.yaml", in: Self.repoRoot)
        XCTAssertFalse(workflows.contains("FORKMESH_NATIVE_WATCH: \"0\""))
        XCTAssertFalse(workflows.contains("FORKMESH_NATIVE_WIDGETS: \"0\""))
    }

    func testTheAppAnswersExportCompliance() throws {
        let info = try plist("Config/ForkMesh-Info.plist")
        XCTAssertNotNil(info["ITSAppUsesNonExemptEncryption"] as? Bool)
    }

    func testOnDeviceRepositoriesShowInTheFilesApp() throws {
        let info = try plist("Config/ForkMesh-Info.plist")
        XCTAssertEqual(info["UIFileSharingEnabled"] as? Bool, true)
        XCTAssertEqual(info["LSSupportsOpeningDocumentsInPlace"] as? Bool, true)
    }

    func testTheIPhoneLayoutIsLockedToPortrait() throws {
        let info = try plist("Config/ForkMesh-Info.plist")
        XCTAssertEqual(info["UISupportedInterfaceOrientations"] as? [String], ["UIInterfaceOrientationPortrait"])
        XCTAssertEqual((info["UISupportedInterfaceOrientations~ipad"] as? [String])?.count, 4)
    }

    func testNoPrivacyManifestIsExcludedFromItsBundle() throws {
        XCTAssertFalse(try text("ForkMesh.xcodeproj/project.pbxproj").contains("PrivacyInfo.xcprivacy"))
    }

    func testEachPrivacyManifestDeclaresTheRequiredReasonAPIsItsBundleUses() throws {
        let bundles: [(manifest: String, sources: [String])] = [
            ("ForkMesh/Resources/PrivacyInfo.xcprivacy", ["ForkMesh", "Shared", "SharedSurfaces", "ForkMeshCore/Sources"]),
            ("NotificationService/PrivacyInfo.xcprivacy", ["NotificationService", "Shared"]),
            ("ForkMeshWidgets/PrivacyInfo.xcprivacy", ["ForkMeshWidgets", "SharedSurfaces", "ForkMeshCore/Sources"]),
            ("ForkMeshWatch/PrivacyInfo.xcprivacy", [
                "ForkMeshWatch", "ForkMeshCore/Sources/FMBase", "ForkMeshCore/Sources/FMAgentWatch",
                "ForkMeshCore/Sources/FMWatchWire",
            ]),
            ("ForkMeshWatchWidgets/PrivacyInfo.xcprivacy", [
                "ForkMeshWatchWidgets", "ForkMeshCore/Sources/FMBase", "ForkMeshCore/Sources/FMAgentWatch",
                "ForkMeshCore/Sources/FMWatchWire",
            ]),
            ("ForkMeshShare/PrivacyInfo.xcprivacy", ["ForkMeshShare", "ForkMeshCore/Sources/FMSurfaces", "ForkMeshCore/Sources/FMBase"]),
            ("ForkMeshAutoFill/PrivacyInfo.xcprivacy", [
                "ForkMeshAutoFill", "ForkMeshCore/Sources/FMVault", "ForkMeshCore/Sources/FMCrypto", "ForkMeshCore/Sources/FMBase",
                "ForkMeshCore/Sources/FMNet",
            ]),
        ]
        for bundle in bundles {
            let manifest = try plist(bundle.manifest)
            XCTAssertEqual(manifest["NSPrivacyTracking"] as? Bool, false, bundle.manifest)
            let declared = (manifest["NSPrivacyAccessedAPITypes"] as? [[String: Any]] ?? []).filter {
                !($0["NSPrivacyAccessedAPITypeReasons"] as? [String] ?? []).isEmpty
            }.compactMap { $0["NSPrivacyAccessedAPIType"] as? String }
            let sources = try swiftSources(bundle.sources)
            for api in Self.requiredReasonAPIs {
                guard let token = api.tokens.first(where: sources.contains) else { continue }
                XCTAssertTrue(declared.contains(api.category), "\(bundle.manifest): \(token) needs \(api.category)")
            }
        }
    }

    func testNoAppIntentDescriptionNamesAnAppleDevice() throws {
        let sources = try swiftSources([
            "ForkMesh", "Shared", "SharedSurfaces", "ForkMeshWidgets", "ForkMeshWatch", "ForkMeshWatchWidgets",
        ])
        let pattern = try NSRegularExpression(pattern: "IntentDescription\\(\\s*\"([^\"]*)\"")
        let range = NSRange(sources.startIndex..., in: sources)
        for match in pattern.matches(in: sources, range: range) {
            let description = String(sources[try XCTUnwrap(Range(match.range(at: 1), in: sources))])
            for word in ["iphone", "ipad", "apple watch"] {
                XCTAssertFalse(description.lowercased().contains(word), "App Intent description names a device: \(description)")
            }
        }
    }
}
