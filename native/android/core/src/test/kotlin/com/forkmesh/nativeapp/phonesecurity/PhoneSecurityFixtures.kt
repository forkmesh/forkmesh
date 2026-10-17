package com.forkmesh.nativeapp.phonesecurity

import java.time.LocalDate

object PhoneSecurityFixtures {
    val today: LocalDate = LocalDate.of(2026, 10, 17)

    val pixelBuild = PhoneBuild(
        fingerprint = "google/husky/husky:16/BP2A.250805.005/13691446:user/release-keys",
        model = "Pixel 8 Pro",
        manufacturer = "Google",
        brand = "google",
        device = "husky",
        product = "husky",
        hardware = "husky",
        tags = "release-keys",
        type = "user",
    )

    val emulatorBuild = PhoneBuild(
        fingerprint = "google/sdk_gphone64_x86_64/emu64xa:16/BE2A.250530.026.D1/13818094:userdebug/dev-keys",
        model = "sdk_gphone64_x86_64",
        manufacturer = "Google",
        brand = "google",
        device = "emu64xa",
        product = "sdk_gphone64_x86_64",
        hardware = "ranchu",
        tags = "dev-keys",
        type = "userdebug",
    )

    val cleanMounts = """
        /dev/block/dm-6 / erofs ro,seclabel,relatime 0 0
        tmpfs /dev tmpfs rw,seclabel,nosuid,relatime,size=3843564k,nr_inodes=960891,mode=755 0 0
        proc /proc proc rw,relatime,gid=3009,hidepid=invisible 0 0
        /dev/block/dm-7 /system_ext erofs ro,seclabel,relatime 0 0
        /dev/block/dm-8 /vendor erofs ro,seclabel,relatime 0 0
        /dev/block/dm-9 /product erofs ro,seclabel,relatime 0 0
        /dev/block/dm-48 /data f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on 0 0
    """.trimIndent()

    val cleanMaps = """
        12c00000-32c00000 rw-p 00000000 00:00 0                                  [anon:dalvik-main space (region space)]
        6f3d1000-6f5e3000 rw-p 00000000 00:00 0                                  [anon:dalvik-/system/framework/boot.art]
        7a1c200000-7a1c400000 r--p 00000000 fe:2a 1234                           /system/framework/framework.jar
        7a2d000000-7a2d200000 r-xp 00000000 fe:00 2201                           /apex/com.android.art/lib64/libart.so
        7a3e000000-7a3e100000 r-xp 00000000 fe:00 3301                           /system/lib64/libc.so
        7a3e200000-7a3e300000 r-xp 00000000 fe:00 3302                           /system/lib64/libfreetype.so
        7a3e400000-7a3e500000 r-xp 00000000 fe:00 3303                           /system/lib64/libmedia.so
        7a3e600000-7a3e700000 r-xp 00000000 fe:00 3304                           /vendor/lib64/libfridge_sensor.so
        7a3e800000-7a3e900000 r--p 00000000 fe:30 4401                           /data/app/~~a1b2==/com.forkmesh.nativeapp-c3d4==/base.apk
        7a3ea00000-7a3eb00000 r-xp 00000000 00:00 0
        7ffd1c000000-7ffd1c021000 rw-p 00000000 00:00 0                          [stack]
    """.trimIndent()

    fun hardened() = PhoneSecurityFacts(
        ownPackage = "com.forkmesh.forkmesh",
        deviceSecure = true,
        biometrics = PhoneBiometrics.Enrolled,
        screenTimeoutMillis = 30_000,
        lockScreenNotifications = true,
        lockScreenPrivateContent = false,
        stayAwakeWhileCharging = false,
        existingPaths = emptySet(),
        installedPackages = emptySet(),
        build = pixelBuild,
        procMounts = cleanMounts,
        procSelfMaps = cleanMaps,
        xposedClassLoadable = false,
        selinuxEnforce = "1\n",
        debuggerAttached = false,
        appDebuggable = false,
        installer = "com.android.vending",
        cleartextPermitted = false,
        relayUrl = "wss://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws",
        strongBox = true,
        hardwareKeystore = true,
        securityPatch = "2026-10-05",
        sdkInt = 36,
        encryptionStatus = PhoneSecurityParsers.ENCRYPTION_ACTIVE_PER_USER,
        developerOptions = false,
        usbDebugging = false,
        wirelessDebugging = false,
        autoTime = true,
        httpProxy = "",
        vpnActive = false,
        userCertificates = emptyList(),
        privateDnsMode = "hostname",
        privateDnsServer = "dns.example.org",
        privateDnsActive = true,
        accessibilityServices = emptyList(),
        notificationListeners = emptyList(),
        deviceAdmins = emptyList(),
        keyboard = "com.google.android.inputmethod.latin/com.android.inputmethod.latin.LatinIME",
        keyboardIsSystem = true,
        cameraGranted = false,
        microphoneGranted = false,
        nearbyGranted = false,
        notificationsGranted = true,
    )

    fun rooted() = hardened().copy(
        existingPaths = setOf("/system/xbin/su", "/data/adb/magisk", "/system/xbin/busybox", "/data/local/tmp/frida-server"),
        installedPackages = setOf("com.topjohnwu.magisk", "org.lsposed.manager"),
        build = pixelBuild.copy(tags = "test-keys", type = "userdebug"),
        procMounts = cleanMounts.replace("/dev/block/dm-8 /vendor erofs ro,", "/dev/block/dm-8 /vendor erofs rw,"),
        procSelfMaps = cleanMaps + "\n7b00000000-7b00100000 r-xp 00000000 00:05 9911                           /memfd:frida-agent-64.so (deleted)",
        xposedClassLoadable = true,
        selinuxEnforce = "0",
    )

    fun empty() = PhoneSecurityFacts()

    fun finding(
        facts: PhoneSecurityFacts,
        id: String,
        today: LocalDate = PhoneSecurityFixtures.today,
    ): PhoneSecurityFinding = PhoneSecurityScanner.scan(facts, today).first { it.check.id == id }
}
