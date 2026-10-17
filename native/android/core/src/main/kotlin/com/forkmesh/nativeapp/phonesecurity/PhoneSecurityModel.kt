package com.forkmesh.nativeapp.phonesecurity

enum class PhoneSecurityStatus(val displayName: String, val rank: Int) {
    Pass("Pass", 4),
    Review("Review", 2),
    Warning("Warning", 1),
    Critical("Critical", 0),
    Unavailable("Unavailable", 3),
}

enum class PhoneSecurityFilter(val displayName: String) {
    All("All"),
    Attention("Needs attention"),
    Critical("Critical"),
    Warning("Warnings"),
    Review("For review"),
    Fixable("Can fix"),
    Pass("Passed"),
    Unavailable("Unavailable"),
}

object PhoneSecurityCategory {
    const val LOCK_SCREEN = "Lock screen"
    const val DEVICE_INTEGRITY = "Device integrity"
    const val SYSTEM = "System"
    const val APP_PROTECTION = "App protection"
    const val NETWORK = "Network"
    const val APPS_WITH_ACCESS = "Apps with access"
    const val PRIVACY = "Privacy"

    val all: List<String> = listOf(LOCK_SCREEN, DEVICE_INTEGRITY, SYSTEM, APP_PROTECTION, NETWORK, APPS_WITH_ACCESS, PRIVACY)
}

data class PhoneSecurityCheck(
    val id: String,
    val title: String,
    val category: String,
    val explanation: String,
)

data class PhoneSecurityFix(
    val label: String,
    val description: String,
    val action: String,
    val ownAppDetails: Boolean = false,
)

data class PhoneSecurityFinding(
    val check: PhoneSecurityCheck,
    val status: PhoneSecurityStatus,
    val detail: String,
    val usage: String? = null,
    val fix: PhoneSecurityFix? = null,
)

object PhoneSecuritySettings {
    const val MAIN = "android.settings.SETTINGS"
    const val SECURITY = "android.settings.SECURITY_SETTINGS"
    const val BIOMETRIC_ENROLL = "android.settings.BIOMETRIC_ENROLL"
    const val DEVELOPMENT = "android.settings.APPLICATION_DEVELOPMENT_SETTINGS"
    const val ACCESSIBILITY = "android.settings.ACCESSIBILITY_SETTINGS"
    const val NOTIFICATION_LISTENERS = "android.settings.ACTION_NOTIFICATION_LISTENER_SETTINGS"
    const val DATE = "android.settings.DATE_SETTINGS"
    const val DISPLAY = "android.settings.DISPLAY_SETTINGS"
    const val WIFI = "android.settings.WIFI_SETTINGS"
    const val WIRELESS = "android.settings.WIRELESS_SETTINGS"
    const val INPUT_METHOD = "android.settings.INPUT_METHOD_SETTINGS"
    const val APP_DETAILS = "android.settings.APPLICATION_DETAILS_SETTINGS"
    const val SYSTEM_UPDATE = "android.settings.SYSTEM_UPDATE_SETTINGS"
    const val NOTIFICATIONS = "android.settings.NOTIFICATION_SETTINGS"

    val all: List<String> = listOf(
        SECURITY, BIOMETRIC_ENROLL, DEVELOPMENT, ACCESSIBILITY, NOTIFICATION_LISTENERS, DATE, DISPLAY, WIFI, WIRELESS,
        INPUT_METHOD, APP_DETAILS, SYSTEM_UPDATE, NOTIFICATIONS,
    )
}
