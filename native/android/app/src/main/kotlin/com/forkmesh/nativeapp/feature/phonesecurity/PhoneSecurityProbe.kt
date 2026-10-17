package com.forkmesh.nativeapp.feature.phonesecurity

import android.Manifest
import android.app.KeyguardManager
import android.app.NotificationManager
import android.app.admin.DevicePolicyManager
import android.content.ContentResolver
import android.content.Context
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.Uri
import android.os.Build
import android.os.Debug
import android.provider.Settings
import android.security.NetworkSecurityPolicy
import android.security.keystore.KeyInfo
import android.view.accessibility.AccessibilityManager
import android.view.inputmethod.InputMethodManager
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.SettingsService
import com.forkmesh.nativeapp.phonesecurity.PhoneBiometrics
import com.forkmesh.nativeapp.phonesecurity.PhoneBuild
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityFacts
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityPackages
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityParsers
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityPaths
import java.io.File
import java.security.KeyStore
import java.security.cert.X509Certificate
import javax.crypto.SecretKey
import javax.crypto.SecretKeyFactory

object PhoneSecurityProbe {
    private inline fun <T> attempt(block: () -> T): T? = try {
        block()
    } catch (_: Throwable) {
        null
    }

    fun read(context: Context): PhoneSecurityFacts {
        val resolver = context.contentResolver
        val pm = context.packageManager
        val dpm = attempt { context.getSystemService(DevicePolicyManager::class.java) }
        val cm = attempt { context.getSystemService(ConnectivityManager::class.java) }
        return PhoneSecurityFacts(
            ownPackage = context.packageName,
            deviceSecure = attempt { context.getSystemService(KeyguardManager::class.java)?.isDeviceSecure },
            biometrics = attempt { biometrics(context) },
            screenTimeoutMillis = attempt { Settings.System.getLong(resolver, Settings.System.SCREEN_OFF_TIMEOUT) },
            lockScreenNotifications = secureFlag(resolver, "lock_screen_show_notifications"),
            lockScreenPrivateContent = secureFlag(resolver, "lock_screen_allow_private_notifications"),
            stayAwakeWhileCharging = attempt { Settings.Global.getInt(resolver, Settings.Global.STAY_ON_WHILE_PLUGGED_IN, 0) != 0 },
            existingPaths = attempt { PhoneSecurityPaths.all.filter { File(it).exists() }.toSet() },
            installedPackages = attempt { PhoneSecurityPackages.all.filter { installed(pm, it) }.toSet() },
            build = attempt {
                PhoneBuild(
                    Build.FINGERPRINT.orEmpty(), Build.MODEL.orEmpty(), Build.MANUFACTURER.orEmpty(), Build.BRAND.orEmpty(),
                    Build.DEVICE.orEmpty(), Build.PRODUCT.orEmpty(), Build.HARDWARE.orEmpty(), Build.TAGS.orEmpty(), Build.TYPE.orEmpty(),
                )
            },
            procMounts = attempt { File("/proc/mounts").readText() },
            procSelfMaps = attempt { File("/proc/self/maps").readText() },
            xposedClassLoadable = attempt { xposedLoaded(context) },
            selinuxEnforce = attempt { File("/sys/fs/selinux/enforce").readText() },
            debuggerAttached = attempt { Debug.isDebuggerConnected() || Debug.waitingForDebugger() },
            appDebuggable = attempt { (context.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0 },
            installer = attempt { installer(context) },
            cleartextPermitted = attempt { NetworkSecurityPolicy.getInstance().isCleartextTrafficPermitted },
            relayUrl = attempt { Services.get<SettingsService>()?.serverUrl?.value },
            strongBox = attempt { if (Build.VERSION.SDK_INT >= 28) pm.hasSystemFeature(PackageManager.FEATURE_STRONGBOX_KEYSTORE) else null },
            hardwareKeystore = attempt {
                if (Build.VERSION.SDK_INT >= 31 && pm.hasSystemFeature(PackageManager.FEATURE_HARDWARE_KEYSTORE)) true else storedKeysInHardware() ?: if (Build.VERSION.SDK_INT >= 31) false else null
            },
            securityPatch = attempt { Build.VERSION.SECURITY_PATCH },
            sdkInt = Build.VERSION.SDK_INT,
            encryptionStatus = attempt { dpm?.storageEncryptionStatus },
            developerOptions = attempt { Settings.Global.getInt(resolver, Settings.Global.DEVELOPMENT_SETTINGS_ENABLED, 0) != 0 },
            usbDebugging = attempt { Settings.Global.getInt(resolver, Settings.Global.ADB_ENABLED, 0) != 0 },
            wirelessDebugging = attempt { if (Build.VERSION.SDK_INT >= 30) Settings.Global.getInt(resolver, "adb_wifi_enabled", 0) != 0 else false },
            autoTime = attempt { Settings.Global.getInt(resolver, Settings.Global.AUTO_TIME) != 0 },
            httpProxy = attempt { proxy(cm) },
            vpnActive = attempt { cm?.let { it.getNetworkCapabilities(it.activeNetwork)?.hasTransport(NetworkCapabilities.TRANSPORT_VPN) ?: false } },
            userCertificates = attempt { userCertificates() },
            privateDnsMode = attempt { Settings.Global.getString(resolver, "private_dns_mode") },
            privateDnsServer = attempt { privateDnsServer(resolver, cm) },
            privateDnsActive = attempt { if (Build.VERSION.SDK_INT >= 28) cm?.let { it.getLinkProperties(it.activeNetwork)?.isPrivateDnsActive } else null },
            accessibilityServices = attempt { accessibility(context, resolver) },
            notificationListeners = attempt {
                Settings.Secure.getString(resolver, "enabled_notification_listeners")?.let(PhoneSecurityParsers::accessibilityPackages) ?: emptyList()
            },
            deviceAdmins = attempt { dpm?.activeAdmins?.map { it.packageName }?.distinct() ?: emptyList() },
            keyboard = attempt { Settings.Secure.getString(resolver, Settings.Secure.DEFAULT_INPUT_METHOD) },
            keyboardIsSystem = attempt { keyboardIsSystem(context, resolver) },
            cameraGranted = attempt { granted(context, Manifest.permission.CAMERA) },
            microphoneGranted = attempt { granted(context, Manifest.permission.RECORD_AUDIO) },
            nearbyGranted = attempt {
                if (Build.VERSION.SDK_INT >= 31) {
                    granted(context, Manifest.permission.BLUETOOTH_SCAN) || granted(context, Manifest.permission.BLUETOOTH_CONNECT)
                } else {
                    null
                }
            },
            notificationsGranted = attempt { context.getSystemService(NotificationManager::class.java)?.areNotificationsEnabled() },
        )
    }

    private fun secureFlag(resolver: ContentResolver, name: String): Boolean? = attempt { Settings.Secure.getInt(resolver, name) != 0 }

    private fun granted(context: Context, permission: String) = context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED

    private fun biometrics(context: Context): PhoneBiometrics? {
        if (Build.VERSION.SDK_INT < 29) {
            return if (context.packageManager.hasSystemFeature(PackageManager.FEATURE_FINGERPRINT)) null else PhoneBiometrics.NoHardware
        }
        val manager = context.getSystemService(android.hardware.biometrics.BiometricManager::class.java) ?: return null
        val result = if (Build.VERSION.SDK_INT >= 30) {
            manager.canAuthenticate(android.hardware.biometrics.BiometricManager.Authenticators.BIOMETRIC_WEAK)
        } else {
            @Suppress("DEPRECATION")
            manager.canAuthenticate()
        }
        return when (result) {
            android.hardware.biometrics.BiometricManager.BIOMETRIC_SUCCESS -> PhoneBiometrics.Enrolled
            android.hardware.biometrics.BiometricManager.BIOMETRIC_ERROR_NONE_ENROLLED -> PhoneBiometrics.NoneEnrolled
            android.hardware.biometrics.BiometricManager.BIOMETRIC_ERROR_NO_HARDWARE -> PhoneBiometrics.NoHardware
            else -> null
        }
    }

    private fun installed(pm: PackageManager, name: String): Boolean = try {
        if (Build.VERSION.SDK_INT >= 33) {
            pm.getPackageInfo(name, PackageManager.PackageInfoFlags.of(0))
        } else {
            @Suppress("DEPRECATION")
            pm.getPackageInfo(name, 0)
        }
        true
    } catch (_: PackageManager.NameNotFoundException) {
        false
    }

    private fun xposedLoaded(context: Context): Boolean {
        val loaders = listOfNotNull(ClassLoader.getSystemClassLoader(), context.classLoader)
        val loadable = PhoneSecurityPackages.xposedClasses.any { name ->
            loaders.any { loader -> attempt { Class.forName(name, false, loader) } != null }
        }
        if (loadable) return true
        return Throwable().stackTrace.any { frame ->
            frame.className.startsWith("de.robv.android.xposed") || frame.className.startsWith("com.saurik.substrate") ||
                frame.className.startsWith("org.lsposed")
        }
    }

    private fun installer(context: Context): String {
        val pm = context.packageManager
        return if (Build.VERSION.SDK_INT >= 30) {
            pm.getInstallSourceInfo(context.packageName).installingPackageName.orEmpty()
        } else {
            @Suppress("DEPRECATION")
            pm.getInstallerPackageName(context.packageName).orEmpty()
        }
    }

    private fun proxy(cm: ConnectivityManager?): String {
        val info = cm?.defaultProxy ?: cm?.let { it.getLinkProperties(it.activeNetwork)?.httpProxy }
        if (info != null) {
            val pac = info.pacFileUrl
            if (pac != null && pac != Uri.EMPTY && pac.toString().isNotEmpty()) return "PAC $pac"
            if (!info.host.isNullOrEmpty()) return "${info.host}:${info.port}"
        }
        val host = System.getProperty("http.proxyHost").orEmpty()
        if (host.isNotEmpty()) return listOf(host, System.getProperty("http.proxyPort").orEmpty()).filter { it.isNotEmpty() }.joinToString(":")
        return ""
    }

    private fun privateDnsServer(resolver: ContentResolver, cm: ConnectivityManager?): String? {
        val named = attempt { Settings.Global.getString(resolver, "private_dns_specifier") }?.takeIf { it.isNotBlank() }
        if (named != null) return named
        if (Build.VERSION.SDK_INT < 28) return null
        return cm?.let { it.getLinkProperties(it.activeNetwork)?.privateDnsServerName }
    }

    private fun userCertificates(): List<String> {
        val store = KeyStore.getInstance("AndroidCAStore")
        store.load(null)
        return store.aliases().toList()
            .filter { it.startsWith("user:") }
            .map { alias ->
                val certificate = store.getCertificate(alias) as? X509Certificate
                certificate?.subjectX500Principal?.name?.let(PhoneSecurityParsers::commonName) ?: alias
            }
            .sorted()
    }

    private fun storedKeysInHardware(): Boolean? {
        val store = KeyStore.getInstance("AndroidKeyStore")
        store.load(null)
        for (alias in store.aliases().toList()) {
            val key = attempt { store.getKey(alias, null) } as? SecretKey ?: continue
            val info = attempt {
                SecretKeyFactory.getInstance(key.algorithm, "AndroidKeyStore").getKeySpec(key, KeyInfo::class.java) as KeyInfo
            } ?: continue
            return if (Build.VERSION.SDK_INT >= 31) {
                info.securityLevel == android.security.keystore.KeyProperties.SECURITY_LEVEL_TRUSTED_ENVIRONMENT ||
                    info.securityLevel == android.security.keystore.KeyProperties.SECURITY_LEVEL_STRONGBOX
            } else {
                @Suppress("DEPRECATION")
                info.isInsideSecureHardware
            }
        }
        return null
    }

    private fun accessibility(context: Context, resolver: ContentResolver): List<String> {
        val enabled = attempt { Settings.Secure.getString(resolver, Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES) }
        if (enabled != null) return PhoneSecurityParsers.accessibilityPackages(enabled)
        val manager = context.getSystemService(AccessibilityManager::class.java) ?: return emptyList()
        return manager.getEnabledAccessibilityServiceList(android.accessibilityservice.AccessibilityServiceInfo.FEEDBACK_ALL_MASK)
            .mapNotNull { it.resolveInfo?.serviceInfo?.packageName }
            .distinct()
    }

    private fun keyboardIsSystem(context: Context, resolver: ContentResolver): Boolean? {
        val id = Settings.Secure.getString(resolver, Settings.Secure.DEFAULT_INPUT_METHOD) ?: return null
        val manager = context.getSystemService(InputMethodManager::class.java) ?: return null
        val info = manager.enabledInputMethodList.firstOrNull { it.id == id } ?: return null
        val flags = info.serviceInfo?.applicationInfo?.flags ?: return null
        return (flags and (ApplicationInfo.FLAG_SYSTEM or ApplicationInfo.FLAG_UPDATED_SYSTEM_APP)) != 0
    }
}
