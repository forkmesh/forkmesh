import FMTools
import Foundation
import XCTest

final class PhoneSecurityReportTests: XCTestCase {
    private typealias F = PhoneSecurityFixtures

    private let specIds: [String] = [
        "ios.passcode", "ios.biometrics", "ios.notification-previews", "ios.jailbreak-apps", "ios.jailbreak-tools",
        "ios.rootless-jailbreak", "ios.tweak-frameworks", "ios.injected-libraries", "ios.dyld-environment", "ios.sandbox",
        "ios.system-links", "ios.debugger", "ios.simulator", "ios.os-version", "ios.install-source", "ios.debug-build",
        "ios.data-protection", "ios.transport-security", "ios.relay-https", "ios.screen-capture", "ios.http-proxy", "ios.vpn",
        "ios.permission.camera", "ios.permission.microphone", "ios.permission.photos", "ios.permission.speech",
        "ios.permission.bluetooth", "ios.permission.motion",
    ]

    private func check(_ id: String, _ title: String, _ category: PhoneSecurityCategory = .system) -> PhoneSecurityCheck {
        PhoneSecurityCheck(id: id, title: title, category: category)
    }

    private func mixed() -> PhoneSecurityReport {
        PhoneSecurityReport(findings: [
            PhoneSecurityFinding(check: check("a", "Alpha"), status: .pass, detail: "fine"),
            PhoneSecurityFinding(check: check("b", "Bravo", .network), status: .review, detail: "look", usage: "Proxy: HTTP 10.0.0.1",
                                 fix: PhoneSecurityFix(label: "l", description: "d.", action: .steps("A → B"))),
            PhoneSecurityFinding(check: check("c", "Charlie", .privacy), status: .review, detail: "camera",
                                 fix: PhoneSecurityFix(label: "l", description: "d.", action: .open(.app))),
            PhoneSecurityFinding(check: check("d", "Delta", .lockScreen), status: .critical, detail: "bad"),
            PhoneSecurityFinding(check: check("e", "Echo"), status: .warning, detail: "hmm"),
            PhoneSecurityFinding(check: check("f", "Foxtrot"), status: .unavailable, detail: "unknown"),
        ])
    }

    func testTheCatalogMatchesTheSpecExactly() {
        XCTAssertEqual(PhoneSecurityCatalog.all.map(\.id), specIds)
    }

    func testCheckIdsAreUniqueAndPrefixed() {
        let ids: [String] = PhoneSecurityCatalog.all.map(\.id)
        XCTAssertEqual(Set(ids).count, ids.count)
        XCTAssertTrue(ids.allSatisfy { $0.hasPrefix("ios.") })
        XCTAssertEqual(Set(PhoneSecurityCatalog.all.map(\.title)).count, ids.count)
    }

    func testEveryCheckIsExplained() {
        for check in PhoneSecurityCatalog.all {
            XCTAssertGreaterThan(check.explanation.count, 60, check.id)
            XCTAssertTrue(check.explanation.hasSuffix("."), check.id)
        }
        XCTAssertEqual(Set(PhoneSecurityExplanations.texts.keys), Set(specIds))
    }

    func testEveryCategoryHasChecks() {
        for category in PhoneSecurityCategory.allCases {
            XCTAssertTrue(PhoneSecurityCatalog.all.contains { $0.category == category }, category.rawValue)
        }
        XCTAssertEqual(PhoneSecurityReport(facts: F.hardened()).categories, PhoneSecurityCategory.allCases)
    }

    func testCheckLookup() {
        XCTAssertEqual(PhoneSecurityCatalog.check(id: "ios.vpn")?.title, "VPN")
        XCTAssertNil(PhoneSecurityCatalog.check(id: "android.vpn"))
    }

    func testThePathListsHaveNoDuplicates() {
        for list in [PhoneSecurityPaths.jailbreakApps, PhoneSecurityPaths.jailbreakTools, PhoneSecurityPaths.rootlessJailbreak,
                     PhoneSecurityPaths.tweakFrameworks, PhoneSecurityPaths.systemLinks] {
            XCTAssertEqual(Set(list).count, list.count)
            XCTAssertTrue(list.allSatisfy { $0.hasPrefix("/") })
        }
        XCTAssertEqual(PhoneSecurityPaths.probed.count,
                       PhoneSecurityPaths.jailbreakApps.count + PhoneSecurityPaths.jailbreakTools.count
                           + PhoneSecurityPaths.rootlessJailbreak.count + PhoneSecurityPaths.tweakFrameworks.count)
        XCTAssertTrue(PhoneSecurityPaths.injectedImageMarkers.allSatisfy { $0 == $0.lowercased() })
    }

    func testEveryCheckProducesExactlyOneFinding() {
        for facts in [PhoneSecurityFacts(), F.hardened(), F.jailbroken()] {
            XCTAssertEqual(Set(PhoneSecurityScanner.scan(facts).map(\.check.id)), Set(specIds))
            XCTAssertEqual(PhoneSecurityScanner.scan(facts).count, specIds.count)
        }
    }

    func testFindingsSortBySeverityThenTitle() {
        let report: PhoneSecurityReport = mixed()
        XCTAssertEqual(report.findings.map(\.check.id), ["d", "e", "b", "c", "f", "a"])
    }

    func testScanResultsAreSorted() {
        let findings: [PhoneSecurityFinding] = PhoneSecurityScanner.scan(F.jailbroken())
        let ranks: [Int] = findings.map(\.status.rank)
        XCTAssertEqual(ranks, ranks.sorted())
        XCTAssertEqual(findings.first?.status, .critical)
    }

    func testStatusRanksAndLabels() {
        XCTAssertEqual(PhoneSecurityStatus.allCases.sorted { $0.rank < $1.rank }, [.critical, .warning, .review, .unavailable, .pass])
        XCTAssertEqual(PhoneSecurityStatus.allCases.map(\.label), ["Pass", "Review", "Warning", "Critical", "Unavailable"])
        XCTAssertEqual(PhoneSecurityStatus.allCases.filter(\.needsAttention), [.review, .warning, .critical])
    }

    func testFilterNamesMatchTheDesktop() {
        XCTAssertEqual(PhoneSecurityFilter.allCases.map(\.label),
                       ["All", "Needs attention", "Critical", "Warnings", "For review", "Can fix", "Passed", "Unavailable"])
    }

    func testFilterCounts() {
        let report: PhoneSecurityReport = mixed()
        XCTAssertEqual(report.count(.all), 6)
        XCTAssertEqual(report.count(.attention), 4)
        XCTAssertEqual(report.count(.critical), 1)
        XCTAssertEqual(report.count(.warning), 1)
        XCTAssertEqual(report.count(.review), 2)
        XCTAssertEqual(report.count(.fixable), 1)
        XCTAssertEqual(report.count(.pass), 1)
        XCTAssertEqual(report.count(.unavailable), 1)
    }

    func testCanFixMeansASettingsButton() {
        let report: PhoneSecurityReport = mixed()
        XCTAssertEqual(report.visible(filter: .fixable).map(\.check.id), ["c"])
    }

    func testFiltersByCategory() {
        let report: PhoneSecurityReport = mixed()
        XCTAssertEqual(report.visible(category: .network).map(\.check.id), ["b"])
        XCTAssertEqual(report.visible(filter: .review, category: .privacy).map(\.check.id), ["c"])
        XCTAssertEqual(report.visible(filter: .critical, category: .privacy).map(\.check.id), [])
        XCTAssertEqual(report.categories, [.lockScreen, .system, .network, .privacy])
    }

    func testSearchesTitleCategoryStatusDetailAndUsage() {
        let report: PhoneSecurityReport = mixed()
        XCTAssertEqual(report.visible(query: "charlie").map(\.check.id), ["c"])
        XCTAssertEqual(report.visible(query: "LOCK SCREEN").map(\.check.id), ["d"])
        XCTAssertEqual(report.visible(query: "critical").map(\.check.id), ["d"])
        XCTAssertEqual(report.visible(query: "camera").map(\.check.id), ["c"])
        XCTAssertEqual(report.visible(query: "10.0.0.1").map(\.check.id), ["b"])
        XCTAssertEqual(report.visible(query: "  ").count, 6)
        XCTAssertEqual(report.visible(query: "zzz").count, 0)
    }

    func testSummaryMatchesTheDesktopWording() {
        XCTAssertEqual(mixed().summary, "6 checks: 1 pass, 2 need review, 1 warnings, 1 critical, 1 unavailable")
    }

    func testFilteredSummaryWithoutScopeIsTheSummary() {
        let report: PhoneSecurityReport = mixed()
        XCTAssertEqual(report.filteredSummary(), report.summary)
        XCTAssertEqual(report.filteredSummary(query: "   "), report.summary)
    }

    func testFilteredSummaryDescribesTheScope() {
        let report: PhoneSecurityReport = mixed()
        let summary: String = report.summary
        XCTAssertEqual(report.filteredSummary(filter: .review), "Showing 2 of 6 checks in For review. \(summary)")
        XCTAssertEqual(report.filteredSummary(query: "alpha"), "Showing 1 of 6 checks matching \"alpha\". \(summary)")
        XCTAssertEqual(report.filteredSummary(filter: .review, category: .network, query: "bravo"),
                       "Showing 1 of 6 checks in For review · Network matching \"bravo\". \(summary)")
        XCTAssertEqual(report.filteredSummary(category: .privacy), "Showing 1 of 6 checks in Privacy. \(summary)")
    }

    func testEmptyFilteredSummariesNameTheEmptyScope() {
        let report: PhoneSecurityReport = mixed()
        XCTAssertEqual(report.filteredSummary(query: "zzz"), "No checks match \"zzz\". Try a different search.")
        XCTAssertEqual(report.filteredSummary(filter: .critical, category: .privacy),
                       "No checks in Critical · Privacy. Try a different filter.")
        XCTAssertEqual(report.filteredSummary(filter: .pass, query: "zzz"),
                       "No checks in Passed match \"zzz\". Try a different filter or search.")
    }

    func testHeadlinePicksTheWorstResult() {
        XCTAssertEqual(mixed().headline, "1 critical problem")
        XCTAssertEqual(mixed().worst, .critical)
        XCTAssertEqual(PhoneSecurityReport(facts: F.jailbroken()).headline, "8 critical problems")
        XCTAssertEqual(PhoneSecurityReport(facts: F.hardened()).headline, "No problems found")
        XCTAssertNil(PhoneSecurityReport(facts: F.hardened()).worst)
        XCTAssertEqual(PhoneSecurityReport(facts: F.with { $0.debuggerAttached = true }).headline, "1 warning")
        XCTAssertEqual(PhoneSecurityReport(facts: F.with {
            $0.screenCaptured = true
            $0.debuggerAttached = true
        }).headline, "2 warnings")
        XCTAssertEqual(PhoneSecurityReport(facts: F.with { $0.simulator = true }).headline, "1 thing to review")
        XCTAssertEqual(PhoneSecurityReport(facts: F.with {
            $0.simulator = true
            $0.debugBuild = true
        }).headline, "2 things to review")
    }

    func testAReportSortsWhateverItIsGiven() {
        let report = PhoneSecurityReport(findings: mixed().findings.reversed())
        XCTAssertEqual(report.findings.map(\.check.id), ["d", "e", "b", "c", "f", "a"])
    }

    func testProxyDictionaryParsing() {
        let proxy: PhoneProxySettings = PhoneProxySettings.parse([
            "HTTPEnable": NSNumber(value: 1), "HTTPProxy": " proxy.corp ", "HTTPPort": NSNumber(value: 3128),
            "HTTPSEnable": 1, "HTTPSProxy": "secure.corp", "HTTPSPort": 443,
            "ProxyAutoConfigEnable": true, "ProxyAutoConfigURLString": "http://wpad/wpad.dat",
        ])
        XCTAssertEqual(proxy, PhoneProxySettings(httpHost: "proxy.corp", httpPort: 3128, httpsHost: "secure.corp", httpsPort: 443,
                                                 autoConfigURL: "http://wpad/wpad.dat"))
        XCTAssertEqual(proxy.summary, "HTTP proxy.corp:3128, HTTPS secure.corp:443, automatic from http://wpad/wpad.dat")
    }

    func testDisabledOrBlankProxyEntriesAreIgnored() {
        let proxy: PhoneProxySettings = PhoneProxySettings.parse([
            "HTTPEnable": 0, "HTTPProxy": "proxy.corp", "HTTPSEnable": 1, "HTTPSProxy": "  ",
            "ProxyAutoConfigURLString": "http://wpad/wpad.dat", "ExceptionsList": ["*.local"],
        ])
        XCTAssertTrue(proxy.isEmpty)
        XCTAssertTrue(PhoneProxySettings.parse([:]).isEmpty)
    }

    func testProxyWithoutAPortNamesOnlyTheHost() {
        XCTAssertEqual(PhoneProxySettings(httpHost: "p.corp").summary, "HTTP p.corp")
        XCTAssertEqual(PhoneProxySettings(httpHost: "p.corp", httpPort: 0).summary, "HTTP p.corp")
    }

    func testVPNInterfacesFromScopedProxyKeys() {
        XCTAssertEqual(PhoneProxySettings.vpnInterfaces(scoped: ["en0", "pdp_ip0", "utun3", "ipsec0", "lo0", "ppp0", "tap1", "tun2"]),
                       ["ipsec0", "ppp0", "tap1", "tun2", "utun3"])
        XCTAssertEqual(PhoneProxySettings.vpnInterfaces(scoped: ["en0", "en2", "pdp_ip1", "awdl0", "llw0"]), [])
        XCTAssertEqual(PhoneProxySettings.vpnInterfaces(scoped: ["UTUN1"]), ["UTUN1"])
    }

    func testFactsKnowWhenSomeoneIsDeveloping() {
        XCTAssertFalse(F.hardened().developing)
        XCTAssertTrue(F.with { $0.debugBuild = true }.developing)
        XCTAssertTrue(F.with { $0.simulator = true }.developing)
        XCTAssertFalse(PhoneSecurityFacts().developing)
    }

    func testBiometryLabels() {
        XCTAssertEqual(PhoneBiometry.faceID.label, "Face ID")
        XCTAssertEqual(PhoneBiometry.touchID.label, "Touch ID")
        XCTAssertEqual(PhoneBiometry.opticID.label, "Optic ID")
    }

    func testPermissionTitlesAndUses() {
        for permission in PhonePermission.allCases {
            XCTAssertFalse(PhoneSecurityCatalog.permissionTitle(permission).isEmpty)
            XCTAssertTrue(PhoneSecurityCatalog.permissionUse(permission).hasPrefix("ForkMesh uses"), permission.rawValue)
            XCTAssertTrue(PhoneSecurityCatalog.permissionUse(permission).hasSuffix("."), permission.rawValue)
        }
    }

    func testFixActionsKnowWhetherTheyOpenSettings() {
        XCTAssertTrue(PhoneSecurityFixAction.open(.app).opensSettings)
        XCTAssertTrue(PhoneSecurityFixAction.open(.appNotifications).opensSettings)
        XCTAssertFalse(PhoneSecurityFixAction.steps("A → B").opensSettings)
    }
}
