package com.forkmesh.nativeapp.account

fun isGuestMode(authAvailable: Boolean, isGuest: Boolean): Boolean = authAvailable && isGuest

suspend fun requireAccount(
    guest: Boolean,
    showSheet: suspend () -> AuthFlowStart?,
    openSignIn: suspend (AuthFlowStart) -> Unit,
    isAuthenticated: () -> Boolean,
): Boolean {
    if (!guest) return true
    val choice = showSheet()
    if (choice != null) openSignIn(choice)
    return isAuthenticated()
}

fun guestChatName(nodeKey: String): String {
    if (nodeKey.isEmpty()) return "Guest"
    val digest = com.forkmesh.nativeapp.crypto.Sha256.hash(nodeKey.toByteArray(Charsets.UTF_8))
    var value = 0L
    for (i in 0 until 4) value = (value shl 8) or (digest[i].toLong() and 0xff)
    return "Guest ${1000 + value % 9000}"
}
