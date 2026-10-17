package com.forkmesh.nativeapp.phonesecurity

enum class PhoneBiometrics { Enrolled, NoneEnrolled, NoHardware }

data class PhoneBuild(
    val fingerprint: String = "",
    val model: String = "",
    val manufacturer: String = "",
    val brand: String = "",
    val device: String = "",
    val product: String = "",
    val hardware: String = "",
    val tags: String = "",
    val type: String = "",
)

data class PhoneSecurityFacts(
    val ownPackage: String? = null,
    val deviceSecure: Boolean? = null,
    val biometrics: PhoneBiometrics? = null,
    val screenTimeoutMillis: Long? = null,
    val lockScreenNotifications: Boolean? = null,
    val lockScreenPrivateContent: Boolean? = null,
    val stayAwakeWhileCharging: Boolean? = null,
    val existingPaths: Set<String>? = null,
    val installedPackages: Set<String>? = null,
    val build: PhoneBuild? = null,
    val procMounts: String? = null,
    val procSelfMaps: String? = null,
    val xposedClassLoadable: Boolean? = null,
    val selinuxEnforce: String? = null,
    val debuggerAttached: Boolean? = null,
    val appDebuggable: Boolean? = null,
    val installer: String? = null,
    val cleartextPermitted: Boolean? = null,
    val relayUrl: String? = null,
    val strongBox: Boolean? = null,
    val hardwareKeystore: Boolean? = null,
    val securityPatch: String? = null,
    val sdkInt: Int? = null,
    val encryptionStatus: Int? = null,
    val developerOptions: Boolean? = null,
    val usbDebugging: Boolean? = null,
    val wirelessDebugging: Boolean? = null,
    val autoTime: Boolean? = null,
    val httpProxy: String? = null,
    val vpnActive: Boolean? = null,
    val userCertificates: List<String>? = null,
    val privateDnsMode: String? = null,
    val privateDnsServer: String? = null,
    val privateDnsActive: Boolean? = null,
    val accessibilityServices: List<String>? = null,
    val notificationListeners: List<String>? = null,
    val deviceAdmins: List<String>? = null,
    val keyboard: String? = null,
    val keyboardIsSystem: Boolean? = null,
    val cameraGranted: Boolean? = null,
    val microphoneGranted: Boolean? = null,
    val nearbyGranted: Boolean? = null,
    val notificationsGranted: Boolean? = null,
)

object PhoneSecurityPaths {
    val su: List<String> = listOf(
        "/sbin/su",
        "/system/bin/su",
        "/system/xbin/su",
        "/system/sbin/su",
        "/system/su",
        "/system/bin/.ext/su",
        "/system/bin/failsafe/su",
        "/system/sd/xbin/su",
        "/system/usr/we-need-root/su",
        "/system_ext/bin/su",
        "/vendor/bin/su",
        "/vendor/xbin/su",
        "/odm/bin/su",
        "/product/bin/su",
        "/data/local/su",
        "/data/local/bin/su",
        "/data/local/xbin/su",
        "/su/bin/su",
        "/cache/su",
        "/dev/su",
    )

    val magisk: List<String> = listOf(
        "/sbin/.magisk",
        "/sbin/magisk",
        "/debug_ramdisk/magisk",
        "/data/adb/magisk",
        "/data/adb/magisk.db",
        "/data/adb/modules",
        "/data/adb/post-fs-data.d",
        "/data/adb/service.d",
        "/cache/.disable_magisk",
        "/cache/magisk.log",
        "/dev/.magisk.unblock",
        "/init.magisk.rc",
        "/system/etc/init/magisk",
        "/data/adb/ksu",
        "/data/adb/ksud",
        "/data/adb/ap",
        "/data/adb/apd",
    )

    val busybox: List<String> = listOf(
        "/system/bin/busybox",
        "/system/xbin/busybox",
        "/system/sbin/busybox",
        "/sbin/busybox",
        "/vendor/bin/busybox",
        "/vendor/xbin/busybox",
        "/system_ext/bin/busybox",
        "/data/local/busybox",
        "/data/local/bin/busybox",
        "/data/local/xbin/busybox",
        "/su/bin/busybox",
        "/su/xbin/busybox",
    )

    val frida: List<String> = listOf(
        "/data/local/tmp/frida-server",
        "/data/local/tmp/re.frida.server",
        "/data/local/tmp/frida",
        "/data/local/tmp/frida-server-arm64",
        "/data/local/tmp/frida-server-arm",
        "/data/local/tmp/frida-server-x86_64",
        "/data/local/tmp/frida-server-x86",
        "/data/local/frida-server",
        "/system/bin/frida-server",
        "/system/xbin/frida-server",
        "/system/bin/frida",
        "/system/xbin/frida",
        "/sbin/frida-server",
        "/vendor/bin/frida-server",
    )

    val xposed: List<String> = listOf(
        "/system/framework/XposedBridge.jar",
        "/system/xposed.prop",
        "/system/lib/libxposed_art.so",
        "/system/lib64/libxposed_art.so",
        "/system/bin/app_process32_xposed",
        "/system/bin/app_process64_xposed",
        "/data/adb/lspd",
        "/data/adb/modules/zygisk_lsposed",
        "/data/adb/modules/riru_lsposed",
        "/data/adb/modules/riru_edxposed",
    )

    val all: List<String> get() = (su + magisk + busybox + frida + xposed).distinct()
}

object PhoneSecurityPackages {
    val rootApps: Map<String, String> = linkedMapOf(
        "com.topjohnwu.magisk" to "Magisk",
        "io.github.huskydg.magisk" to "Magisk Delta",
        "io.github.vvb2060.magisk" to "Magisk Alpha",
        "eu.chainfire.supersu" to "SuperSU",
        "eu.chainfire.supersu.pro" to "SuperSU Pro",
        "com.noshufou.android.su" to "Superuser",
        "com.noshufou.android.su.elite" to "Superuser Elite",
        "com.koushikdutta.superuser" to "Superuser",
        "com.thirdparty.superuser" to "Superuser",
        "me.phh.superuser" to "Superuser",
        "com.yellowes.su" to "Superuser",
        "me.weishu.kernelsu" to "KernelSU",
        "com.rifsxd.ksunext" to "KernelSU Next",
        "me.bmax.apatch" to "APatch",
        "com.kingroot.kinguser" to "KingRoot",
        "com.kingo.root" to "KingoRoot",
    )

    val hookingApps: Map<String, String> = linkedMapOf(
        "de.robv.android.xposed.installer" to "Xposed Installer",
        "org.meowcat.edxposed.manager" to "EdXposed Manager",
        "com.solohsu.android.edxp.manager" to "EdXposed Manager",
        "org.lsposed.manager" to "LSPosed",
        "io.github.lsposed.manager" to "LSPosed",
        "org.lsposed.lspatch" to "LSPatch",
    )

    val all: List<String> get() = (rootApps.keys + hookingApps.keys).distinct()

    val xposedClasses: List<String> = listOf(
        "de.robv.android.xposed.XposedBridge",
        "de.robv.android.xposed.XposedHelpers",
    )

    val trustedInstallers: Map<String, String> = linkedMapOf(
        "com.android.vending" to "Google Play",
        "com.google.android.feedback" to "Google Play",
        "com.sec.android.app.samsungapps" to "Galaxy Store",
        "com.amazon.venezia" to "Amazon Appstore",
        "org.fdroid.fdroid" to "F-Droid",
        "org.fdroid.basic" to "F-Droid",
    )

    val fileInstallers: Set<String> = setOf(
        "com.android.packageinstaller",
        "com.google.android.packageinstaller",
        "com.samsung.android.packageinstaller",
        "com.android.shell",
    )

    val trustedKeyboards: Map<String, String> = linkedMapOf(
        "com.google.android.inputmethod.latin" to "Gboard",
        "com.android.inputmethod.latin" to "Android Keyboard",
        "com.samsung.android.honeyboard" to "Samsung Keyboard",
        "com.sec.android.inputmethod" to "Samsung Keyboard",
        "com.touchtype.swiftkey" to "Microsoft SwiftKey",
        "com.touchtype.swiftkey.beta" to "Microsoft SwiftKey Beta",
    )
}
