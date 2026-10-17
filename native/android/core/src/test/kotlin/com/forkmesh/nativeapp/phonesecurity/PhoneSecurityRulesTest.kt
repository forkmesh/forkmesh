package com.forkmesh.nativeapp.phonesecurity

import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityStatus.Critical
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityStatus.Pass
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityStatus.Review
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityStatus.Unavailable
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityStatus.Warning
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.LocalDate

class PhoneSecurityRulesTest {
    private val base = PhoneSecurityFixtures.hardened()
    private val none = PhoneSecurityFixtures.empty()

    private fun f(facts: PhoneSecurityFacts, id: String, today: LocalDate = PhoneSecurityFixtures.today) =
        PhoneSecurityFixtures.finding(facts, id, today)

    private fun status(facts: PhoneSecurityFacts, id: String, today: LocalDate = PhoneSecurityFixtures.today) = f(facts, id, today).status

    fun testScreenLock() {
        assertEq(Pass, status(base, "android.screen-lock"))
        val open = f(base.copy(deviceSecure = false), "android.screen-lock")
        assertEq(Critical, open.status)
        assertEq(PhoneSecuritySettings.SECURITY, open.fix?.action)
        assertEq(Unavailable, status(none, "android.screen-lock"))
    }

    fun testBiometrics() {
        assertEq(Pass, status(base, "android.biometrics"))
        assertEq(Pass, status(base.copy(biometrics = PhoneBiometrics.NoHardware), "android.biometrics"))
        val missing = f(base.copy(biometrics = PhoneBiometrics.NoneEnrolled), "android.biometrics")
        assertEq(Review, missing.status)
        assertEq(PhoneSecuritySettings.BIOMETRIC_ENROLL, missing.fix?.action)
        assertEq(Unavailable, status(none, "android.biometrics"))
    }

    fun testLockTimeoutBoundaries() {
        assertEq(Pass, status(base.copy(screenTimeoutMillis = 300_000), "android.lock-timeout"))
        assertEq(Review, status(base.copy(screenTimeoutMillis = 300_001), "android.lock-timeout"))
        assertEq(Review, status(base.copy(screenTimeoutMillis = 1_800_000), "android.lock-timeout"))
        assertEq(Warning, status(base.copy(screenTimeoutMillis = 1_800_001), "android.lock-timeout"))
        assertEq(Unavailable, status(none, "android.lock-timeout"))
    }

    fun testLockTimeoutDetailAndFix() {
        assertEq("The screen turns off after 30 seconds.", f(base, "android.lock-timeout").detail)
        val never = f(base.copy(screenTimeoutMillis = Int.MAX_VALUE.toLong()), "android.lock-timeout")
        assertEq("The screen never turns off by itself.", never.detail)
        assertEq(PhoneSecuritySettings.DISPLAY, never.fix?.action)
        assertEq(null, f(base, "android.lock-timeout").fix)
    }

    fun testLockNotifications() {
        assertEq(Pass, status(base, "android.lock-notifications"))
        val shown = f(base.copy(lockScreenPrivateContent = true), "android.lock-notifications")
        assertEq(Review, shown.status)
        assertEq(PhoneSecuritySettings.NOTIFICATIONS, shown.fix?.action)
        assertEq(Pass, status(base.copy(lockScreenNotifications = false, lockScreenPrivateContent = true), "android.lock-notifications"))
        assertEq(Pass, status(base.copy(lockScreenNotifications = false, lockScreenPrivateContent = null), "android.lock-notifications"))
        assertEq(Unavailable, status(base.copy(lockScreenNotifications = null, lockScreenPrivateContent = null), "android.lock-notifications"))
        assertEq(Review, status(base.copy(lockScreenNotifications = null, lockScreenPrivateContent = true), "android.lock-notifications"))
    }

    fun testStayAwake() {
        assertEq(Pass, status(base, "android.stay-awake"))
        val on = f(base.copy(stayAwakeWhileCharging = true), "android.stay-awake")
        assertEq(Review, on.status)
        assertEq(PhoneSecuritySettings.DEVELOPMENT, on.fix?.action)
        assertEq(Unavailable, status(none, "android.stay-awake"))
    }

    fun testEverySuPathTriggersTheSuCheck() {
        for (path in PhoneSecurityPaths.su) {
            val finding = f(base.copy(existingPaths = setOf(path)), "android.su-binary")
            assertEq(Critical, finding.status, path)
            expect(finding.detail.contains(path), path)
            expect(finding.usage?.contains(path) == true, path)
        }
    }

    fun testSuCheckPassesAndUnavailable() {
        assertEq(Pass, status(base, "android.su-binary"))
        assertEq(Pass, status(base.copy(existingPaths = setOf("/system/bin/sh", "/data/local/tmp/frida-server")), "android.su-binary"))
        assertEq(Unavailable, status(none, "android.su-binary"))
    }

    fun testEveryRootPackageTriggersTheRootAppsCheck() {
        for ((pkg, name) in PhoneSecurityPackages.rootApps) {
            val finding = f(base.copy(installedPackages = setOf(pkg)), "android.root-apps")
            assertEq(Critical, finding.status, pkg)
            expect(finding.detail.contains(name), pkg)
            expect(finding.usage?.contains(pkg) == true, pkg)
        }
    }

    fun testRootAppsPassAndUnavailable() {
        assertEq(Pass, status(base, "android.root-apps"))
        assertEq(Pass, status(base.copy(installedPackages = setOf("org.lsposed.manager")), "android.root-apps"))
        assertEq(Unavailable, status(none, "android.root-apps"))
    }

    fun testRootAppsHaveNoFix() {
        assertEq(null, f(base.copy(installedPackages = setOf("com.topjohnwu.magisk")), "android.root-apps").fix)
    }

    fun testEveryMagiskPathTriggersTheFrameworkFilesCheck() {
        for (path in PhoneSecurityPaths.magisk) {
            assertEq(Critical, status(base.copy(existingPaths = setOf(path)), "android.magisk-files"), path)
        }
        assertEq(Pass, status(base, "android.magisk-files"))
        assertEq(Unavailable, status(none, "android.magisk-files"))
    }

    fun testEveryBusyboxPathWarns() {
        for (path in PhoneSecurityPaths.busybox) {
            assertEq(Warning, status(base.copy(existingPaths = setOf(path)), "android.busybox"), path)
        }
        assertEq(Pass, status(base, "android.busybox"))
        assertEq(Unavailable, status(none, "android.busybox"))
    }

    fun testEveryFridaPathTriggersTheServerCheck() {
        for (path in PhoneSecurityPaths.frida) {
            assertEq(Critical, status(base.copy(existingPaths = setOf(path)), "android.frida-server"), path)
        }
        assertEq(Pass, status(base, "android.frida-server"))
        assertEq(Unavailable, status(none, "android.frida-server"))
    }

    fun testPathListsDoNotCrossTrigger() {
        val onlySu = base.copy(existingPaths = setOf(PhoneSecurityPaths.su.first()))
        assertEq(Pass, status(onlySu, "android.magisk-files"))
        assertEq(Pass, status(onlySu, "android.busybox"))
        assertEq(Pass, status(onlySu, "android.frida-server"))
        assertEq(Pass, status(onlySu, "android.hooking-frameworks"))
    }

    fun testTestKeys() {
        assertEq(Pass, status(base, "android.test-keys"))
        assertEq(Warning, status(base.copy(build = PhoneSecurityFixtures.pixelBuild.copy(tags = "test-keys")), "android.test-keys"))
        assertEq(Warning, status(base.copy(build = PhoneSecurityFixtures.pixelBuild.copy(tags = "release-keys,test-keys")), "android.test-keys"))
        assertEq(Pass, status(base.copy(build = PhoneSecurityFixtures.pixelBuild.copy(tags = "dev-keys")), "android.test-keys"))
        assertEq(Unavailable, status(base.copy(build = PhoneSecurityFixtures.pixelBuild.copy(tags = "")), "android.test-keys"))
        assertEq(Unavailable, status(none, "android.test-keys"))
    }

    fun testBuildType() {
        assertEq(Pass, status(base, "android.build-type"))
        assertEq(Warning, status(base.copy(build = PhoneSecurityFixtures.pixelBuild.copy(type = "userdebug")), "android.build-type"))
        assertEq(Warning, status(base.copy(build = PhoneSecurityFixtures.pixelBuild.copy(type = "eng")), "android.build-type"))
        assertEq(Warning, status(base.copy(build = PhoneSecurityFixtures.pixelBuild.copy(type = "ENG")), "android.build-type"))
        assertEq(Unavailable, status(base.copy(build = PhoneSecurityFixtures.pixelBuild.copy(type = " ")), "android.build-type"))
        assertEq(Unavailable, status(none, "android.build-type"))
    }

    fun testSystemRw() {
        assertEq(Pass, status(base, "android.system-rw"))
        for (point in listOf("/", "/system", "/vendor")) {
            val finding = f(base.copy(procMounts = "/dev/block/dm-0 $point ext4 rw,seclabel 0 0"), "android.system-rw")
            assertEq(Warning, finding.status, point)
            expect(finding.detail.contains(point), point)
        }
        assertEq(Unavailable, status(base.copy(procMounts = ""), "android.system-rw"))
        assertEq(Unavailable, status(none, "android.system-rw"))
    }

    fun testHookingFrameworksFromPackages() {
        for ((pkg, name) in PhoneSecurityPackages.hookingApps) {
            val finding = f(base.copy(installedPackages = setOf(pkg)), "android.hooking-frameworks")
            assertEq(Critical, finding.status, pkg)
            expect(finding.detail.contains(name), pkg)
        }
    }

    fun testHookingFrameworksFromClasses() {
        val finding = f(base.copy(xposedClassLoadable = true), "android.hooking-frameworks")
        assertEq(Critical, finding.status)
        expect(finding.detail.contains("Xposed classes"), finding.detail)
    }

    fun testHookingFrameworksFromMappedLibraries() {
        val maps = PhoneSecurityFixtures.cleanMaps + "\n7b0-7b1 r-xp 00000000 fe:00 1 /data/adb/modules/zygisk_lsposed/lib64/liblspd.so"
        assertEq(Critical, status(base.copy(procSelfMaps = maps), "android.hooking-frameworks"))
        val frida = PhoneSecurityFixtures.cleanMaps + "\n7b0-7b1 r-xp 00000000 fe:00 1 /data/local/tmp/frida-agent-64.so"
        assertEq(Pass, status(base.copy(procSelfMaps = frida), "android.hooking-frameworks"))
    }

    fun testHookingFrameworksFromFiles() {
        for (path in PhoneSecurityPaths.xposed) {
            assertEq(Critical, status(base.copy(existingPaths = setOf(path)), "android.hooking-frameworks"), path)
        }
    }

    fun testHookingFrameworksPassAndUnavailable() {
        assertEq(Pass, status(base, "android.hooking-frameworks"))
        assertEq(Pass, status(none.copy(xposedClassLoadable = false), "android.hooking-frameworks"))
        assertEq(Unavailable, status(none, "android.hooking-frameworks"))
    }

    fun testInjectedLibrariesNamesThemInTheUsage() {
        val maps = PhoneSecurityFixtures.cleanMaps + "\n7b0-7b1 r-xp 00000000 fe:00 1 /data/app/x/lib/arm64/libfrida-gadget.so\n7b2-7b3 r-xp 00000000 fe:00 1 /data/app/y/libsubstrate.so"
        val finding = f(base.copy(procSelfMaps = maps), "android.injected-libraries")
        assertEq(Critical, finding.status)
        assertEq("Loaded: libfrida-gadget.so, libsubstrate.so", finding.usage)
        assertEq("Frida, Substrate code is loaded into ForkMesh.", finding.detail)
        assertEq(null, finding.fix)
    }

    fun testInjectedLibrariesPassAndUnavailable() {
        assertEq(Pass, status(base, "android.injected-libraries"))
        assertEq(Unavailable, status(base.copy(procSelfMaps = " "), "android.injected-libraries"))
        assertEq(Unavailable, status(none, "android.injected-libraries"))
    }

    fun testSelinux() {
        assertEq(Pass, status(base, "android.selinux"))
        assertEq(Critical, status(base.copy(selinuxEnforce = "0"), "android.selinux"))
        assertEq(Critical, status(base.copy(selinuxEnforce = "0\n"), "android.selinux"))
        assertEq(Unavailable, status(base.copy(selinuxEnforce = "x"), "android.selinux"))
        assertEq(Unavailable, status(none, "android.selinux"))
    }

    fun testEmulator() {
        assertEq(Pass, status(base, "android.emulator"))
        val emulator = f(base.copy(build = PhoneSecurityFixtures.emulatorBuild), "android.emulator")
        assertEq(Review, emulator.status)
        assertEq(null, emulator.fix)
        assertEq("This is a Google Pixel 8 Pro.", f(base, "android.emulator").detail)
        assertEq(Unavailable, status(none, "android.emulator"))
    }

    fun testDebugger() {
        assertEq(Pass, status(base, "android.debugger"))
        assertEq(Warning, status(base.copy(debuggerAttached = true), "android.debugger"))
        assertEq(Warning, status(base.copy(debuggerAttached = true, appDebuggable = null), "android.debugger"))
        assertEq(Review, status(base.copy(debuggerAttached = true, appDebuggable = true), "android.debugger"))
        assertEq(Unavailable, status(none, "android.debugger"))
    }

    fun testAppDebuggable() {
        assertEq(Pass, status(base, "android.app-debuggable"))
        assertEq(Review, status(base.copy(appDebuggable = true), "android.app-debuggable"))
        assertEq(Unavailable, status(none, "android.app-debuggable"))
    }

    fun testInstallSourceStoresPass() {
        for ((pkg, name) in PhoneSecurityPackages.trustedInstallers) {
            val finding = f(base.copy(installer = pkg), "android.install-source")
            assertEq(Pass, finding.status, pkg)
            assertEq("Installed from $name.", finding.detail)
        }
    }

    fun testInstallSourceOthersReview() {
        val sideloaded = f(base.copy(installer = ""), "android.install-source")
        assertEq(Review, sideloaded.status)
        expect(sideloaded.detail.contains("sideloaded"), sideloaded.detail)
        expect(sideloaded.fix?.ownAppDetails == true, "the fix opens ForkMesh's app info")
        assertEq(Review, status(base.copy(installer = "com.google.android.packageinstaller"), "android.install-source"))
        val other = f(base.copy(installer = "com.example.store"), "android.install-source")
        assertEq(Review, other.status)
        expect(other.detail.contains("com.example.store"), other.detail)
        assertEq(Unavailable, status(none, "android.install-source"))
    }

    fun testCleartext() {
        assertEq(Pass, status(base, "android.cleartext"))
        assertEq(Warning, status(base.copy(cleartextPermitted = true), "android.cleartext"))
        assertEq(Unavailable, status(none, "android.cleartext"))
    }

    fun testRelayHttps() {
        assertEq(Pass, status(base, "android.relay-https"))
        assertEq(Pass, status(base.copy(relayUrl = "https://relay.example.org"), "android.relay-https"))
        assertEq(Review, status(base.copy(relayUrl = "ws://10.0.2.2:8787/ws"), "android.relay-https"))
        assertEq(Review, status(base.copy(relayUrl = "http://localhost:8787"), "android.relay-https"))
        assertEq(Review, status(base.copy(relayUrl = "http://desk.local:8787"), "android.relay-https"))
        val plain = f(base.copy(relayUrl = "ws://relay.example.org/ws"), "android.relay-https")
        assertEq(Critical, plain.status)
        expect(plain.detail.contains("relay.example.org"), plain.detail)
        assertEq(Unavailable, status(base.copy(relayUrl = ""), "android.relay-https"))
        assertEq(Unavailable, status(base.copy(relayUrl = "gopher://x"), "android.relay-https"))
        assertEq(Unavailable, status(none, "android.relay-https"))
    }

    fun testHardwareKeystore() {
        assertEq(Pass, status(base, "android.hardware-keystore"))
        assertEq(Pass, status(base.copy(strongBox = false, hardwareKeystore = true), "android.hardware-keystore"))
        assertEq(Pass, status(base.copy(strongBox = true, hardwareKeystore = null), "android.hardware-keystore"))
        assertEq(Review, status(base.copy(strongBox = false, hardwareKeystore = false), "android.hardware-keystore"))
        assertEq(Review, status(base.copy(strongBox = null, hardwareKeystore = false), "android.hardware-keystore"))
        assertEq(Unavailable, status(base.copy(strongBox = false, hardwareKeystore = null), "android.hardware-keystore"))
        assertEq(Unavailable, status(none, "android.hardware-keystore"))
    }

    private fun patchStatus(daysOld: Long): PhoneSecurityStatus {
        val today = PhoneSecurityFixtures.today
        return status(base.copy(securityPatch = today.minusDays(daysOld).toString()), "android.patch-level", today)
    }

    fun testPatchLevelBoundaries() {
        assertEq(Pass, patchStatus(0))
        assertEq(Pass, patchStatus(90))
        assertEq(Review, patchStatus(91))
        assertEq(Review, patchStatus(180))
        assertEq(Warning, patchStatus(181))
        assertEq(Warning, patchStatus(365))
        assertEq(Critical, patchStatus(366))
    }

    fun testPatchLevelDetailAndFix() {
        val old = f(base.copy(securityPatch = "2025-01-01"), "android.patch-level")
        assertEq(Critical, old.status)
        assertEq("Security patch 2025-01-01, 654 days old.", old.detail)
        assertEq(PhoneSecuritySettings.SYSTEM_UPDATE, old.fix?.action)
        assertEq("Security patch 2026-10-17.", f(base.copy(securityPatch = "2026-10-17"), "android.patch-level").detail)
        assertEq("Security patch 2026-10-16, 1 day old.", f(base.copy(securityPatch = "2026-10-16"), "android.patch-level").detail)
    }

    fun testPatchLevelFutureDatePasses() {
        assertEq(Pass, status(base.copy(securityPatch = "2027-01-01"), "android.patch-level"))
    }

    fun testPatchLevelInvalidIsUnavailable() {
        assertEq(Unavailable, status(base.copy(securityPatch = ""), "android.patch-level"))
        assertEq(Unavailable, status(base.copy(securityPatch = "2026-13-40"), "android.patch-level"))
        assertEq(Unavailable, status(none, "android.patch-level"))
    }

    fun testPatchLevelUsesTheGivenClock() {
        val facts = base.copy(securityPatch = "2026-01-01")
        assertEq(Pass, status(facts, "android.patch-level", LocalDate.of(2026, 3, 1)))
        assertEq(Critical, status(facts, "android.patch-level", LocalDate.of(2027, 6, 1)))
        val millis = LocalDate.of(2026, 3, 1).atStartOfDay(java.time.ZoneOffset.UTC).toInstant().toEpochMilli()
        assertEq(Pass, PhoneSecurityScanner.scan(facts, millis).first { it.check.id == "android.patch-level" }.status)
    }

    fun testOsVersion() {
        assertEq(Pass, status(base, "android.os-version"))
        assertEq(Pass, status(base.copy(sdkInt = 34), "android.os-version"))
        assertEq(Review, status(base.copy(sdkInt = 33), "android.os-version"))
        assertEq(Review, status(base.copy(sdkInt = 31), "android.os-version"))
        val old = f(base.copy(sdkInt = 30), "android.os-version")
        assertEq(Warning, old.status)
        assertEq("Android 11 (API 30).", old.detail)
        assertEq(PhoneSecuritySettings.SYSTEM_UPDATE, old.fix?.action)
        assertEq(Unavailable, status(none, "android.os-version"))
    }

    fun testEncryption() {
        val p = PhoneSecurityParsers
        assertEq(Pass, status(base, "android.encryption"))
        assertEq(Pass, status(base.copy(encryptionStatus = p.ENCRYPTION_ACTIVE), "android.encryption"))
        assertEq(Review, status(base.copy(encryptionStatus = p.ENCRYPTION_ACTIVE_DEFAULT_KEY), "android.encryption"))
        assertEq(Critical, status(base.copy(encryptionStatus = p.ENCRYPTION_INACTIVE), "android.encryption"))
        assertEq(Critical, status(base.copy(encryptionStatus = p.ENCRYPTION_UNSUPPORTED), "android.encryption"))
        assertEq(Unavailable, status(base.copy(encryptionStatus = 99), "android.encryption"))
        assertEq(Unavailable, status(none, "android.encryption"))
    }

    fun testDeveloperOptions() {
        assertEq(Pass, status(base, "android.developer-options"))
        val on = f(base.copy(developerOptions = true), "android.developer-options")
        assertEq(Review, on.status)
        assertEq(PhoneSecuritySettings.DEVELOPMENT, on.fix?.action)
        expect(on.usage?.isNotBlank() == true, "developer options say who needs them")
        assertEq(Unavailable, status(none, "android.developer-options"))
    }

    fun testUsbDebugging() {
        assertEq(Pass, status(base, "android.usb-debugging"))
        assertEq(Warning, status(base.copy(usbDebugging = true), "android.usb-debugging"))
        assertEq(Unavailable, status(none, "android.usb-debugging"))
    }

    fun testWirelessDebugging() {
        assertEq(Pass, status(base, "android.wireless-debugging"))
        assertEq(Warning, status(base.copy(wirelessDebugging = true), "android.wireless-debugging"))
        assertEq(Unavailable, status(none, "android.wireless-debugging"))
    }

    fun testAutoTime() {
        assertEq(Pass, status(base, "android.auto-time"))
        val off = f(base.copy(autoTime = false), "android.auto-time")
        assertEq(Review, off.status)
        assertEq(PhoneSecuritySettings.DATE, off.fix?.action)
        assertEq(Unavailable, status(none, "android.auto-time"))
    }

    fun testHttpProxy() {
        assertEq(Pass, status(base, "android.http-proxy"))
        val proxied = f(base.copy(httpProxy = "10.1.2.3:3128"), "android.http-proxy")
        assertEq(Review, proxied.status)
        expect(proxied.usage?.contains("10.1.2.3:3128") == true, "the usage names the proxy")
        assertEq(PhoneSecuritySettings.WIFI, proxied.fix?.action)
        assertEq(Pass, status(base.copy(httpProxy = "  "), "android.http-proxy"))
        assertEq(Unavailable, status(none, "android.http-proxy"))
    }

    fun testVpnAlwaysPasses() {
        val off = f(base, "android.vpn")
        assertEq(Pass, off.status)
        assertEq("No VPN is on.", off.detail)
        val on = f(base.copy(vpnActive = true), "android.vpn")
        assertEq(Pass, on.status)
        assertEq("A VPN carries this phone's traffic.", on.detail)
        assertEq(null, on.fix)
        assertEq(Unavailable, status(none, "android.vpn"))
    }

    fun testUserCertificates() {
        assertEq(Pass, status(base, "android.user-certificates"))
        val one = f(base.copy(userCertificates = listOf("Example Proxy CA")), "android.user-certificates")
        assertEq(Warning, one.status)
        assertEq("1 certificate authority was added to this phone.", one.detail)
        expect(one.usage?.startsWith("Added: Example Proxy CA.") == true, one.usage.toString())
        val two = f(base.copy(userCertificates = listOf("A", "B")), "android.user-certificates")
        assertEq("2 certificate authorities were added to this phone.", two.detail)
        assertEq(Unavailable, status(none, "android.user-certificates"))
    }

    fun testPrivateDnsModes() {
        assertEq(Pass, status(base, "android.private-dns"))
        assertEq("Private DNS uses dns.example.org.", f(base, "android.private-dns").detail)
        assertEq(Pass, status(base.copy(privateDnsMode = "opportunistic"), "android.private-dns"))
        val off = f(base.copy(privateDnsMode = "off", privateDnsActive = false), "android.private-dns")
        assertEq(Review, off.status)
        assertEq(PhoneSecuritySettings.WIRELESS, off.fix?.action)
    }

    fun testPrivateDnsFallsBackToTheNetwork() {
        assertEq(Pass, status(base.copy(privateDnsMode = null, privateDnsActive = true), "android.private-dns"))
        assertEq(Review, status(base.copy(privateDnsMode = null, privateDnsActive = false), "android.private-dns"))
        assertEq(Unavailable, status(base.copy(privateDnsMode = null, privateDnsActive = null), "android.private-dns"))
        assertEq(Unavailable, status(none, "android.private-dns"))
    }

    fun testAccessibilityServicesNameThemInTheUsage() {
        assertEq(Pass, status(base, "android.accessibility-services"))
        val on = f(base.copy(accessibilityServices = listOf("com.google.android.marvin.talkback", "com.example.helper")), "android.accessibility-services")
        assertEq(Review, on.status)
        assertEq("2 accessibility services can read the screen.", on.detail)
        expect(on.usage?.contains("com.google.android.marvin.talkback, com.example.helper") == true, on.usage.toString())
        assertEq(PhoneSecuritySettings.ACCESSIBILITY, on.fix?.action)
        assertEq(Unavailable, status(none, "android.accessibility-services"))
    }

    fun testNotificationListenersIgnoreForkMesh() {
        assertEq(Pass, status(base, "android.notification-listeners"))
        assertEq(Pass, status(base.copy(notificationListeners = listOf("com.forkmesh.forkmesh")), "android.notification-listeners"))
        val other = f(base.copy(notificationListeners = listOf("com.forkmesh.forkmesh", "com.example.watch")), "android.notification-listeners")
        assertEq(Review, other.status)
        assertEq("1 app can read every notification.", other.detail)
        expect(other.usage?.contains("com.example.watch") == true, other.usage.toString())
        expect(other.usage?.contains("com.forkmesh.forkmesh") == false, other.usage.toString())
        assertEq(PhoneSecuritySettings.NOTIFICATION_LISTENERS, other.fix?.action)
        assertEq(Unavailable, status(none, "android.notification-listeners"))
    }

    fun testDeviceAdmins() {
        assertEq(Pass, status(base, "android.device-admins"))
        val one = f(base.copy(deviceAdmins = listOf("com.google.android.apps.work.clouddpc")), "android.device-admins")
        assertEq(Review, one.status)
        assertEq("1 app is a device admin.", one.detail)
        assertEq("2 apps are device admins.", f(base.copy(deviceAdmins = listOf("a.b", "c.d")), "android.device-admins").detail)
        assertEq(Unavailable, status(none, "android.device-admins"))
    }

    fun testKeyboard() {
        assertEq(Pass, status(base, "android.keyboard"))
        assertEq("The keyboard is Gboard.", f(base, "android.keyboard").detail)
        assertEq(Pass, status(base.copy(keyboard = "com.touchtype.swiftkey/com.touchtype.KeyboardService", keyboardIsSystem = false), "android.keyboard"))
        assertEq(Pass, status(base.copy(keyboard = "com.oem.keyboard/.Ime", keyboardIsSystem = true), "android.keyboard"))
        val other = f(base.copy(keyboard = "com.example.keys/.Ime", keyboardIsSystem = false), "android.keyboard")
        assertEq(Review, other.status)
        assertEq(PhoneSecuritySettings.INPUT_METHOD, other.fix?.action)
        assertEq(Review, status(base.copy(keyboard = "com.example.keys/.Ime", keyboardIsSystem = null), "android.keyboard"))
        assertEq(Unavailable, status(base.copy(keyboard = ""), "android.keyboard"))
        assertEq(Unavailable, status(none, "android.keyboard"))
    }

    fun testCameraPermission() {
        val denied = f(base, "android.permission.camera")
        assertEq(Pass, denied.status)
        assertEq(PhoneSecurityUsage.CAMERA, denied.usage)
        val granted = f(base.copy(cameraGranted = true), "android.permission.camera")
        assertEq(Review, granted.status)
        expect(granted.usage?.contains("QR sign-in") == true && granted.usage?.contains("video calls") == true, granted.usage.toString())
        expect(granted.fix?.ownAppDetails == true, "the fix opens ForkMesh's app info")
        assertEq(PhoneSecuritySettings.APP_DETAILS, granted.fix?.action)
        assertEq(Unavailable, status(none, "android.permission.camera"))
    }

    fun testMicrophonePermission() {
        assertEq(Pass, status(base, "android.permission.microphone"))
        val granted = f(base.copy(microphoneGranted = true), "android.permission.microphone")
        assertEq(Review, granted.status)
        expect(granted.usage?.contains("calls") == true && granted.usage?.contains("voice prompts") == true, granted.usage.toString())
        assertEq(Unavailable, status(none, "android.permission.microphone"))
    }

    fun testNearbyPermission() {
        assertEq(Pass, status(base, "android.permission.nearby"))
        val granted = f(base.copy(nearbyGranted = true), "android.permission.nearby")
        assertEq(Review, granted.status)
        expect(granted.usage?.contains("Glasses Alert") == true && granted.usage?.contains("local mesh") == true, granted.usage.toString())
        val old = f(none.copy(sdkInt = 30), "android.permission.nearby")
        assertEq(Unavailable, old.status)
        assertEq("Android before 12 has no Nearby devices permission.", old.detail)
    }

    fun testNotificationsPermission() {
        assertEq(Pass, status(base, "android.permission.notifications"))
        val refused = f(base.copy(notificationsGranted = false), "android.permission.notifications")
        assertEq(Review, refused.status)
        expect(refused.detail.contains("calls and pings cannot ring"), refused.detail)
        expect(refused.fix?.ownAppDetails == true, "the fix opens ForkMesh's app info")
        assertEq(Unavailable, status(none, "android.permission.notifications"))
    }

    fun testEmptyFactsAreAllUnavailable() {
        val findings = PhoneSecurityScanner.scan(none, PhoneSecurityFixtures.today)
        for (finding in findings) assertEq(Unavailable, finding.status, finding.check.id)
        for (finding in findings) expect(finding.detail.isNotBlank(), finding.check.id)
    }
}
