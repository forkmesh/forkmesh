package com.forkmesh.nativeapp.phonesecurity

import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityScanner as S
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class PhoneSecurityScannerTest {
    private val today = PhoneSecurityFixtures.today

    private fun check(id: String, title: String, category: String = PhoneSecurityCategory.SYSTEM) =
        PhoneSecurityCheck(id, title, category, "Explains $title.")

    private fun finding(title: String, status: PhoneSecurityStatus, detail: String = "", usage: String? = null, fix: PhoneSecurityFix? = null, category: String = PhoneSecurityCategory.SYSTEM) =
        PhoneSecurityFinding(check("android.$title".lowercase().replace(' ', '-'), title, category), status, detail, usage, fix)

    private val sampleFix = PhoneSecurityFix("Open", "Open settings.", PhoneSecuritySettings.SECURITY)

    private val sample = listOf(
        finding("Alpha", PhoneSecurityStatus.Pass),
        finding("Bravo", PhoneSecurityStatus.Review, "needs a look", fix = sampleFix, category = PhoneSecurityCategory.NETWORK),
        finding("Charlie", PhoneSecurityStatus.Warning, "risky", fix = sampleFix),
        finding("Delta", PhoneSecurityStatus.Critical, "bad", usage = "Used by QR sign-in", category = PhoneSecurityCategory.PRIVACY),
        finding("Echo", PhoneSecurityStatus.Unavailable, "unknown"),
        finding("Foxtrot", PhoneSecurityStatus.Pass, category = PhoneSecurityCategory.NETWORK),
    )

    fun testSortOrderIsCriticalWarningReviewUnavailablePass() {
        val shuffled = sample.reversed()
        assertEq(listOf("Delta", "Charlie", "Bravo", "Echo", "Alpha", "Foxtrot"), S.sorted(shuffled).map { it.check.title })
    }

    fun testSortBreaksTiesByTitle() {
        val list = listOf(
            finding("zulu", PhoneSecurityStatus.Review),
            finding("Mike", PhoneSecurityStatus.Review),
            finding("alpha", PhoneSecurityStatus.Review),
        )
        assertEq(listOf("alpha", "Mike", "zulu"), S.sorted(list).map { it.check.title })
    }

    fun testScanIsSortedBySeverity() {
        val findings = S.scan(PhoneSecurityFixtures.rooted(), today)
        val ranks = findings.map { it.status.rank }
        assertEq(ranks.sorted(), ranks)
        for (index in 1 until findings.size) {
            val a = findings[index - 1]
            val b = findings[index]
            if (a.status == b.status) expect(a.check.title.lowercase() <= b.check.title.lowercase(), "${a.check.title} before ${b.check.title}")
        }
    }

    fun testScanIsDeterministic() {
        assertEq(S.scan(PhoneSecurityFixtures.rooted(), today), S.scan(PhoneSecurityFixtures.rooted(), today))
    }

    fun testFilterAll() {
        assertEq(6, sample.count { S.matchesFilter(it, PhoneSecurityFilter.All) })
    }

    fun testFilterAttentionIsCriticalWarningReview() {
        assertEq(listOf("Bravo", "Charlie", "Delta"), sample.filter { S.matchesFilter(it, PhoneSecurityFilter.Attention) }.map { it.check.title })
    }

    fun testFilterPerStatus() {
        assertEq(listOf("Delta"), sample.filter { S.matchesFilter(it, PhoneSecurityFilter.Critical) }.map { it.check.title })
        assertEq(listOf("Charlie"), sample.filter { S.matchesFilter(it, PhoneSecurityFilter.Warning) }.map { it.check.title })
        assertEq(listOf("Bravo"), sample.filter { S.matchesFilter(it, PhoneSecurityFilter.Review) }.map { it.check.title })
        assertEq(listOf("Alpha", "Foxtrot"), sample.filter { S.matchesFilter(it, PhoneSecurityFilter.Pass) }.map { it.check.title })
        assertEq(listOf("Echo"), sample.filter { S.matchesFilter(it, PhoneSecurityFilter.Unavailable) }.map { it.check.title })
    }

    fun testFilterCanFixMeansHasAFix() {
        assertEq(listOf("Bravo", "Charlie"), sample.filter { S.matchesFilter(it, PhoneSecurityFilter.Fixable) }.map { it.check.title })
    }

    fun testCounts() {
        val counts = S.counts(sample)
        assertEq(6, counts[PhoneSecurityFilter.All])
        assertEq(3, counts[PhoneSecurityFilter.Attention])
        assertEq(1, counts[PhoneSecurityFilter.Critical])
        assertEq(1, counts[PhoneSecurityFilter.Warning])
        assertEq(1, counts[PhoneSecurityFilter.Review])
        assertEq(2, counts[PhoneSecurityFilter.Fixable])
        assertEq(2, counts[PhoneSecurityFilter.Pass])
        assertEq(1, counts[PhoneSecurityFilter.Unavailable])
        assertEq(PhoneSecurityFilter.entries.toSet(), counts.keys)
    }

    fun testCountsOfARealScanAddUp() {
        val findings = S.scan(PhoneSecurityFixtures.rooted(), today)
        val counts = S.counts(findings)
        val byStatus = listOf(PhoneSecurityFilter.Critical, PhoneSecurityFilter.Warning, PhoneSecurityFilter.Review, PhoneSecurityFilter.Pass, PhoneSecurityFilter.Unavailable)
        assertEq(findings.size, byStatus.sumOf { counts.getValue(it) })
        assertEq(counts.getValue(PhoneSecurityFilter.Critical) + counts.getValue(PhoneSecurityFilter.Warning) + counts.getValue(PhoneSecurityFilter.Review), counts[PhoneSecurityFilter.Attention])
    }

    fun testCategoryFilter() {
        assertEq(listOf("Bravo", "Foxtrot"), sample.filter { S.matchesCategory(it, PhoneSecurityCategory.NETWORK) }.map { it.check.title })
        assertEq(6, sample.count { S.matchesCategory(it, null) })
        assertEq(6, sample.count { S.matchesCategory(it, "") })
    }

    fun testQueryMatchesTitle() {
        expect(S.matchesQuery(sample[0], "alp"), "title")
    }

    fun testQueryMatchesCategory() {
        expect(S.matchesQuery(sample[1], "network"), "category")
    }

    fun testQueryMatchesDetail() {
        expect(S.matchesQuery(sample[2], "RISK"), "detail")
    }

    fun testQueryMatchesUsage() {
        expect(S.matchesQuery(sample[3], "qr sign-in"), "usage")
    }

    fun testQueryDoesNotMatchExplanationOrId() {
        expect(!S.matchesQuery(sample[0], "Explains"), "explanation is not searched")
        expect(!S.matchesQuery(sample[0], "android."), "id is not searched")
    }

    fun testBlankQueryMatchesEverything() {
        for (item in sample) {
            expect(S.matchesQuery(item, ""), "empty")
            expect(S.matchesQuery(item, "   "), "blank")
        }
    }

    fun testQueryIsTrimmed() {
        expect(S.matchesQuery(sample[0], "  alpha  "), "trimmed")
    }

    fun testVisibleCombinesFilterCategoryAndQuery() {
        assertEq(listOf("Bravo"), S.visible(sample, PhoneSecurityFilter.Attention, PhoneSecurityCategory.NETWORK, "").map { it.check.title })
        assertEq(listOf("Foxtrot"), S.visible(sample, PhoneSecurityFilter.All, PhoneSecurityCategory.NETWORK, "fox").map { it.check.title })
        assertEq(emptyList<String>(), S.visible(sample, PhoneSecurityFilter.Critical, PhoneSecurityCategory.NETWORK, "").map { it.check.title })
    }

    fun testStateSummaryWording() {
        assertEq("6 checks: 2 pass, 1 need review, 1 warnings, 1 critical, 1 unavailable", S.stateSummary(sample))
        assertEq("0 checks: 0 pass, 0 need review, 0 warnings, 0 critical, 0 unavailable", S.stateSummary(emptyList()))
    }

    fun testFilteredSummaryWithoutScopeIsTheStateSummary() {
        assertEq(S.stateSummary(sample), S.filteredSummary(sample, 6, ""))
        assertEq(S.stateSummary(sample), S.filteredSummary(sample, 6, "  ", PhoneSecurityFilter.All, null))
    }

    fun testFilteredSummaryForAQuery() {
        assertEq(
            "Showing 1 of 6 checks matching \"alpha\". 6 checks: 2 pass, 1 need review, 1 warnings, 1 critical, 1 unavailable",
            S.filteredSummary(sample, 1, " alpha "),
        )
    }

    fun testFilteredSummaryForAFilter() {
        assertEq(
            "Showing 3 of 6 checks in Needs attention. 6 checks: 2 pass, 1 need review, 1 warnings, 1 critical, 1 unavailable",
            S.filteredSummary(sample, 3, "", PhoneSecurityFilter.Attention),
        )
    }

    fun testFilteredSummaryForAFilterCategoryAndQuery() {
        assertEq(
            "Showing 1 of 6 checks in For review · Network matching \"bra\". 6 checks: 2 pass, 1 need review, 1 warnings, 1 critical, 1 unavailable",
            S.filteredSummary(sample, 1, "bra", PhoneSecurityFilter.Review, PhoneSecurityCategory.NETWORK),
        )
    }

    fun testFilteredSummaryForACategoryOnly() {
        assertEq(
            "Showing 2 of 6 checks in Network. 6 checks: 2 pass, 1 need review, 1 warnings, 1 critical, 1 unavailable",
            S.filteredSummary(sample, 2, "", PhoneSecurityFilter.All, PhoneSecurityCategory.NETWORK),
        )
    }

    fun testEmptyResultWording() {
        assertEq("No checks match \"zzz\". Try a different search.", S.filteredSummary(sample, 0, "zzz"))
        assertEq("No checks in Critical · Network. Try a different filter.", S.filteredSummary(sample, 0, "", PhoneSecurityFilter.Critical, PhoneSecurityCategory.NETWORK))
        assertEq(
            "No checks in Warnings match \"zzz\". Try a different filter or search.",
            S.filteredSummary(sample, 0, "zzz", PhoneSecurityFilter.Warning),
        )
    }

    fun testPassingAndUnavailableFindingsNeverOfferAFix() {
        for (facts in listOf(PhoneSecurityFixtures.hardened(), PhoneSecurityFixtures.rooted(), PhoneSecurityFixtures.empty())) {
            for (item in S.scan(facts, today)) {
                if (item.status == PhoneSecurityStatus.Pass || item.status == PhoneSecurityStatus.Unavailable) {
                    assertEq(null, item.fix, item.check.id)
                }
            }
        }
    }

    private fun worstCase() = PhoneSecurityFixtures.rooted().copy(
        deviceSecure = false,
        biometrics = PhoneBiometrics.NoneEnrolled,
        screenTimeoutMillis = 3_600_000,
        lockScreenPrivateContent = true,
        stayAwakeWhileCharging = true,
        build = PhoneSecurityFixtures.emulatorBuild.copy(tags = "test-keys"),
        debuggerAttached = true,
        appDebuggable = true,
        installer = "",
        cleartextPermitted = true,
        relayUrl = "ws://relay.example.org/ws",
        strongBox = false,
        hardwareKeystore = false,
        securityPatch = "2020-01-01",
        sdkInt = 29,
        encryptionStatus = PhoneSecurityParsers.ENCRYPTION_INACTIVE,
        developerOptions = true,
        usbDebugging = true,
        wirelessDebugging = true,
        autoTime = false,
        httpProxy = "10.0.0.1:8080",
        vpnActive = true,
        userCertificates = listOf("Proxy CA"),
        privateDnsMode = "off",
        accessibilityServices = listOf("com.example.reader"),
        notificationListeners = listOf("com.example.watch"),
        deviceAdmins = listOf("com.example.admin"),
        keyboard = "com.example.keys/.Ime",
        keyboardIsSystem = false,
        cameraGranted = true,
        microphoneGranted = true,
        nearbyGranted = true,
        notificationsGranted = false,
    )

    fun testEveryFixHasALabelDescriptionAndSettingsAction() {
        val fixes = S.scan(worstCase(), today).mapNotNull { it.fix }
        expect(fixes.size >= 20, "the worst case offers many fixes, got ${fixes.size}")
        for (fix in fixes) {
            expect(fix.label.isNotBlank(), "label")
            expect(fix.description.isNotBlank() && fix.description.endsWith("."), fix.description)
            expect(fix.action.isNotBlank(), fix.label)
            expect(fix.action in PhoneSecuritySettings.all, fix.action)
            if (fix.ownAppDetails) assertEq(PhoneSecuritySettings.APP_DETAILS, fix.action)
        }
    }

    fun testEverySpecSettingsPageIsUsedByAFix() {
        val actions = S.scan(worstCase(), today).mapNotNull { it.fix?.action }.toSet()
        assertEq(PhoneSecuritySettings.all.toSet(), actions)
    }

    fun testSignsOfRootHaveNoFix() {
        val rootIds = setOf(
            "android.su-binary", "android.root-apps", "android.magisk-files", "android.busybox", "android.test-keys",
            "android.build-type", "android.system-rw", "android.hooking-frameworks", "android.injected-libraries",
            "android.frida-server", "android.selinux",
        )
        for (item in S.scan(worstCase(), today).filter { it.check.id in rootIds }) {
            expect(item.status != PhoneSecurityStatus.Pass, item.check.id)
            assertEq(null, item.fix, item.check.id)
        }
    }

    fun testWorstCaseHasNoPassesExceptVpn() {
        val passes = S.scan(worstCase(), today).filter { it.status == PhoneSecurityStatus.Pass }.map { it.check.id }
        assertEq(listOf("android.vpn"), passes)
    }

    fun testEveryDetailEndsInAPeriod() {
        for (facts in listOf(PhoneSecurityFixtures.hardened(), worstCase(), PhoneSecurityFixtures.empty())) {
            for (item in S.scan(facts, today)) expect(item.detail.endsWith("."), "${item.check.id}: ${item.detail}")
        }
    }

    fun testUsageIsNeverBlank() {
        for (facts in listOf(PhoneSecurityFixtures.hardened(), worstCase(), PhoneSecurityFixtures.empty())) {
            for (item in S.scan(facts, today)) expect(item.usage == null || item.usage!!.isNotBlank(), item.check.id)
        }
    }

    fun testFindingsCarryCatalogChecks() {
        for (item in S.scan(worstCase(), today)) assertEq(PhoneSecurityCatalog.byId(item.check.id), item.check)
    }
}
