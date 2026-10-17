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
