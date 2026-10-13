package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class RepoCatalogTest {
    private val catalog = listOf(
        Repository(
            owner = "beta",
            name = "relay",
            description = "Mainnode catalog relay",
            language = "Python",
            mirrors = 1,
            isPrivate = true,
            updatedMs = 1000,
        ),
        Repository(
            owner = "alpha",
            name = "mobile",
            description = "Phone command center",
            language = "Dart",
            mirrors = 8,
            cloneOnline = true,
            updatedMs = 2000,
        ),
        Repository(
            owner = "gamma",
            name = "desktop",
            description = "Qt canonical host",
            language = "C++",
            mirrors = 3,
            liveHost = true,
            updatedMs = 3000,
        ),
    )

    private fun names(query: String = "", filter: RepoFilter = RepoFilter.All, sort: RepoSort = RepoSort.RecentlyUpdated) =
        RepoCatalog.filterAndSort(catalog, RepoCatalog.normalizeQuery(query), filter, sort).map { it.fullName }

    fun testSearchMatchesDescriptionAndLanguage() {
        assertEq(listOf("beta/relay"), names(query = "mainnode"))
        assertEq(listOf("alpha/mobile"), names(query = "dart"))
        assertEq(listOf("alpha/mobile"), names(query = "  DART "))
        assertEq(listOf("gamma/desktop"), names(query = "gamma/"))
    }

    fun testDefaultOrderIsRecentlyUpdatedFirst() {
        assertEq(listOf("gamma/desktop", "alpha/mobile", "beta/relay"), names())
    }

    fun testFiltersRefineTheCatalog() {
        assertEq(listOf("beta/relay"), names(filter = RepoFilter.Private))
        assertEq(listOf("gamma/desktop", "alpha/mobile"), names(filter = RepoFilter.Online))
        assertEq(listOf("beta/relay"), names(filter = RepoFilter.Offline))
        assertEq(listOf("gamma/desktop", "alpha/mobile"), names(filter = RepoFilter.Public))
    }

    fun testSortByMirrorsAndName() {
        assertEq(listOf("alpha/mobile", "gamma/desktop"), names(filter = RepoFilter.Online, sort = RepoSort.Mirrors))
        assertEq(listOf("alpha/mobile", "beta/relay", "gamma/desktop"), names(sort = RepoSort.Name))
    }

    fun testTiesFallBackToTheName() {
        val tied = listOf(Repository(owner = "b", name = "x", mirrors = 2), Repository(owner = "A", name = "x", mirrors = 2))
        assertEq(listOf("A/x", "b/x"), RepoCatalog.filterAndSort(tied, "", RepoFilter.All, RepoSort.Mirrors).map { it.fullName })
        assertEq(listOf("A/x", "b/x"), RepoCatalog.filterAndSort(tied, "", RepoFilter.All, RepoSort.RecentlyUpdated).map { it.fullName })
    }

    fun testLabels() {
        assertEq(listOf("All", "Online", "Offline", "Public", "Private"), RepoFilter.entries.map { it.label })
        assertEq(listOf("Updated", "Name", "Mirrors"), RepoSort.entries.map { it.buttonLabel })
        assertEq(listOf("Recently updated first", "Name A-Z", "Mirrors high-low"), RepoSort.entries.map { it.menuLabel })
    }

    fun testEmptyStateCopyFollowsTheControls() {
        expect(!RepoCatalog.hasActiveControls("", RepoFilter.All))
        expect(RepoCatalog.hasActiveControls("x", RepoFilter.All))
        expect(RepoCatalog.hasActiveControls("", RepoFilter.Offline))
        assertEq("No matching repositories", RepoCatalog.emptyTitle(true))
        assertEq("Try another search term or filter.", RepoCatalog.emptyMessage(true))
        assertEq("No repositories", RepoCatalog.emptyTitle(false))
        assertEq("Repository mirrors will appear here when available.", RepoCatalog.emptyMessage(false))
        assertEq("list-online-mirrors-dart-1", RepoCatalog.listStateKey(RepoFilter.Online, RepoSort.Mirrors, "dart", 1))
    }

    fun testMetaLineHidesZeroCountsAndNamesMirrors() {
        val now = 10L * 86_400_000
        val mobile = catalog[1].copy(updatedMs = now - 3 * 3_600_000)
        val items = RepoCatalog.metaItems(mobile, now)
        assertEq(listOf("Dart", "8 mirrors", "Public", "Updated 3h ago"), items.map { it.text })
        expect(items[0].isLanguage)
        assertEq("Updated 1970-01-10", items.last().tooltip)

        val starred = Repository(owner = "o", name = "r", stars = 2, forks = 1, mirrors = 1, isPrivate = true)
        assertEq(listOf("2", "1", "1 mirror", "Private"), RepoCatalog.metaItems(starred, now).map { it.text })
        assertEq(listOf("star_border", "call_split", "dns_outlined", "lock_outline"), RepoCatalog.metaItems(starred, now).map { it.icon })
    }

    fun testPresence() {
        expect(!RepoCatalog.isOnline(catalog[0]))
        expect(RepoCatalog.isOnline(catalog[1]))
        expect(RepoCatalog.isOnline(catalog[2]))
    }
}
