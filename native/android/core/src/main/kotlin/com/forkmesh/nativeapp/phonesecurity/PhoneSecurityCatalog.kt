package com.forkmesh.nativeapp.phonesecurity

object PhoneSecurityCatalog {
    private fun check(id: String, title: String, category: String) =
        PhoneSecurityCheck(id, title, category, PhoneSecurityExplanations.text(id))

    private val lock = PhoneSecurityCategory.LOCK_SCREEN
    private val integrity = PhoneSecurityCategory.DEVICE_INTEGRITY
    private val app = PhoneSecurityCategory.APP_PROTECTION
    private val system = PhoneSecurityCategory.SYSTEM
    private val network = PhoneSecurityCategory.NETWORK
    private val access = PhoneSecurityCategory.APPS_WITH_ACCESS
    private val privacy = PhoneSecurityCategory.PRIVACY

    val screenLock = check("android.screen-lock", "Screen lock", lock)
    val biometrics = check("android.biometrics", "Fingerprint or face unlock", lock)
    val lockTimeout = check("android.lock-timeout", "Screen timeout", lock)
    val lockNotifications = check("android.lock-notifications", "Notification content on the lock screen", lock)
    val stayAwake = check("android.stay-awake", "Stay awake while charging", lock)
    val suBinary = check("android.su-binary", "su binary", integrity)
    val rootApps = check("android.root-apps", "Root manager apps", integrity)
    val magiskFiles = check("android.magisk-files", "Root framework files", integrity)
    val busybox = check("android.busybox", "BusyBox", integrity)
    val testKeys = check("android.test-keys", "Release signing", integrity)
    val buildType = check("android.build-type", "Production build", integrity)
    val systemRw = check("android.system-rw", "System partition read-only", integrity)
    val hookingFrameworks = check("android.hooking-frameworks", "Xposed or LSPosed", integrity)
    val injectedLibraries = check("android.injected-libraries", "Code injected into ForkMesh", integrity)
    val fridaServer = check("android.frida-server", "Instrumentation server", integrity)
    val selinux = check("android.selinux", "SELinux enforcing", integrity)
    val emulator = check("android.emulator", "Real device", integrity)
    val debugger = check("android.debugger", "Debugger attached", integrity)
    val appDebuggable = check("android.app-debuggable", "Release build of ForkMesh", app)
    val installSource = check("android.install-source", "Where ForkMesh came from", app)
    val cleartext = check("android.cleartext", "Encrypted connections only", app)
    val relayHttps = check("android.relay-https", "Relay address uses HTTPS", app)
    val hardwareKeystore = check("android.hardware-keystore", "Secure hardware for keys", app)
    val patchLevel = check("android.patch-level", "Security patch level", system)
    val osVersion = check("android.os-version", "Android version", system)
    val encryption = check("android.encryption", "Storage encryption", system)
    val developerOptions = check("android.developer-options", "Developer options", system)
    val usbDebugging = check("android.usb-debugging", "USB debugging", system)
    val wirelessDebugging = check("android.wireless-debugging", "Wireless debugging", system)
    val autoTime = check("android.auto-time", "Automatic date and time", system)
    val httpProxy = check("android.http-proxy", "HTTP proxy", network)
    val vpn = check("android.vpn", "VPN", network)
    val userCertificates = check("android.user-certificates", "Added certificate authorities", network)
    val privateDns = check("android.private-dns", "Private DNS", network)
    val accessibilityServices = check("android.accessibility-services", "Accessibility services", access)
    val notificationListeners = check("android.notification-listeners", "Apps that read notifications", access)
    val deviceAdmins = check("android.device-admins", "Device admin apps", access)
    val keyboard = check("android.keyboard", "Keyboard", access)
    val camera = check("android.permission.camera", "Camera access", privacy)
    val microphone = check("android.permission.microphone", "Microphone access", privacy)
    val nearby = check("android.permission.nearby", "Nearby devices", privacy)
    val notifications = check("android.permission.notifications", "Notifications", privacy)

    val checks: List<PhoneSecurityCheck> = listOf(
        screenLock, biometrics, lockTimeout, lockNotifications, stayAwake,
        suBinary, rootApps, magiskFiles, busybox, testKeys, buildType, systemRw, hookingFrameworks, injectedLibraries,
        fridaServer, selinux, emulator, debugger,
        appDebuggable, installSource, cleartext, relayHttps, hardwareKeystore,
        patchLevel, osVersion, encryption, developerOptions, usbDebugging, wirelessDebugging, autoTime,
        httpProxy, vpn, userCertificates, privateDns,
        accessibilityServices, notificationListeners, deviceAdmins, keyboard,
        camera, microphone, nearby, notifications,
    )

    fun byId(id: String): PhoneSecurityCheck? = checks.firstOrNull { it.id == id }
}
