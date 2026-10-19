import FMApi
import XCTest

@testable import FMPackages

final class PluginMenuTests: XCTestCase {
    func testAPluginHomeScreenNamesItsPackage() {
        XCTAssertEqual(PluginMenu.packageId(for: .crm(pipeline: nil)), "crm")
        XCTAssertEqual(PluginMenu.packageId(for: .crmOrg(id: "acme")), "crm")
        XCTAssertEqual(PluginMenu.packageId(for: .rssReader), "rss-reader")
        XCTAssertEqual(PluginMenu.packageId(for: .secureDrive), "secure-drive")
        XCTAssertEqual(PluginMenu.packageId(for: .faceChanger), "face-changer")
        XCTAssertEqual(PluginMenu.packageId(for: .videoMixDjStudio), "video-mix-dj-studio")
        XCTAssertEqual(PluginMenu.packageId(for: .appStoreStatus), "app-store-status")
        XCTAssertEqual(PluginMenu.packageId(for: .usage), "usage")
        XCTAssertEqual(PluginMenu.packageId(for: .runners), "runners")
        XCTAssertTrue(PluginMenu.addsButton(for: .runners))
        XCTAssertEqual(PluginMenu.packageId(for: .instanceFeed(post: nil)), "instance-feed")
        XCTAssertEqual(PluginMenu.packageId(for: .marketing(section: "email")), "marketing")
        XCTAssertEqual(PluginMenu.packageId(for: .ledger(section: nil)), "ledger")
        XCTAssertEqual(PluginMenu.packageId(for: .calendar(date: nil, mode: .month)), "calendar")
        XCTAssertEqual(PluginMenu.packageId(for: .status), "status-page")
        XCTAssertEqual(PluginMenu.packageId(for: .localAI), "on-device-ai")
        XCTAssertEqual(PluginMenu.packageId(for: .logs), "logs")
        XCTAssertEqual(PluginMenu.packageId(for: .diagnostics), "diagnostics")
        XCTAssertEqual(PluginMenu.packageId(for: .phoneSecurity), "phone-security")
        XCTAssertEqual(PluginMenu.packageId(for: .eyeControl), "eye-control")
        XCTAssertEqual(PluginMenu.packageId(for: .ember(nil)), "ember")
        XCTAssertEqual(PluginMenu.packageId(for: .strata(RepoRef(owner: "a", name: "b"))), "strata")
        XCTAssertEqual(PluginMenu.packageId(for: .synapse(nil)), "synapse")
        XCTAssertEqual(PluginMenu.packageId(for: .openWrtRouter), "openwrt")
        XCTAssertEqual(PluginMenu.packageId(for: .fileExplorer), "files")
    }

    func testEveryHomeKindBelongsToAMarketplacePackage() {
        for kind in PluginMenu.homeKinds {
            let package = PackageGate.kinds[kind]
            XCTAssertNotNil(package, "\(kind)")
            XCTAssertTrue(PackageGate.isGated(package ?? ""), "\(kind)")
        }
    }

    func testEveryPhoneScreenAPackageOpensGetsTheMenuExceptTheTabsAndTheTour() {
        let tabs: Set<Route.Kind> = [.agents, .inbox, .chat, .tour]
        for id in MarketPackage.nativeDestinations.keys {
            guard let route = PackageRoutes.route(for: MarketPackage(id: id, name: id)) else { continue }
            if tabs.contains(route.kind) {
                XCTAssertNil(PluginMenu.packageId(for: route), id)
            } else {
                XCTAssertNotNil(PluginMenu.packageId(for: route), id)
            }
        }
    }

    func testDetailScreensTabsAndTheMarketGetNoPluginMenu() {
        XCTAssertNil(PluginMenu.packageId(for: .contact(id: "1")))
        XCTAssertNil(PluginMenu.packageId(for: .note(id: "1")))
        XCTAssertNil(PluginMenu.packageId(for: .cove(id: "x")))
        XCTAssertNil(PluginMenu.packageId(for: .chat))
        XCTAssertNil(PluginMenu.packageId(for: .inbox(.all)))
        XCTAssertNil(PluginMenu.packageId(for: .allAgents))
        XCTAssertNil(PluginMenu.packageId(for: .market(filter: nil)))
        XCTAssertNil(PluginMenu.packageId(for: .package(id: "crm")))
        XCTAssertNil(PluginMenu.packageId(for: .settings(section: nil)))
        XCTAssertNil(PluginMenu.packageId(for: .tour))
        XCTAssertFalse(PluginMenu.addsButton(for: .package(id: "crm")))
    }

    func testScreensWithTheirOwnMoreMenuGetNoSecondButton() {
        let repo = RepoRef(owner: "jett", name: "site")
        XCTAssertTrue(PluginMenu.screenOwnsMenu(.crm(pipeline: nil)))
        XCTAssertTrue(PluginMenu.screenOwnsMenu(.crmOrg(id: "acme")))
        XCTAssertTrue(PluginMenu.screenOwnsMenu(.teammates))
        XCTAssertTrue(PluginMenu.screenOwnsMenu(.coves(repo)))
        XCTAssertFalse(PluginMenu.addsButton(for: .crm(pipeline: nil)))
        XCTAssertFalse(PluginMenu.addsButton(for: .teammates))
        XCTAssertFalse(PluginMenu.addsButton(for: .coves(repo)))
        XCTAssertTrue(PluginMenu.addsButton(for: .coves(nil)))
        XCTAssertTrue(PluginMenu.addsButton(for: .rssReader))
        XCTAssertTrue(PluginMenu.addsButton(for: .secureDrive))
        XCTAssertTrue(PluginMenu.addsButton(for: .calendar(date: nil, mode: .month)))
        let owners: [Route] = [.crm(pipeline: nil), .crmOrg(id: "acme"), .teammates, .coves(repo)]
        for route in owners {
            XCTAssertNotNil(PluginMenu.packageId(for: route), "\(route)")
        }
    }

    func testTheMenuOpensOnePopupForRatingReportingAndUninstalling() {
        XCTAssertEqual(PluginMenu.buttonIdentifier, "plugin-more")
        XCTAssertEqual(PluginMenu.optionsLabel, "Rate, report or uninstall\u{2026}")
        XCTAssertEqual(PluginMenu.feedbackLabel, "Send feedback or report a bug")
    }

    func testThePopupSendsARatingAReportOrBoth() {
        XCTAssertFalse(PluginFeedback().canSend)
        XCTAssertFalse(PluginFeedback(text: "   ").canSend)
        XCTAssertFalse(PluginFeedback(text: "  ").canAskAgent)
        let rated = PluginFeedback(stars: 5)
        XCTAssertTrue(rated.canSend)
        XCTAssertTrue(rated.rates)
        XCTAssertFalse(rated.reports)
        XCTAssertFalse(rated.canAskAgent)
        let written = PluginFeedback(text: "Crashes on open")
        XCTAssertTrue(written.canSend)
        XCTAssertFalse(written.rates)
        XCTAssertTrue(written.canAskAgent)
    }

    func testFiveStarsKeepTheEarlierReviewAndFewerStarsUseTheTypedText() {
        let rules = ReviewRules()
        let earlier = ReviewDraft(stars: 3, problem: "old problem", improvement: "old idea", comment: "kept")
        XCTAssertNil(PluginFeedback(text: "words").review(earlier, rules: rules))
        let five = PluginFeedback(stars: 5, text: "Love it").review(earlier, rules: rules)
        XCTAssertEqual(five?.stars, 5)
        XCTAssertEqual(five?.problem, "old problem")
        XCTAssertFalse(PluginFeedback(stars: 5).asksImprovement(rules))
        let three = PluginFeedback(stars: 2, text: "  The sync stops  ", improvement: " Retry it ").review(earlier, rules: rules)
        XCTAssertEqual(three?.stars, 2)
        XCTAssertEqual(three?.problem, "The sync stops")
        XCTAssertEqual(three?.improvement, "Retry it")
        XCTAssertEqual(three?.comment, "kept")
        XCTAssertTrue(PluginFeedback(stars: 2).asksImprovement(rules))
        XCTAssertEqual(PluginFeedback(stars: 2).review(earlier, rules: rules)?.problem, "old problem")
    }

    func testThanksSayWhatWasSent() {
        XCTAssertEqual(PluginFeedback.thanks("CRM", rated: true, reported: false), "Thanks for rating CRM.")
        XCTAssertEqual(
            PluginFeedback.thanks("CRM", rated: false, reported: true), "Thanks \u{2014} your feedback on CRM reached the ForkMesh team.")
        XCTAssertEqual(
            PluginFeedback.thanks("CRM", rated: true, reported: true),
            "Thanks \u{2014} your rating and feedback on CRM reached the ForkMesh team.")
    }

    func testTheAgentPromptNamesThePluginAndCarriesTheText() {
        let prompt = PluginFeedback(stars: 2, text: " Export is empty ").agentPrompt(
            name: "Ledger", packageId: "ledger", screen: "Ledger", device: "iPhone")
        XCTAssertTrue(prompt.hasPrefix("Feedback on the ForkMesh plugin Ledger (package `ledger`), sent from the Ledger screen of the iPhone app:"))
        XCTAssertTrue(prompt.contains("\n\nExport is empty\n\nRated 2 of 5 stars.\n\n"))
        XCTAssertFalse(PluginFeedback(text: "x").agentPrompt(name: "A", packageId: "a", screen: "A", device: "iPhone").contains("Rated"))
    }

    func testLabelsUseTheCatalogNameAndFallBackToTheRouteTitle() {
        XCTAssertEqual(PluginMenu.name(catalog: "RSS Reader", route: .rssReader), "RSS Reader")
        XCTAssertEqual(PluginMenu.name(catalog: "  ", route: .crm(pipeline: nil)), "CRM")
        XCTAssertEqual(PluginMenu.name(catalog: nil, route: .crmOrg(id: "acme")), "CRM")
        XCTAssertEqual(PluginMenu.rateLabel("CRM", rated: false), "Rate CRM")
        XCTAssertEqual(PluginMenu.rateLabel("CRM", rated: true), "Edit your rating of CRM")
    }

    func testEveryPackageSharesItsPublicMarketLink() {
        XCTAssertEqual(PluginMenu.shareURL(packageId: "crm")?.absoluteString, "https://forkmesh.com/market/crm")
        XCTAssertEqual(
            PluginMenu.shareURL(packageId: "video-mix-dj-studio")?.absoluteString,
            "https://forkmesh.com/market/video-mix-dj-studio")
        XCTAssertEqual(PluginMenu.shareURL(packageId: " RSS-Reader ")?.absoluteString, "https://forkmesh.com/market/rss-reader")
        XCTAssertEqual(PluginMenu.shareURL(packageId: "acme.tools")?.absoluteString, "https://forkmesh.com/market/acme.tools")
        XCTAssertNil(PluginMenu.shareURL(packageId: ""))
        XCTAssertNil(PluginMenu.shareURL(packageId: "previews"))
        XCTAssertNil(PluginMenu.shareURL(packageId: "../crm"))
        XCTAssertNil(PluginMenu.shareURL(packageId: "crm/reviews"))
        XCTAssertNil(PluginMenu.shareURL(packageId: "9lives"))
        XCTAssertNil(PluginMenu.shareURL(packageId: String(repeating: "a", count: 97)))
    }

    func testASharedLinkOpensThePackageItWasSharedFrom() {
        for id in ["crm", "rss-reader", "face-changer", "acme.tools"] {
            let url = PluginMenu.shareURL(packageId: id)
            XCTAssertNotNil(url, id)
            XCTAssertEqual(url.flatMap { RouteParser.parse($0)?.stack }, [.market(filter: nil), .package(id: id)], id)
        }
    }

    func testShareLabelsMatchTheMenuStyle() {
        XCTAssertEqual(PluginMenu.shareLabel, "Share\u{2026}")
        XCTAssertEqual(PluginMenu.copyLinkLabel, "Copy link")
        XCTAssertEqual(PluginMenu.linkCopiedNotice, "Link copied")
        XCTAssertEqual(PluginMenu.shareSubject("CRM"), "CRM on ForkMesh")
        XCTAssertEqual(PluginMenu.shareMessage("CRM"), "Get CRM from the ForkMesh Market.")
    }
}
