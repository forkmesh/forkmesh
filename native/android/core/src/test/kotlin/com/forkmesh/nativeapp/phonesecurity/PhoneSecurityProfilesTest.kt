package com.forkmesh.nativeapp.phonesecurity

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class PhoneSecurityProfilesTest {
    private val today = PhoneSecurityFixtures.today

    private fun scan(facts: PhoneSecurityFacts) = PhoneSecurityScanner.scan(facts, today)

    private fun ids(facts: PhoneSecurityFacts, status: PhoneSecurityStatus) =
        scan(facts).filter { it.status == status }.map { it.check.id }.toSet()

    fun testHardenedPhoneHasNoCriticalOrWarning() {
        val facts = PhoneSecurityFixtures.hardened()
        assertEq(emptySet<String>(), ids(facts, PhoneSecurityStatus.Critical))
        assertEq(emptySet<String>(), ids(facts, PhoneSecurityStatus.Warning))
    }

    fun testHardenedPhoneHasNothingUnavailable() {
        assertEq(emptySet<String>(), ids(PhoneSecurityFixtures.hardened(), PhoneSecurityStatus.Unavailable))
    }

    fun testHardenedPhonePassesEverything() {
        val findings = scan(PhoneSecurityFixtures.hardened())
        assertEq(findings.size, findings.count { it.status == PhoneSecurityStatus.Pass })
        assertEq("42 checks: 42 pass, 0 need review, 0 warnings, 0 critical, 0 unavailable", PhoneSecurityScanner.stateSummary(findings))
    }

    fun testHardenedPhoneOffersNoFix() {
        assertEq(0, PhoneSecurityScanner.counts(scan(PhoneSecurityFixtures.hardened()))[PhoneSecurityFilter.Fixable])
    }

    fun testRootedPhoneRaisesTheExpectedCriticals() {
        assertEq(
            setOf(
                "android.su-binary", "android.root-apps", "android.magisk-files", "android.hooking-frameworks",
                "android.injected-libraries", "android.frida-server", "android.selinux",
            ),
            ids(PhoneSecurityFixtures.rooted(), PhoneSecurityStatus.Critical),
        )
    }

    fun testRootedPhoneRaisesTheExpectedWarnings() {
        assertEq(
            setOf("android.busybox", "android.test-keys", "android.build-type", "android.system-rw"),
            ids(PhoneSecurityFixtures.rooted(), PhoneSecurityStatus.Warning),
        )
    }

    fun testRootedPhoneListsCriticalsFirst() {
        val findings = scan(PhoneSecurityFixtures.rooted())
        expect(findings.take(7).all { it.status == PhoneSecurityStatus.Critical }, "criticals lead")
        assertEq(PhoneSecurityStatus.Warning, findings[7].status)
    }

    fun testRootedPhoneUsageNamesTheEvidence() {
        val findings = scan(PhoneSecurityFixtures.rooted()).associateBy { it.check.id }
        expect(findings.getValue("android.su-binary").usage!!.contains("/system/xbin/su"), "su path")
        expect(findings.getValue("android.root-apps").usage!!.contains("com.topjohnwu.magisk"), "magisk package")
        expect(findings.getValue("android.injected-libraries").usage!!.contains("frida-agent-64.so"), "frida agent")
        expect(findings.getValue("android.hooking-frameworks").detail.contains("LSPosed"), "lsposed")
    }

    fun testRootedSummary() {
        assertEq(
            "42 checks: 31 pass, 0 need review, 4 warnings, 7 critical, 0 unavailable",
            PhoneSecurityScanner.stateSummary(scan(PhoneSecurityFixtures.rooted())),
        )
    }

    fun testEmulatorFixtureOnlyReviews() {
        val facts = PhoneSecurityFixtures.hardened().copy(
            build = PhoneSecurityFixtures.emulatorBuild.copy(type = "user", tags = "release-keys"),
            relayUrl = "ws://10.0.2.2:8787/api/repo/mainnode/forkmesh/rooms/world-general/ws",
            appDebuggable = true,
            debuggerAttached = true,
            installer = "",
        )
        assertEq(emptySet<String>(), ids(facts, PhoneSecurityStatus.Critical))
        assertEq(emptySet<String>(), ids(facts, PhoneSecurityStatus.Warning))
        assertEq(
            setOf("android.emulator", "android.relay-https", "android.app-debuggable", "android.debugger", "android.install-source"),
            ids(facts, PhoneSecurityStatus.Review),
        )
    }

    fun testUnreadablePhoneIsAllUnavailable() {
        val findings = scan(PhoneSecurityFixtures.empty())
        assertEq(42, PhoneSecurityScanner.counts(findings)[PhoneSecurityFilter.Unavailable])
        assertEq(0, PhoneSecurityScanner.counts(findings)[PhoneSecurityFilter.Fixable])
        assertEq(0, PhoneSecurityScanner.counts(findings)[PhoneSecurityFilter.Attention])
    }
}
