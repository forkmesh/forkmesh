package com.forkmesh.nativeapp.phonesecurity

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class PhoneSecurityCatalogTest {
    private val expected: List<Triple<String, String, String>> = listOf(
        Triple("android.screen-lock", "Screen lock", "Lock screen"),
        Triple("android.biometrics", "Fingerprint or face unlock", "Lock screen"),
        Triple("android.lock-timeout", "Screen timeout", "Lock screen"),
        Triple("android.lock-notifications", "Notification content on the lock screen", "Lock screen"),
        Triple("android.stay-awake", "Stay awake while charging", "Lock screen"),
        Triple("android.su-binary", "su binary", "Device integrity"),
        Triple("android.root-apps", "Root manager apps", "Device integrity"),
        Triple("android.magisk-files", "Root framework files", "Device integrity"),
        Triple("android.busybox", "BusyBox", "Device integrity"),
        Triple("android.test-keys", "Release signing", "Device integrity"),
        Triple("android.build-type", "Production build", "Device integrity"),
        Triple("android.system-rw", "System partition read-only", "Device integrity"),
        Triple("android.hooking-frameworks", "Xposed or LSPosed", "Device integrity"),
        Triple("android.injected-libraries", "Code injected into ForkMesh", "Device integrity"),
        Triple("android.frida-server", "Instrumentation server", "Device integrity"),
        Triple("android.selinux", "SELinux enforcing", "Device integrity"),
        Triple("android.emulator", "Real device", "Device integrity"),
        Triple("android.debugger", "Debugger attached", "Device integrity"),
        Triple("android.app-debuggable", "Release build of ForkMesh", "App protection"),
        Triple("android.install-source", "Where ForkMesh came from", "App protection"),
        Triple("android.cleartext", "Encrypted connections only", "App protection"),
        Triple("android.relay-https", "Relay address uses HTTPS", "App protection"),
        Triple("android.hardware-keystore", "Secure hardware for keys", "App protection"),
        Triple("android.patch-level", "Security patch level", "System"),
        Triple("android.os-version", "Android version", "System"),
        Triple("android.encryption", "Storage encryption", "System"),
        Triple("android.developer-options", "Developer options", "System"),
        Triple("android.usb-debugging", "USB debugging", "System"),
        Triple("android.wireless-debugging", "Wireless debugging", "System"),
        Triple("android.auto-time", "Automatic date and time", "System"),
        Triple("android.http-proxy", "HTTP proxy", "Network"),
        Triple("android.vpn", "VPN", "Network"),
        Triple("android.user-certificates", "Added certificate authorities", "Network"),
        Triple("android.private-dns", "Private DNS", "Network"),
        Triple("android.accessibility-services", "Accessibility services", "Apps with access"),
        Triple("android.notification-listeners", "Apps that read notifications", "Apps with access"),
        Triple("android.device-admins", "Device admin apps", "Apps with access"),
        Triple("android.keyboard", "Keyboard", "Apps with access"),
        Triple("android.permission.camera", "Camera access", "Privacy"),
        Triple("android.permission.microphone", "Microphone access", "Privacy"),
        Triple("android.permission.nearby", "Nearby devices", "Privacy"),
        Triple("android.permission.notifications", "Notifications", "Privacy"),
    )

    fun testIdSetEqualsTheSpecTable() {
        assertEq(expected.map { it.first }.toSet(), PhoneSecurityCatalog.checks.map { it.id }.toSet())
        assertEq(42, PhoneSecurityCatalog.checks.size)
    }

    fun testCatalogOrderFollowsTheSpecTable() {
        assertEq(expected.map { it.first }, PhoneSecurityCatalog.checks.map { it.id })
    }

    fun testTitlesMatchTheSpec() {
        for ((id, title, _) in expected) assertEq(title, PhoneSecurityCatalog.byId(id)?.title, id)
    }

    fun testCategoriesMatchTheSpec() {
        for ((id, _, category) in expected) assertEq(category, PhoneSecurityCatalog.byId(id)?.category, id)
    }

    fun testIdsAreUnique() {
        val ids = PhoneSecurityCatalog.checks.map { it.id }
        assertEq(ids.size, ids.toSet().size)
    }

    fun testTitlesAreUnique() {
        val titles = PhoneSecurityCatalog.checks.map { it.title }
        assertEq(titles.size, titles.toSet().size)
    }

    fun testEveryIdIsPrefixedAndroid() {
        for (check in PhoneSecurityCatalog.checks) expect(check.id.startsWith("android."), check.id)
    }

    fun testEveryIdIsLowercaseKebab() {
        val shape = Regex("^android\\.[a-z]+(-[a-z]+)*(\\.[a-z]+(-[a-z]+)*)?$")
        for (check in PhoneSecurityCatalog.checks) expect(shape.matches(check.id), check.id)
    }

    fun testEveryCategoryIsKnown() {
        for (check in PhoneSecurityCatalog.checks) expect(check.category in PhoneSecurityCategory.all, check.id)
    }

    fun testEveryCategoryHasChecks() {
        for (category in PhoneSecurityCategory.all) {
            expect(PhoneSecurityCatalog.checks.any { it.category == category }, category)
        }
    }

    fun testCategoryNamesAndOrder() {
        assertEq(
            listOf("Lock screen", "Device integrity", "System", "App protection", "Network", "Apps with access", "Privacy"),
            PhoneSecurityCategory.all,
        )
    }

    fun testEveryCheckHasAnExplanationEndingInAPeriod() {
        for (check in PhoneSecurityCatalog.checks) {
            expect(check.explanation.isNotBlank(), "${check.id} has no explanation")
            expect(check.explanation.endsWith("."), "${check.id} explanation ends without a period")
        }
    }

    fun testExplanationsAreSentencesNotFragments() {
        for (check in PhoneSecurityCatalog.checks) {
            expect(check.explanation.length >= 60, "${check.id} explanation is too short")
            expect(check.explanation.first().isUpperCase(), "${check.id} explanation starts lowercase")
        }
    }

    fun testExplanationsCoverExactlyTheCatalog() {
        assertEq(PhoneSecurityCatalog.checks.map { it.id }.toSet(), PhoneSecurityExplanations.ids)
    }

    fun testUnknownIdHasNoExplanation() {
        assertEq("", PhoneSecurityExplanations.text("android.nope"))
        assertEq(null, PhoneSecurityCatalog.byId("android.nope"))
    }

    fun testScanReturnsOneFindingPerCheck() {
        val findings = PhoneSecurityScanner.scan(PhoneSecurityFixtures.hardened(), PhoneSecurityFixtures.today)
        assertEq(PhoneSecurityCatalog.checks.map { it.id }.sorted(), findings.map { it.check.id }.sorted())
    }

    fun testStatusRanksAndNames() {
        assertEq(listOf(0, 1, 2, 3, 4), listOf(
            PhoneSecurityStatus.Critical, PhoneSecurityStatus.Warning, PhoneSecurityStatus.Review,
            PhoneSecurityStatus.Unavailable, PhoneSecurityStatus.Pass,
        ).map { it.rank })
        assertEq(listOf("Pass", "Review", "Warning", "Critical", "Unavailable"), PhoneSecurityStatus.entries.map { it.displayName })
    }

    fun testFilterNamesMatchTheDesktop() {
        assertEq(
            listOf("All", "Needs attention", "Critical", "Warnings", "For review", "Can fix", "Passed", "Unavailable"),
            PhoneSecurityScanner.filters.map { it.displayName },
        )
    }

    fun testSettingsActionsAreAndroidSettingsIntents() {
        for (action in PhoneSecuritySettings.all + PhoneSecuritySettings.MAIN) {
            expect(action.startsWith("android.settings."), action)
        }
        assertEq(13, PhoneSecuritySettings.all.size)
        assertEq(PhoneSecuritySettings.all.size, PhoneSecuritySettings.all.toSet().size)
    }

    fun testPathListsAreAbsoluteAndDistinct() {
        val lists = listOf(PhoneSecurityPaths.su, PhoneSecurityPaths.magisk, PhoneSecurityPaths.busybox, PhoneSecurityPaths.frida, PhoneSecurityPaths.xposed)
        for (list in lists) {
            expect(list.isNotEmpty(), "a path list is empty")
            assertEq(list.size, list.toSet().size)
            for (path in list) expect(path.startsWith("/") && !path.endsWith("/"), path)
        }
        assertEq(lists.flatten().toSet().size, PhoneSecurityPaths.all.size)
    }

    fun testSuPathsCoverTheCommonRootLocations() {
        for (path in listOf("/system/bin/su", "/system/xbin/su", "/sbin/su", "/su/bin/su", "/data/local/xbin/su")) {
            expect(path in PhoneSecurityPaths.su, path)
        }
        expect(PhoneSecurityPaths.su.all { it.endsWith("/su") }, "every su path ends in su")
        expect(PhoneSecurityPaths.busybox.all { it.endsWith("/busybox") }, "every busybox path ends in busybox")
    }

    fun testPackageListsCoverTheSpecNames() {
        val roots = PhoneSecurityPackages.rootApps.values.toSet()
        for (name in listOf("Magisk", "SuperSU", "Superuser", "KernelSU", "APatch", "KingRoot")) expect(name in roots, name)
        val hooks = PhoneSecurityPackages.hookingApps.values.joinToString(" ")
        for (name in listOf("Xposed", "LSPosed", "EdXposed")) expect(hooks.contains(name), name)
        assertEq((PhoneSecurityPackages.rootApps.keys + PhoneSecurityPackages.hookingApps.keys).size, PhoneSecurityPackages.all.size)
        expect(PhoneSecurityPackages.all.all { Regex("^[a-z][a-z0-9_]*(\\.[a-z0-9_]+)+$").matches(it) }, "package names are valid")
    }

    fun testManifestQueriesEveryProbedPackage() {
        val manifest = listOf("../app/src/main/AndroidManifest.xml", "app/src/main/AndroidManifest.xml").map(::File).firstOrNull { it.isFile }
            ?: throw AssertionError("AndroidManifest.xml not found from ${File(".").absolutePath}")
        val declared = Regex("<package android:name=\"([^\"]+)\"").findAll(manifest.readText()).map { it.groupValues[1] }.toSet()
        assertEq(emptyList<String>(), PhoneSecurityPackages.all.filter { it !in declared })
    }
}
