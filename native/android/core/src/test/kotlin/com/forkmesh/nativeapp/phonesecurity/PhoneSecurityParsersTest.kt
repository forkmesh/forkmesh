package com.forkmesh.nativeapp.phonesecurity

import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityParsers as P
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.LocalDate

class PhoneSecurityParsersTest {
    private val today = LocalDate.of(2026, 10, 17)

    fun testCleanMountsHaveNoWritableSystem() {
        assertEq(emptyList<String>(), P.writableSystemMounts(PhoneSecurityFixtures.cleanMounts))
    }

    fun testWritableRootIsFound() {
        assertEq(listOf("/"), P.writableSystemMounts("/dev/block/dm-0 / ext4 rw,seclabel,relatime 0 0"))
    }

    fun testWritableSystemIsFound() {
        assertEq(listOf("/system"), P.writableSystemMounts("/dev/block/bootdevice/by-name/system /system ext4 rw,seclabel,relatime,data=ordered 0 0"))
    }

    fun testWritableVendorIsFound() {
        assertEq(listOf("/vendor"), P.writableSystemMounts("/dev/block/dm-1 /vendor ext4 rw,seclabel 0 0"))
    }

    fun testAllThreeWritableAreReportedInOrder() {
        val text = """
            /dev/block/dm-1 /vendor ext4 rw 0 0
            /dev/block/dm-0 /system ext4 rw 0 0
            /dev/root / ext4 rw 0 0
        """.trimIndent()
        assertEq(listOf("/", "/system", "/vendor"), P.writableSystemMounts(text))
    }

    fun testOtherWritableMountsAreIgnored() {
        val text = """
            /dev/block/dm-48 /data f2fs rw,lazytime 0 0
            tmpfs /dev tmpfs rw,nosuid 0 0
            /dev/block/dm-7 /system_ext erofs rw 0 0
            /dev/block/dm-9 /vendor_dlkm erofs rw 0 0
            /dev/block/dm-9 /system/bin ext4 rw 0 0
        """.trimIndent()
        assertEq(emptyList<String>(), P.writableSystemMounts(text))
    }

    fun testRwMustBeAWholeOption() {
        assertEq(emptyList<String>(), P.writableSystemMounts("/dev/block/dm-0 /system ext4 ro,rwsomething,norw 0 0"))
    }

    fun testRamdiskRootIsIgnored() {
        assertEq(emptyList<String>(), P.writableSystemMounts("rootfs / rootfs rw,seclabel 0 0"))
        assertEq(emptyList<String>(), P.writableSystemMounts("tmpfs / tmpfs rw,seclabel 0 0"))
        assertEq(listOf("/system"), P.writableSystemMounts("rootfs / rootfs rw 0 0\n/dev/block/system /system ext4 rw 0 0"))
    }

    fun testLaterMountOverridesEarlierOne() {
        assertEq(listOf("/system"), P.writableSystemMounts("/dev/a /system ext4 ro 0 0\n/dev/b /system ext4 rw 0 0"))
        assertEq(emptyList<String>(), P.writableSystemMounts("/dev/a /system ext4 rw 0 0\n/dev/b /system ext4 ro 0 0"))
    }

    fun testMalformedMountLinesAreSkipped() {
        assertEq(emptyList<String>(), P.writableSystemMounts("garbage\n/system rw\n\n   "))
    }

    fun testCleanMapsHaveNoInjectedLibraries() {
        assertEq(emptyList<InjectedLibrary>(), P.injectedLibraries(PhoneSecurityFixtures.cleanMaps))
    }

    private fun mapsLine(path: String) = "7b00000000-7b00100000 r-xp 00000000 fe:30 9911                           $path"

    private fun familyOf(path: String): InjectedFamily? = P.injectedLibraries(mapsLine(path)).firstOrNull()?.family

    fun testFridaAgentIsFound() {
        assertEq(InjectedFamily.Frida, familyOf("/data/local/tmp/frida-agent-64.so"))
        assertEq(InjectedFamily.Frida, familyOf("/memfd:frida-agent-64.so (deleted)"))
    }

    fun testFridaGadgetIsFound() {
        assertEq(InjectedFamily.Frida, familyOf("/data/app/~~x==/com.example-y==/lib/arm64/libfrida-gadget.so"))
        assertEq(InjectedFamily.Frida, familyOf("/data/app/x/lib/arm64/frida-gadget.so"))
    }

    fun testLibFridaIsFound() {
        assertEq(InjectedFamily.Frida, familyOf("/data/local/tmp/libfrida-core.so"))
    }

    fun testReFridaFolderIsFound() {
        assertEq(InjectedFamily.Frida, familyOf("/data/local/tmp/re.frida.server/frida-agent-32.so"))
        val names = P.injectedLibraries(mapsLine("/data/local/tmp/re.frida.server/frida-agent-32.so")).map { it.name }
        assertEq(listOf("re.frida.server", "frida-agent-32.so"), names)
    }

    fun testSubstrateIsFound() {
        assertEq(InjectedFamily.Substrate, familyOf("/data/app/com.saurik.substrate-1/lib/arm/libsubstrate.so"))
        assertEq(InjectedFamily.Substrate, familyOf("/data/app/com.saurik.substrate-1/lib/arm/libsubstrate-dvm.so"))
    }

    fun testXposedBridgeIsFound() {
        assertEq(InjectedFamily.Xposed, familyOf("/system/framework/XposedBridge.jar"))
    }

    fun testLsposedIsFound() {
        assertEq(InjectedFamily.Xposed, familyOf("/data/adb/modules/zygisk_lsposed/lib64/liblspd.so"))
    }

    fun testRiruIsFound() {
        assertEq(InjectedFamily.Xposed, familyOf("/system/lib64/libriru_lsposed.so"))
    }

    fun testEdxposedIsFound() {
        assertEq(InjectedFamily.Xposed, familyOf("/system/lib64/libedxp.so"))
        assertEq(InjectedFamily.Xposed, familyOf("/data/misc/edxp_1234/framework/edxp.dex"))
    }

    fun testMatchingIsCaseInsensitive() {
        assertEq(InjectedFamily.Frida, familyOf("/data/local/tmp/FRIDA-AGENT-64.SO"))
        assertEq(InjectedFamily.Xposed, familyOf("/system/framework/xposedbridge.jar"))
        assertEq(InjectedFamily.Substrate, familyOf("/data/LibSubstrate.so"))
    }

    fun testLookalikeLibrariesAreNotFlagged() {
        for (path in listOf(
            "/vendor/lib64/libfridge_sensor.so",
            "/system/lib64/libmediaplayerservice.so",
            "/system/lib64/libsubstitute_text.so",
            "/system/lib64/libfreetype.so",
            "/system/lib64/libxposure.so",
            "/data/app/com.example.friday-1/base.apk",
            "/system/lib64/libmedxplayer.so",
            "/system/lib64/libgui.so",
            "[anon:scudo:primary]",
        )) {
            assertEq(null, familyOf(path), path)
        }
    }

    fun testLinesWithoutAPathAreSkipped() {
        assertEq(emptyList<InjectedLibrary>(), P.injectedLibraries("7a3ea00000-7a3eb00000 r-xp 00000000 00:00 0\n\nnot a maps line"))
    }

    fun testRepeatedLibrariesAreReportedOnce() {
        val text = (1..4).joinToString("\n") { mapsLine("/data/local/tmp/frida-agent-64.so") }
        assertEq(1, P.injectedLibraries(text).size)
    }

    fun testAccessibilityStringParsesPackages() {
        assertEq(
            listOf("com.google.android.marvin.talkback", "com.example.helper"),
            P.accessibilityPackages("com.google.android.marvin.talkback/.TalkBackService:com.example.helper/com.example.helper.Svc"),
        )
    }

    fun testAccessibilityStringDedupesAndTrims() {
        assertEq(listOf("pkg", "pkg2"), P.accessibilityPackages(" pkg/.Cls : pkg/.Other:pkg2/cls2:"))
    }

    fun testEmptyAccessibilityStringHasNoPackages() {
        assertEq(emptyList<String>(), P.accessibilityPackages(""))
        assertEq(emptyList<String>(), P.accessibilityPackages(":::"))
    }

    fun testAccessibilityEntryWithoutClassKeepsPackage() {
        assertEq(listOf("com.example.only"), P.accessibilityPackages("com.example.only"))
    }

    fun testPhysicalPhoneIsNotAnEmulator() {
        expect(!P.isEmulator(PhoneSecurityFixtures.pixelBuild), "a Pixel is a real phone")
        expect(!P.isEmulator(PhoneBuild("samsung/dm3qxxx/dm3q:16/BP2A/S918BXXU:user/release-keys", "SM-S918B", "samsung", "samsung", "dm3q", "dm3qxxx", "qcom")), "a Galaxy is a real phone")
    }

    fun testStudioEmulatorIsDetected() {
        expect(P.isEmulator(PhoneSecurityFixtures.emulatorBuild), "sdk_gphone is an emulator")
    }

    fun testEachEmulatorSignalIsEnough() {
        val base = PhoneSecurityFixtures.pixelBuild
        val variants = listOf(
            base.copy(fingerprint = "generic/sdk/generic:8.0.0/OSR1/1:userdebug/test-keys"),
            base.copy(fingerprint = "unknown/unknown/unknown"),
            base.copy(model = "google_sdk"),
            base.copy(model = "Android SDK built for x86"),
            base.copy(model = "Emulator"),
            base.copy(manufacturer = "Genymotion"),
            base.copy(brand = "generic", device = "generic_x86"),
            base.copy(product = "sdk_google_phone_x86"),
            base.copy(product = "vbox86p"),
            base.copy(hardware = "goldfish"),
            base.copy(hardware = "ranchu"),
        )
        for (build in variants) expect(P.isEmulator(build), build.toString())
    }

    fun testGenericBrandAloneIsNotAnEmulator() {
        expect(!P.isEmulator(PhoneSecurityFixtures.pixelBuild.copy(brand = "generic")), "brand alone is not enough")
    }

    fun testPatchDateParses() {
        assertEq(LocalDate.of(2026, 10, 5), P.patchDate("2026-10-05"))
        assertEq(LocalDate.of(2026, 10, 5), P.patchDate(" 2026-10-05\n"))
    }

    fun testInvalidPatchDatesAreRejected() {
        for (text in listOf("", "2026-13-01", "2026-02-30", "2026/10/05", "Oct 2026", "2026-10", "26-10-05", "2026-10-05T00:00")) {
            assertEq(null, P.patchDate(text), text)
        }
    }

    fun testPatchAgeInDays() {
        assertEq(12L, P.patchAgeDays("2026-10-05", today))
        assertEq(0L, P.patchAgeDays("2026-10-17", today))
        assertEq(-14L, P.patchAgeDays("2026-10-31", today))
        assertEq(null, P.patchAgeDays("soon", today))
    }

    fun testPatchStatusBoundaries() {
        assertEq(PhoneSecurityStatus.Pass, P.patchStatus(-5))
        assertEq(PhoneSecurityStatus.Pass, P.patchStatus(0))
        assertEq(PhoneSecurityStatus.Pass, P.patchStatus(90))
        assertEq(PhoneSecurityStatus.Review, P.patchStatus(91))
        assertEq(PhoneSecurityStatus.Review, P.patchStatus(180))
        assertEq(PhoneSecurityStatus.Warning, P.patchStatus(181))
        assertEq(PhoneSecurityStatus.Warning, P.patchStatus(365))
        assertEq(PhoneSecurityStatus.Critical, P.patchStatus(366))
        assertEq(PhoneSecurityStatus.Critical, P.patchStatus(2000))
    }

    fun testOsStatusBoundaries() {
        assertEq(PhoneSecurityStatus.Pass, P.osStatus(37))
        assertEq(PhoneSecurityStatus.Pass, P.osStatus(34))
        assertEq(PhoneSecurityStatus.Review, P.osStatus(33))
        assertEq(PhoneSecurityStatus.Review, P.osStatus(31))
        assertEq(PhoneSecurityStatus.Warning, P.osStatus(30))
        assertEq(PhoneSecurityStatus.Warning, P.osStatus(26))
    }

    fun testAndroidVersionNames() {
        assertEq("Android 14 (API 34)", P.androidVersionName(34))
        assertEq("Android 12L (API 32)", P.androidVersionName(32))
        assertEq("Android 8.0 (API 26)", P.androidVersionName(26))
        assertEq("Android API 99", P.androidVersionName(99))
    }

    fun testTimeoutBoundaries() {
        assertEq(PhoneSecurityStatus.Pass, P.timeoutStatus(15_000))
        assertEq(PhoneSecurityStatus.Pass, P.timeoutStatus(300_000))
        assertEq(PhoneSecurityStatus.Review, P.timeoutStatus(300_001))
        assertEq(PhoneSecurityStatus.Review, P.timeoutStatus(1_800_000))
        assertEq(PhoneSecurityStatus.Warning, P.timeoutStatus(1_800_001))
        assertEq(PhoneSecurityStatus.Warning, P.timeoutStatus(Int.MAX_VALUE.toLong()))
        assertEq(PhoneSecurityStatus.Warning, P.timeoutStatus(-1))
    }

    fun testDurationText() {
        assertEq("30 seconds", P.durationText(30_000))
        assertEq("1 second", P.durationText(1_000))
        assertEq("1 minute", P.durationText(60_000))
        assertEq("10 minutes", P.durationText(600_000))
        assertEq("2 hours", P.durationText(7_200_000))
        assertEq("never", P.durationText(Int.MAX_VALUE.toLong()))
        assertEq("never", P.durationText(-1))
    }

    fun testTrustedInstallers() {
        assertEq("Google Play", P.installerName("com.android.vending"))
        assertEq("Galaxy Store", P.installerName("com.sec.android.app.samsungapps"))
        assertEq("Amazon Appstore", P.installerName("com.amazon.venezia"))
        assertEq("F-Droid", P.installerName("org.fdroid.fdroid"))
        expect(!P.isTrustedInstaller("com.google.android.packageinstaller"), "the package installer is not a store")
        expect(!P.isTrustedInstaller(""), "no installer is not a store")
        expect(!P.isTrustedInstaller("com.android.vending.fake"), "lookalikes are not stores")
    }

    fun testTrustedKeyboards() {
        expect(P.isTrustedKeyboard("com.google.android.inputmethod.latin"), "Gboard")
        expect(P.isTrustedKeyboard("com.samsung.android.honeyboard"), "Samsung")
        expect(P.isTrustedKeyboard("com.touchtype.swiftkey"), "SwiftKey")
        expect(!P.isTrustedKeyboard("com.example.keyboard"), "unknown keyboard")
        assertEq("Gboard", P.keyboardName("com.google.android.inputmethod.latin"))
        assertEq("com.example.keyboard", P.keyboardName("com.example.keyboard"))
    }

    fun testComponentPackage() {
        assertEq("com.google.android.inputmethod.latin", P.packageOfComponent("com.google.android.inputmethod.latin/com.android.inputmethod.latin.LatinIME"))
        assertEq("pkg", P.packageOfComponent("pkg"))
    }

    fun testSecureRelaySchemes() {
        assertEq(RelayVerdict.Secure, P.relayVerdict("https://app.forkmesh.com"))
        assertEq(RelayVerdict.Secure, P.relayVerdict("wss://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws"))
        assertEq(RelayVerdict.Secure, P.relayVerdict("HTTPS://APP.FORKMESH.COM"))
    }

    fun testLocalPlainRelays() {
        for (url in listOf(
            "http://localhost:8787", "ws://localhost:8787/ws", "http://127.0.0.1:8787", "http://127.4.5.6",
            "http://[::1]:8787", "ws://10.0.2.2:8787/ws", "http://forkmesh.local", "http://desk.LOCAL:8080", "http://app.localhost",
        )) {
            assertEq(RelayVerdict.LocalPlain, P.relayVerdict(url), url)
        }
    }

    fun testRemotePlainRelays() {
        for (url in listOf(
            "http://app.forkmesh.com", "ws://203.0.113.9:8787/ws", "http://192.168.1.20", "http://localhost.example.com",
            "http://127.0.0.1.example.com", "http://local", "http://128.0.0.1",
        )) {
            assertEq(RelayVerdict.Plain, P.relayVerdict(url), url)
        }
    }

    fun testUnreadableRelays() {
        for (url in listOf("", "app.forkmesh.com", "ftp://app.forkmesh.com", "https://", "http://bad host")) {
            assertEq(RelayVerdict.Unknown, P.relayVerdict(url), url)
        }
    }

    fun testLoopbackRange() {
        expect(P.isLocalHost("127.0.0.1"), "127.0.0.1")
        expect(P.isLocalHost("127.255.255.255"), "127.255.255.255")
        expect(!P.isLocalHost("127.256.0.1"), "out of range")
        expect(!P.isLocalHost("127.0.0"), "too short")
        expect(!P.isLocalHost(""), "empty")
    }

    fun testCommonNameFromDistinguishedName() {
        assertEq("Example Root CA", P.commonName("CN=Example Root CA,O=Example Corp,C=US"))
        assertEq("Proxy, Inc. CA", P.commonName("CN=\"Proxy, Inc. CA\",O=Proxy"))
        assertEq("Example Corp", P.commonName("O=Example Corp,C=US"))
        assertEq("weird", P.commonName("weird"))
        assertEq("Lower", P.commonName("cn=Lower"))
    }

    fun testEncryptionStatusCodes() {
        assertEq(PhoneSecurityStatus.Pass, P.encryptionStatus(P.ENCRYPTION_ACTIVE_PER_USER))
        assertEq(PhoneSecurityStatus.Pass, P.encryptionStatus(P.ENCRYPTION_ACTIVE))
        assertEq(PhoneSecurityStatus.Review, P.encryptionStatus(P.ENCRYPTION_ACTIVE_DEFAULT_KEY))
        assertEq(PhoneSecurityStatus.Review, P.encryptionStatus(P.ENCRYPTION_ACTIVATING))
        assertEq(PhoneSecurityStatus.Critical, P.encryptionStatus(P.ENCRYPTION_INACTIVE))
        assertEq(PhoneSecurityStatus.Critical, P.encryptionStatus(P.ENCRYPTION_UNSUPPORTED))
        assertEq(null, P.encryptionStatus(42))
    }

    fun testEncryptionCodesMatchDevicePolicyManager() {
        assertEq(listOf(0, 1, 2, 3, 4, 5), listOf(
            P.ENCRYPTION_UNSUPPORTED, P.ENCRYPTION_INACTIVE, P.ENCRYPTION_ACTIVATING,
            P.ENCRYPTION_ACTIVE, P.ENCRYPTION_ACTIVE_DEFAULT_KEY, P.ENCRYPTION_ACTIVE_PER_USER,
        ))
    }
}
