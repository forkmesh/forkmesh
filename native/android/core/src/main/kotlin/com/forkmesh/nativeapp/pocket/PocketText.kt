package com.forkmesh.nativeapp.pocket

object PocketText {
    const val UNLOCK_SHARE = 0.85
    const val HUB_HINT = "Locks the screen to Glasses Alert, pings and walking. Slide to unlock."
    const val SETTINGS_NOTE = "A lock screen with Glasses Alert, your pings and the walking figure shown big. Slide to unlock. Your avatar's menu at the top opens it too; it shows a walking figure there while you walk."

    fun hubTitle(walking: Boolean): String = if (walking) "Pocket mode, walking" else "Pocket mode"

    fun hubIcon(walking: Boolean): String = if (walking) "directions_walk" else "phonelink_lock"

    fun glassesTitle(nearby: Int): String = when (nearby) {
        0 -> "No smart glasses"
        1 -> "Smart glasses nearby"
        else -> "$nearby smart glasses nearby"
    }

    fun devicesHeard(heard: Int): String = if (heard == 1) "1 device heard" else "$heard devices heard"

    fun alarmed(ringing: Boolean, nearby: Int): Boolean = ringing || nearby > 0

    fun pingsTitle(unread: Int): String = when (unread) {
        0 -> "No new pings"
        1 -> "1 new ping"
        else -> "$unread new pings"
    }

    fun walkingTitle(moving: Boolean, stepping: Boolean, gaitText: String, detecting: Boolean): String = when {
        moving -> if (stepping) gaitText else "Walking"
        detecting -> "Not walking"
        else -> "Walk detection is off"
    }

    fun cadence(stepsPerMinute: Int): String = "$stepsPerMinute steps/min"

    fun unlocks(drag: Double, travel: Double): Boolean = travel > 0 && drag >= travel * UNLOCK_SHARE

    fun hintAlpha(drag: Double, travel: Double): Double = if (travel <= 0) 0.7 else 0.7 * (1 - (drag / travel).coerceIn(0.0, 1.0))
}
