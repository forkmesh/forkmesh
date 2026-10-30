package com.forkmesh.nativeapp.account

import com.forkmesh.nativeapp.api.PrefStore

data class PermissionReason(val id: String, val icon: String, val title: String, val reason: String)

object PermissionPacing {
    const val INTRO_KEY = "permissions/intro"
    const val NEARBY_KEY = "permissions/nearby-opened"
    const val PENDING = "pending"
    const val SEEN = "seen"

    const val TITLE = "We only ask when it matters"
    const val SUMMARY =
        "ForkMesh never asks for anything up front. Each permission comes up the first time a feature needs it, so you always know why."
    const val FOOTNOTE = "Say no to any of them and the rest of the app keeps working. You can change your mind in Android Settings."

    val reasons: List<PermissionReason> = listOf(
        PermissionReason(
            "notifications", "notifications_active", "Notifications",
            "After you send your first message, so you hear when someone replies.",
        ),
        PermissionReason(
            "nearby", "sensors", "Nearby devices and Wi-Fi",
            "When you open #mesh, to chat with people nearby without the internet.",
        ),
        PermissionReason(
            "camera", "videocam", "Camera and microphone",
            "When you start a call, record a message or dictate.",
        ),
        PermissionReason(
            "photos", "photo", "Photos",
            "When you attach or save a picture. Only the ones you pick.",
        ),
        PermissionReason(
            "health", "favorite", "Physical activity",
            "Only if you turn on Pedometer or Sleep.",
        ),
    )

    fun settle(prefs: PrefStore, freshInstall: Boolean) {
        if (prefs.getString(INTRO_KEY) != null) return
        prefs.putString(INTRO_KEY, if (freshInstall) PENDING else SEEN)
        if (!freshInstall) prefs.putBool(NEARBY_KEY, true)
    }

    fun needsIntro(prefs: PrefStore): Boolean = prefs.getString(INTRO_KEY) == PENDING

    fun finishIntro(prefs: PrefStore) = prefs.putString(INTRO_KEY, SEEN)

    fun nearbyAllowed(prefs: PrefStore): Boolean = prefs.getBool(NEARBY_KEY) ?: false

    fun markNearbyOpened(prefs: PrefStore) = prefs.putBool(NEARBY_KEY, true)
}
