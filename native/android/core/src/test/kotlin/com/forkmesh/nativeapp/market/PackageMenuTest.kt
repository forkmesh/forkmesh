package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class PackageMenuTest {
    fun testAPluginScreenOpenedByItsRouteKnowsWhichPackageItBelongsTo() {
        assertEq("notes", PackageMenu.packageFor(Route.Kind.Notes))
        assertEq("notes", PackageMenu.packageFor(Route.Kind.Note))
        assertEq(PackageGate.CRM, PackageMenu.packageFor(Route.Kind.Contact))
        assertEq("face-changer", PackageMenu.packageFor(Route.Kind.FaceChanger))
        assertEq("special-agents", PackageMenu.packageFor(Route.Kind.Teammates))
        assertEq(PackageGate.ISSUES, PackageMenu.packageFor(Route.Kind.Issue))
    }

    fun testTabRootsAndSharedRepoPagesCarryNoPluginMenu() {
        for (kind in listOf(Route.Kind.Agents, Route.Kind.Inbox, Route.Kind.Chat, Route.Kind.Compose, Route.Kind.Commit, Route.Kind.Commits)) {
            expect(PackageGate.ROUTE_KINDS.containsKey(kind), "$kind")
            assertEq(null, PackageMenu.packageFor(kind), "$kind")
        }
        assertEq(null, PackageMenu.packageFor(Route.Kind.Market))
        assertEq(null, PackageMenu.packageFor(Route.Kind.Package))
        assertEq(null, PackageMenu.packageFor(Route.Kind.Settings))
    }

    fun testEveryTaggedPackageIsAGatedMarketplacePackage() {
        Route.Kind.entries.mapNotNull(PackageMenu::packageFor).forEach { expect(it in PackageGate.GATED, it) }
    }

    fun testTheMenuNamesThePluginEvenBeforeTheCatalogLoads() {
        assertEq("Face Changer", PackageMenu.name("face-changer", "Face Changer"))
        assertEq("Face changer", PackageMenu.name("face-changer"))
        assertEq("Notes", PackageMenu.name("notes", "  "))
        assertEq("this plugin", PackageMenu.name(""))
    }

    fun testTheMenuLabelsSayWhatEachItemDoes() {
        assertEq("Rate Notes", PackageMenu.rateLabel("Notes", rated = false))
        assertEq("Edit your rating of Notes", PackageMenu.rateLabel("Notes", rated = true))
        assertEq("Uninstall", PackageMenu.uninstallLabel(null))
        assertEq("Uninstall from acme", PackageMenu.uninstallLabel("acme"))
        assertEq("Send feedback or report a bug", PackageMenu.FEEDBACK)
        assertEq("Rate, report or uninstall…", PackageMenu.OPTIONS)
    }

    fun testThePopupSendsARatingAReportOrBoth() {
        expect(!PluginFeedback().canSend, "empty")
        expect(!PluginFeedback(text = "   ").canSend, "blank")
        expect(!PluginFeedback(text = "  ").canAskAgent, "blank agent")
        val rated = PluginFeedback(stars = 5)
        expect(rated.canSend && rated.rates && !rated.reports && !rated.canAskAgent, "rated")
        val written = PluginFeedback(text = "Crashes on open")
        expect(written.canSend && !written.rates && written.canAskAgent, "written")
    }

    fun testFiveStarsKeepTheEarlierReviewAndFewerStarsUseTheTypedText() {
        val rules = ReviewRules()
        val earlier = ReviewDraft(stars = 3, problem = "old problem", improvement = "old idea", comment = "kept")
        assertEq(null, PluginFeedback(text = "words").review(earlier, rules))
        val five = PluginFeedback(stars = 5, text = "Love it").review(earlier, rules)
        assertEq(5, five?.stars)
        assertEq("old problem", five?.problem)
        expect(!PluginFeedback(stars = 5).asksImprovement(rules), "five stars ask nothing more")
        val two = PluginFeedback(stars = 2, text = "  The sync stops  ", improvement = " Retry it ").review(earlier, rules)
        assertEq(2, two?.stars)
        assertEq("The sync stops", two?.problem)
        assertEq("Retry it", two?.improvement)
        assertEq("kept", two?.comment)
        expect(PluginFeedback(stars = 2).asksImprovement(rules), "two stars ask how to improve")
        assertEq("old problem", PluginFeedback(stars = 2).review(earlier, rules)?.problem)
    }

    fun testThanksSayWhatWasSent() {
        assertEq("Thanks for rating CRM.", PluginFeedback.thanks("CRM", rated = true, reported = false))
        assertEq("Thanks — your feedback on CRM reached the ForkMesh team.", PluginFeedback.thanks("CRM", rated = false, reported = true))
        assertEq("Thanks — your rating and feedback on CRM reached the ForkMesh team.", PluginFeedback.thanks("CRM", rated = true, reported = true))
    }

    fun testTheAgentPromptNamesThePluginAndCarriesTheText() {
        val prompt = PluginFeedback(stars = 2, text = " Export is empty ").agentPrompt("Ledger", "ledger", "Ledger")
        expect(prompt.startsWith("Feedback on the ForkMesh plugin Ledger (package `ledger`), sent from the Ledger screen of the Android app:"), prompt)
        expect(prompt.contains("\n\nExport is empty\n\nRated 2 of 5 stars.\n\n"), prompt)
        expect(!PluginFeedback(text = "x").agentPrompt("A", "a", "A").contains("Rated"), "no rating line")
    }

    fun testEveryPluginHasAPublicShareLink() {
        assertEq("https://forkmesh.com/market/notes", PackageMenu.shareUrl("notes"))
        assertEq("https://forkmesh.com/market/face-changer", PackageMenu.shareUrl("Face-Changer"))
        assertEq("https://forkmesh.com/market/acme.crm-2", PackageMenu.shareUrl(" acme.crm-2 "))
        assertEq("https://forkmesh.com/market/" + "a".repeat(96), PackageMenu.shareUrl("a".repeat(96)))
        for (bad in listOf("", "9lives", "-notes", "notes-", "no--tes", "no_tes", "a/b", "previews", "a".repeat(97))) {
            assertEq(null, PackageMenu.shareUrl(bad), bad)
        }
        assertEq("Share…", PackageMenu.SHARE)
        assertEq("Copy link", PackageMenu.COPY_LINK)
    }
}
