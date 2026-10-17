package com.forkmesh.nativeapp.phonesecurity

import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityCatalog as C
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityStatus.Critical
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityStatus.Pass
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityStatus.Review
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityStatus.Unavailable
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityStatus.Warning
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.ZoneOffset

object PhoneSecurityUsage {
    const val CAMERA = "ForkMesh uses the camera for QR sign-in, photo capture and video calls."
    const val MICROPHONE = "ForkMesh uses the microphone for voice and video calls, voice prompts and dictation."
    const val NEARBY = "ForkMesh uses nearby devices for Glasses Alert and the local mesh with phones and computers near you."
    const val NOTIFICATIONS = "ForkMesh uses notifications to ring for calls, deliver pings and messages, and sound Glasses Alert alarms."
    const val DEVELOPER = "Only app developers need developer options; turning them off also turns off USB and wireless debugging."
    const val PROXY = "Company and school networks sometimes need a proxy. If you did not set one up, remove it."
    const val ACCESSIBILITY = "Keep only services you rely on, such as a screen reader, password manager or switch access."
    const val NOTIFICATION_LISTENERS = "Smartwatch companions, car apps and notification organisers need this access. Remove any you do not recognise."
    const val DEVICE_ADMINS = "Work profiles, school or company management and find-my-phone tools need device admin. Remove any you do not recognise."
    const val CERTIFICATES = "Company Wi-Fi, VPNs and debugging tools sometimes install one. ForkMesh does not trust added certificates, but your browser and other apps may."
}

object PhoneSecurityScanner {
    private val unreadable = "Android did not let ForkMesh read this."

    fun scan(facts: PhoneSecurityFacts, today: LocalDate): List<PhoneSecurityFinding> = sorted(
        listOf(
            screenLock(facts), biometrics(facts), lockTimeout(facts), lockNotifications(facts), stayAwake(facts),
            suBinary(facts), rootApps(facts), magiskFiles(facts), busybox(facts), testKeys(facts), buildType(facts),
            systemRw(facts), hookingFrameworks(facts), injectedLibraries(facts), fridaServer(facts), selinux(facts),
            emulator(facts), debugger(facts),
            appDebuggable(facts), installSource(facts), cleartext(facts), relayHttps(facts), hardwareKeystore(facts),
            patchLevel(facts, today), osVersion(facts), encryption(facts), developerOptions(facts), usbDebugging(facts),
            wirelessDebugging(facts), autoTime(facts),
            httpProxy(facts), vpn(facts), userCertificates(facts), privateDns(facts),
            accessibilityServices(facts), notificationListeners(facts), deviceAdmins(facts), keyboard(facts),
            camera(facts), microphone(facts), nearby(facts), notifications(facts),
        ).map(::withoutIdleFix),
    )

    fun scan(facts: PhoneSecurityFacts, nowMillis: Long, zone: ZoneId = ZoneOffset.UTC): List<PhoneSecurityFinding> =
        scan(facts, Instant.ofEpochMilli(nowMillis).atZone(zone).toLocalDate())

    fun sorted(findings: List<PhoneSecurityFinding>): List<PhoneSecurityFinding> =
        findings.sortedWith(compareBy<PhoneSecurityFinding> { it.status.rank }.thenBy { it.check.title.lowercase() }.thenBy { it.check.id })

    private fun withoutIdleFix(finding: PhoneSecurityFinding): PhoneSecurityFinding =
        if (finding.status == Pass || finding.status == Unavailable) finding.copy(fix = null) else finding

    private fun finding(check: PhoneSecurityCheck, status: PhoneSecurityStatus, detail: String, usage: String? = null, fix: PhoneSecurityFix? = null) =
        PhoneSecurityFinding(check, status, detail, usage, fix)

    private fun unavailable(check: PhoneSecurityCheck, detail: String = unreadable, usage: String? = null) =
        PhoneSecurityFinding(check, Unavailable, detail, usage)

    private fun names(items: Collection<String>): String = items.joinToString(", ")

    private fun found(paths: List<String>, facts: PhoneSecurityFacts): List<String>? =
        facts.existingPaths?.let { existing -> paths.filter { it in existing } }

    private val securityFix = PhoneSecurityFix("Open security settings", "Open Security in Settings.", PhoneSecuritySettings.SECURITY)
    private val developerFix = PhoneSecurityFix(
        "Open developer options",
        "Open Developer options in Settings to turn the switch off.",
        PhoneSecuritySettings.DEVELOPMENT,
    )
    private val appDetailsFix = PhoneSecurityFix(
        "Open ForkMesh app info",
        "Open ForkMesh's page in Settings, then Permissions, to change what it may use.",
        PhoneSecuritySettings.APP_DETAILS,
        ownAppDetails = true,
    )
    private val updateFix = PhoneSecurityFix(
        "Check for updates",
        "Open System update in Settings and install what is offered.",
        PhoneSecuritySettings.SYSTEM_UPDATE,
    )

    fun screenLock(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.screenLock
        val secure = facts.deviceSecure ?: return unavailable(check)
        return if (secure) {
            finding(check, Pass, "A PIN, pattern or password locks this phone.")
        } else {
            finding(
                check, Critical, "This phone has no PIN, pattern or password.",
                fix = PhoneSecurityFix("Set a screen lock", "Open Security in Settings and choose Screen lock.", PhoneSecuritySettings.SECURITY),
            )
        }
    }

    fun biometrics(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.biometrics
        return when (facts.biometrics ?: return unavailable(check)) {
            PhoneBiometrics.Enrolled -> finding(check, Pass, "A fingerprint or face is set up to unlock this phone.")
            PhoneBiometrics.NoHardware -> finding(check, Pass, "This phone has no fingerprint or face unlock hardware.")
            PhoneBiometrics.NoneEnrolled -> finding(
                check, Review, "This phone supports fingerprint or face unlock, but none is set up.",
                fix = PhoneSecurityFix("Set up fingerprint or face", "Open Settings to add a fingerprint or face.", PhoneSecuritySettings.BIOMETRIC_ENROLL),
            )
        }
    }

    fun lockTimeout(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.lockTimeout
        val millis = facts.screenTimeoutMillis ?: return unavailable(check)
        val status = PhoneSecurityParsers.timeoutStatus(millis)
        val length = PhoneSecurityParsers.durationText(millis)
        val detail = if (length == "never") "The screen never turns off by itself." else "The screen turns off after $length."
        return finding(
            check, status, detail,
            fix = PhoneSecurityFix("Shorten screen timeout", "Open Display in Settings and choose Screen timeout.", PhoneSecuritySettings.DISPLAY),
        )
    }

    fun lockNotifications(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.lockNotifications
        if (facts.lockScreenNotifications == false) return finding(check, Pass, "Notifications are hidden on the lock screen.")
        val privateContent = facts.lockScreenPrivateContent ?: return unavailable(check, "Android does not let apps read the lock screen notification setting on this phone.")
        return if (privateContent) {
            finding(
                check, Review, "The lock screen shows the full text of notifications.",
                fix = PhoneSecurityFix(
                    "Hide sensitive content",
                    "Open Notifications in Settings and turn off sensitive notifications on the lock screen.",
                    PhoneSecuritySettings.NOTIFICATIONS,
                ),
            )
        } else {
            finding(check, Pass, "The lock screen hides notification content until you unlock.")
        }
    }

    fun stayAwake(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.stayAwake
        val on = facts.stayAwakeWhileCharging ?: return unavailable(check)
        return if (on) {
            finding(check, Review, "The screen stays on while the phone charges.", PhoneSecurityUsage.DEVELOPER, developerFix)
        } else {
            finding(check, Pass, "The screen turns off normally while charging.")
        }
    }

    fun suBinary(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.suBinary
        val hits = found(PhoneSecurityPaths.su, facts) ?: return unavailable(check)
        return if (hits.isEmpty()) {
            finding(check, Pass, "No su binary in ${PhoneSecurityPaths.su.size} common root locations.")
        } else {
            finding(check, Critical, "Found su at ${names(hits)}.", "Found: ${names(hits)}")
        }
    }

    fun rootApps(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.rootApps
        val installed = facts.installedPackages ?: return unavailable(check)
        val hits = PhoneSecurityPackages.rootApps.filterKeys { it in installed }
        return if (hits.isEmpty()) {
            finding(check, Pass, "No known root manager app is installed.")
        } else {
            finding(
                check, Critical, "Installed: ${names(hits.values.distinct())}.",
                "Packages: ${names(hits.keys)}",
            )
        }
    }

    fun magiskFiles(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.magiskFiles
        val hits = found(PhoneSecurityPaths.magisk, facts) ?: return unavailable(check)
        return if (hits.isEmpty()) {
            finding(check, Pass, "No Magisk, KernelSU or APatch folders found.")
        } else {
            finding(check, Critical, "Found root framework files at ${names(hits)}.", "Found: ${names(hits)}")
        }
    }

    fun busybox(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.busybox
        val hits = found(PhoneSecurityPaths.busybox, facts) ?: return unavailable(check)
        return if (hits.isEmpty()) {
            finding(check, Pass, "No BusyBox in the system folders.")
        } else {
            finding(check, Warning, "Found BusyBox at ${names(hits)}.", "Found: ${names(hits)}")
        }
    }

    fun testKeys(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.testKeys
        val build = facts.build ?: return unavailable(check)
        if (build.tags.isBlank()) return unavailable(check, "Android reported no build signing tags.")
        return if (build.tags.lowercase().split(',').any { it.trim() == "test-keys" }) {
            finding(check, Warning, "The system is signed with public test keys (${build.tags}).")
        } else {
            finding(check, Pass, "The system is signed with ${build.tags}.")
        }
    }

    fun buildType(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.buildType
        val build = facts.build ?: return unavailable(check)
        val type = build.type.trim().lowercase()
        if (type.isEmpty()) return unavailable(check, "Android reported no build type.")
        return if (type == "userdebug" || type == "eng") {
            finding(check, Warning, "This phone runs a $type build, meant for developers.")
        } else {
            finding(check, Pass, "This phone runs a $type build.")
        }
    }

    fun systemRw(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.systemRw
        val mounts = facts.procMounts?.takeIf { it.isNotBlank() } ?: return unavailable(check)
        val writable = PhoneSecurityParsers.writableSystemMounts(mounts)
        return if (writable.isEmpty()) {
            finding(check, Pass, "The system partitions are mounted read-only.")
        } else {
            finding(check, Warning, "Mounted writable: ${names(writable)}.")
        }
    }

    fun hookingFrameworks(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.hookingFrameworks
        val apps = facts.installedPackages?.let { installed -> PhoneSecurityPackages.hookingApps.filterKeys { it in installed } }
        val classes = facts.xposedClassLoadable
        val libraries = facts.procSelfMaps?.let { maps -> PhoneSecurityParsers.injectedLibraries(maps).filter { it.family == InjectedFamily.Xposed } }
        val files = found(PhoneSecurityPaths.xposed, facts)
        if (apps == null && classes == null && libraries == null && files == null) return unavailable(check)
        val evidence = buildList {
            apps?.takeIf { it.isNotEmpty() }?.let { add("installed: ${names(it.values.distinct())}") }
            if (classes == true) add("Xposed classes load inside ForkMesh")
            libraries?.takeIf { it.isNotEmpty() }?.let { found -> add("loaded: ${names(found.map { it.name })}") }
            files?.takeIf { it.isNotEmpty() }?.let { add("files: ${names(it)}") }
        }
        return if (evidence.isEmpty()) {
            finding(check, Pass, "No Xposed, LSPosed or EdXposed found.")
        } else {
            finding(check, Critical, "Signs of a hooking framework: ${evidence.joinToString("; ")}.", evidence.joinToString("; ").replaceFirstChar { it.uppercase() })
        }
    }

    fun injectedLibraries(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.injectedLibraries
        val maps = facts.procSelfMaps?.takeIf { it.isNotBlank() } ?: return unavailable(check)
        val libraries = PhoneSecurityParsers.injectedLibraries(maps)
        return if (libraries.isEmpty()) {
            finding(check, Pass, "No Frida, Substrate or Xposed code is loaded into ForkMesh.")
        } else {
            val families = libraries.map { it.family.label }.distinct()
            finding(check, Critical, "${names(families)} code is loaded into ForkMesh.", "Loaded: ${names(libraries.map { it.name })}")
        }
    }

    fun fridaServer(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.fridaServer
        val hits = found(PhoneSecurityPaths.frida, facts) ?: return unavailable(check)
        return if (hits.isEmpty()) {
            finding(check, Pass, "No Frida server in /data/local/tmp or the system folders.")
        } else {
            finding(check, Critical, "Found a Frida server at ${names(hits)}.", "Found: ${names(hits)}")
        }
    }

    fun selinux(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.selinux
        val value = facts.selinuxEnforce?.trim() ?: return unavailable(check, "Android does not let apps read the SELinux mode on this phone.")
        return when (value) {
            "1" -> finding(check, Pass, "SELinux is enforcing.")
            "0" -> finding(check, Critical, "SELinux is permissive: it logs rule breaks but does not stop them.")
            else -> unavailable(check, "Android does not let apps read the SELinux mode on this phone.")
        }
    }

    fun emulator(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.emulator
        val build = facts.build ?: return unavailable(check)
        return if (PhoneSecurityParsers.isEmulator(build)) {
            finding(check, Review, "This looks like an emulator (${listOf(build.manufacturer, build.model).filter { it.isNotBlank() }.joinToString(" ")}).")
        } else {
            finding(check, Pass, "This is a ${listOf(build.manufacturer, build.model).filter { it.isNotBlank() }.joinToString(" ").ifEmpty { "real device" }}.")
        }
    }

    fun debugger(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.debugger
        val attached = facts.debuggerAttached ?: return unavailable(check)
        return when {
            !attached -> finding(check, Pass, "No debugger is attached to ForkMesh.")
            facts.appDebuggable == true -> finding(check, Review, "A debugger is attached to this development build of ForkMesh.", PhoneSecurityUsage.DEVELOPER, developerFix)
            else -> finding(check, Warning, "A debugger is attached to ForkMesh.", PhoneSecurityUsage.DEVELOPER, developerFix)
        }
    }

    fun appDebuggable(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.appDebuggable
        val debuggable = facts.appDebuggable ?: return unavailable(check)
        return if (debuggable) {
            finding(check, Review, "This copy of ForkMesh is a debuggable development build.")
        } else {
            finding(check, Pass, "This copy of ForkMesh is a release build.")
        }
    }

    fun installSource(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.installSource
        val installer = facts.installer?.trim() ?: return unavailable(check)
        val store = PhoneSecurityParsers.installerName(installer)
        val fix = PhoneSecurityFix("Open ForkMesh app info", "Open ForkMesh's page in Settings to see where it came from.", PhoneSecuritySettings.APP_DETAILS, ownAppDetails = true)
        return when {
            store != null -> finding(check, Pass, "Installed from $store.")
            installer.isEmpty() -> finding(check, Review, "No app store is recorded as installing ForkMesh; it was sideloaded or installed over USB.", fix = fix)
            installer in PhoneSecurityPackages.fileInstallers -> finding(check, Review, "ForkMesh was installed from a file ($installer).", fix = fix)
            else -> finding(check, Review, "ForkMesh was installed by $installer, which is not a known app store.", fix = fix)
        }
    }

    fun cleartext(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.cleartext
        val permitted = facts.cleartextPermitted ?: return unavailable(check)
        return if (permitted) {
            finding(check, Warning, "ForkMesh is allowed to send unencrypted traffic.")
        } else {
            finding(check, Pass, "ForkMesh only uses encrypted connections, apart from this phone's own loopback address.")
        }
    }

    fun relayHttps(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.relayHttps
        val url = facts.relayUrl?.trim()?.takeIf { it.isNotEmpty() } ?: return unavailable(check, "No relay address is set.")
        val host = PhoneSecurityParsers.relayHost(url).orEmpty()
        return when (PhoneSecurityParsers.relayVerdict(url)) {
            RelayVerdict.Secure -> finding(check, Pass, "The relay $host is reached over an encrypted connection.")
            RelayVerdict.LocalPlain -> finding(check, Review, "The relay $host is local and reached without encryption; fine for testing on this phone or network.")
            RelayVerdict.Plain -> finding(check, Critical, "The relay $host is reached without encryption.", "Change the relay in ForkMesh settings to an https or wss address.")
            RelayVerdict.Unknown -> unavailable(check, "The relay address could not be read.")
        }
    }

    fun hardwareKeystore(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.hardwareKeystore
        return when {
            facts.strongBox == true -> finding(check, Pass, "Keys can be kept in a StrongBox secure chip.")
            facts.hardwareKeystore == true -> finding(check, Pass, "Keys are kept in secure hardware.")
            facts.hardwareKeystore == false -> finding(check, Review, "This phone reports no secure hardware for keys; they are protected by software only.")
            else -> unavailable(check, "This Android version does not report its key storage hardware.")
        }
    }

    fun patchLevel(facts: PhoneSecurityFacts, today: LocalDate): PhoneSecurityFinding {
        val check = C.patchLevel
        val text = facts.securityPatch?.trim() ?: return unavailable(check)
        val age = PhoneSecurityParsers.patchAgeDays(text, today) ?: return unavailable(check, "The security patch level \"$text\" is not a date.")
        val status = PhoneSecurityParsers.patchStatus(age)
        val detail = if (age <= 0) "Security patch $text." else "Security patch $text, ${if (age == 1L) "1 day" else "$age days"} old."
        return finding(check, status, detail, fix = updateFix)
    }

    fun osVersion(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.osVersion
        val sdk = facts.sdkInt ?: return unavailable(check)
        return finding(check, PhoneSecurityParsers.osStatus(sdk), "${PhoneSecurityParsers.androidVersionName(sdk)}.", fix = updateFix)
    }

    fun encryption(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.encryption
        val code = facts.encryptionStatus ?: return unavailable(check)
        val status = PhoneSecurityParsers.encryptionStatus(code) ?: return unavailable(check, "Android reported an unknown encryption state ($code).")
        val detail = when (code) {
            PhoneSecurityParsers.ENCRYPTION_ACTIVE_PER_USER -> "Storage is encrypted per user."
            PhoneSecurityParsers.ENCRYPTION_ACTIVE -> "Storage is encrypted."
            PhoneSecurityParsers.ENCRYPTION_ACTIVE_DEFAULT_KEY -> "Storage is encrypted with a default key, not your screen lock."
            PhoneSecurityParsers.ENCRYPTION_ACTIVATING -> "Storage encryption is still being turned on."
            PhoneSecurityParsers.ENCRYPTION_INACTIVE -> "Storage is not encrypted."
            else -> "This phone does not support storage encryption."
        }
        return finding(check, status, detail, fix = securityFix)
    }

    fun developerOptions(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.developerOptions
        val on = facts.developerOptions ?: return unavailable(check)
        return if (on) {
            finding(check, Review, "Developer options are on.", PhoneSecurityUsage.DEVELOPER, developerFix)
        } else {
            finding(check, Pass, "Developer options are off.")
        }
    }

    fun usbDebugging(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.usbDebugging
        val on = facts.usbDebugging ?: return unavailable(check)
        return if (on) {
            finding(check, Warning, "USB debugging is on.", PhoneSecurityUsage.DEVELOPER, developerFix)
        } else {
            finding(check, Pass, "USB debugging is off.")
        }
    }

    fun wirelessDebugging(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.wirelessDebugging
        val on = facts.wirelessDebugging ?: return unavailable(check)
        return if (on) {
            finding(check, Warning, "Wireless debugging is on.", PhoneSecurityUsage.DEVELOPER, developerFix)
        } else {
            finding(check, Pass, "Wireless debugging is off.")
        }
    }

    fun autoTime(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.autoTime
        val on = facts.autoTime ?: return unavailable(check)
        return if (on) {
            finding(check, Pass, "The phone sets its clock from the network.")
        } else {
            finding(
                check, Review, "The date and time are set by hand.",
                fix = PhoneSecurityFix("Turn on automatic time", "Open Date & time in Settings and turn on Set time automatically.", PhoneSecuritySettings.DATE),
            )
        }
    }

    fun httpProxy(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.httpProxy
        val proxy = facts.httpProxy?.trim() ?: return unavailable(check)
        return if (proxy.isEmpty()) {
            finding(check, Pass, "No proxy is set.")
        } else {
            finding(
                check, Review, "Web traffic goes through the proxy $proxy.", "Proxy: $proxy. ${PhoneSecurityUsage.PROXY}",
                PhoneSecurityFix("Open Wi-Fi settings", "Open Wi-Fi in Settings, edit the network and set Proxy to None.", PhoneSecuritySettings.WIFI),
            )
        }
    }

    fun vpn(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.vpn
        val on = facts.vpnActive ?: return unavailable(check)
        return finding(check, Pass, if (on) "A VPN carries this phone's traffic." else "No VPN is on.")
    }

    fun userCertificates(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.userCertificates
        val certificates = facts.userCertificates ?: return unavailable(check)
        return if (certificates.isEmpty()) {
            finding(check, Pass, "No certificate authorities have been added.")
        } else {
            val count = certificates.size
            finding(
                check, Warning, "${if (count == 1) "1 certificate authority was" else "$count certificate authorities were"} added to this phone.",
                "Added: ${names(certificates)}. ${PhoneSecurityUsage.CERTIFICATES}",
                PhoneSecurityFix("Open security settings", "Open Security, then Encryption & credentials, then User credentials to remove them.", PhoneSecuritySettings.SECURITY),
            )
        }
    }

    fun privateDns(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.privateDns
        val fix = PhoneSecurityFix("Open network settings", "Open Network & internet in Settings and choose Private DNS.", PhoneSecuritySettings.WIRELESS)
        val server = facts.privateDnsServer?.trim()?.takeIf { it.isNotEmpty() }
        return when (facts.privateDnsMode?.trim()?.lowercase()) {
            "off" -> finding(check, Review, "Private DNS is off.", fix = fix)
            "opportunistic" -> finding(check, Pass, "Private DNS is automatic.")
            "hostname" -> finding(check, Pass, if (server != null) "Private DNS uses $server." else "Private DNS uses a named provider.")
            else -> when (facts.privateDnsActive) {
                true -> finding(check, Pass, if (server != null) "Private DNS uses $server." else "Private DNS is in use.")
                false -> finding(check, Review, "Private DNS is not in use on this network.", fix = fix)
                null -> unavailable(check, "Android does not let apps read the Private DNS setting on this phone.")
            }
        }
    }

    fun accessibilityServices(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.accessibilityServices
        val services = facts.accessibilityServices ?: return unavailable(check)
        return if (services.isEmpty()) {
            finding(check, Pass, "No accessibility services are on.")
        } else {
            finding(
                check, Review, "${plural(services.size, "accessibility service")} can read the screen.",
                "On: ${names(services)}. ${PhoneSecurityUsage.ACCESSIBILITY}",
                PhoneSecurityFix("Open accessibility settings", "Open Accessibility in Settings and turn off services you do not use.", PhoneSecuritySettings.ACCESSIBILITY),
            )
        }
    }

    fun notificationListeners(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.notificationListeners
        val listeners = facts.notificationListeners?.filter { it != facts.ownPackage } ?: return unavailable(check, "Android does not let apps see which apps read notifications on this phone.")
        return if (listeners.isEmpty()) {
            finding(check, Pass, "No other app reads your notifications.")
        } else {
            finding(
                check, Review, "${plural(listeners.size, "app")} can read every notification.",
                "Apps: ${names(listeners)}. ${PhoneSecurityUsage.NOTIFICATION_LISTENERS}",
                PhoneSecurityFix("Open notification access", "Open Notification access in Settings and turn off apps you do not use.", PhoneSecuritySettings.NOTIFICATION_LISTENERS),
            )
        }
    }

    fun deviceAdmins(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.deviceAdmins
        val admins = facts.deviceAdmins ?: return unavailable(check)
        return if (admins.isEmpty()) {
            finding(check, Pass, "No app is a device admin.")
        } else {
            finding(
                check, Review, "${plural(admins.size, "app")} ${if (admins.size == 1) "is a device admin" else "are device admins"}.",
                "Apps: ${names(admins)}. ${PhoneSecurityUsage.DEVICE_ADMINS}",
                PhoneSecurityFix("Open security settings", "Open Security, then Device admin apps, to turn off apps you do not use.", PhoneSecuritySettings.SECURITY),
            )
        }
    }

    fun keyboard(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.keyboard
        val id = facts.keyboard?.trim()?.takeIf { it.isNotEmpty() } ?: return unavailable(check)
        val pkg = PhoneSecurityParsers.packageOfComponent(id)
        val name = PhoneSecurityParsers.keyboardName(pkg)
        return when {
            PhoneSecurityParsers.isTrustedKeyboard(pkg) -> finding(check, Pass, "The keyboard is $name.")
            facts.keyboardIsSystem == true -> finding(check, Pass, "The keyboard came with the phone ($pkg).")
            else -> finding(
                check, Review, "The keyboard is $pkg, which did not come with the phone.",
                "Keyboard: $pkg. Keep it only if you trust its maker with everything you type.",
                PhoneSecurityFix("Open keyboard settings", "Open Keyboard settings and choose the keyboard that came with the phone.", PhoneSecuritySettings.INPUT_METHOD),
            )
        }
    }

    private fun permission(check: PhoneSecurityCheck, granted: Boolean?, what: String, usage: String): PhoneSecurityFinding {
        val allowed = granted ?: return unavailable(check, usage = usage)
        return if (allowed) {
            finding(check, Review, "ForkMesh may use the $what.", usage, appDetailsFix)
        } else {
            finding(check, Pass, "ForkMesh may not use the $what.", usage)
        }
    }

    fun camera(facts: PhoneSecurityFacts) = permission(C.camera, facts.cameraGranted, "camera", PhoneSecurityUsage.CAMERA)

    fun microphone(facts: PhoneSecurityFacts) = permission(C.microphone, facts.microphoneGranted, "microphone", PhoneSecurityUsage.MICROPHONE)

    fun nearby(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.nearby
        val granted = facts.nearbyGranted ?: return unavailable(
            check,
            if (facts.sdkInt != null && facts.sdkInt < 31) "Android before 12 has no Nearby devices permission." else unreadable,
            PhoneSecurityUsage.NEARBY,
        )
        return if (granted) {
            finding(check, Review, "ForkMesh may scan for and connect to nearby Bluetooth devices.", PhoneSecurityUsage.NEARBY, appDetailsFix)
        } else {
            finding(check, Pass, "ForkMesh may not scan for nearby devices.", PhoneSecurityUsage.NEARBY)
        }
    }

    fun notifications(facts: PhoneSecurityFacts): PhoneSecurityFinding {
        val check = C.notifications
        val granted = facts.notificationsGranted ?: return unavailable(check, usage = PhoneSecurityUsage.NOTIFICATIONS)
        return if (granted) {
            finding(check, Pass, "ForkMesh may show notifications.", PhoneSecurityUsage.NOTIFICATIONS)
        } else {
            finding(
                check, Review, "ForkMesh may not show notifications, so calls and pings cannot ring.", PhoneSecurityUsage.NOTIFICATIONS,
                PhoneSecurityFix("Open ForkMesh app info", "Open ForkMesh's page in Settings, then Notifications, to allow them.", PhoneSecuritySettings.APP_DETAILS, ownAppDetails = true),
            )
        }
    }

    private fun plural(count: Int, noun: String) = if (count == 1) "1 $noun" else "$count ${noun}s"

    val filters: List<PhoneSecurityFilter> = PhoneSecurityFilter.entries

    fun matchesFilter(finding: PhoneSecurityFinding, filter: PhoneSecurityFilter): Boolean = when (filter) {
        PhoneSecurityFilter.All -> true
        PhoneSecurityFilter.Attention -> finding.status == Critical || finding.status == Warning || finding.status == Review
        PhoneSecurityFilter.Critical -> finding.status == Critical
        PhoneSecurityFilter.Warning -> finding.status == Warning
        PhoneSecurityFilter.Review -> finding.status == Review
        PhoneSecurityFilter.Fixable -> finding.fix != null
        PhoneSecurityFilter.Pass -> finding.status == Pass
        PhoneSecurityFilter.Unavailable -> finding.status == Unavailable
    }

    fun matchesCategory(finding: PhoneSecurityFinding, category: String?): Boolean =
        category.isNullOrEmpty() || finding.check.category == category

    fun matchesQuery(finding: PhoneSecurityFinding, query: String): Boolean {
        val needle = query.trim()
        if (needle.isEmpty()) return true
        val haystack = listOf(finding.check.title, finding.check.category, finding.detail, finding.usage.orEmpty()).joinToString(" ")
        return haystack.contains(needle, ignoreCase = true)
    }

    fun visible(findings: List<PhoneSecurityFinding>, filter: PhoneSecurityFilter, category: String?, query: String): List<PhoneSecurityFinding> =
        findings.filter { matchesFilter(it, filter) && matchesCategory(it, category) && matchesQuery(it, query) }

    fun counts(findings: List<PhoneSecurityFinding>): Map<PhoneSecurityFilter, Int> =
        PhoneSecurityFilter.entries.associateWith { filter -> findings.count { matchesFilter(it, filter) } }

    fun stateSummary(findings: List<PhoneSecurityFinding>): String {
        fun count(status: PhoneSecurityStatus) = findings.count { it.status == status }
        return "${findings.size} checks: ${count(Pass)} pass, ${count(Review)} need review, ${count(Warning)} warnings, " +
            "${count(Critical)} critical, ${count(Unavailable)} unavailable"
    }

    fun filteredSummary(
        findings: List<PhoneSecurityFinding>,
        matched: Int,
        query: String,
        filter: PhoneSecurityFilter = PhoneSecurityFilter.All,
        category: String? = null,
    ): String {
        val needle = query.trim()
        val scope = buildList {
            if (filter != PhoneSecurityFilter.All) add(filter.displayName)
            if (!category.isNullOrEmpty()) add(category)
        }
        val scopeText = scope.joinToString(" · ")
        if (needle.isEmpty() && scope.isEmpty()) return stateSummary(findings)
        if (matched == 0) {
            return when {
                scope.isEmpty() -> "No checks match \"$needle\". Try a different search."
                needle.isEmpty() -> "No checks in $scopeText. Try a different filter."
                else -> "No checks in $scopeText match \"$needle\". Try a different filter or search."
            }
        }
        val description = when {
            scope.isEmpty() -> "matching \"$needle\""
            needle.isEmpty() -> "in $scopeText"
            else -> "in $scopeText matching \"$needle\""
        }
        return "Showing $matched of ${findings.size} checks $description. ${stateSummary(findings)}"
    }
}
