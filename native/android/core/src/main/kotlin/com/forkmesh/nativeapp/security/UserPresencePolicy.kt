package com.forkmesh.nativeapp.security

enum class UserPresenceMethod { Dialog, Prompt, DeviceCredential }

object UserPresencePolicy {
    private const val PROMPT_WITH_CREDENTIAL_SDK = 29

    fun method(deviceSecure: Boolean, sdk: Int): UserPresenceMethod = when {
        !deviceSecure -> UserPresenceMethod.Dialog
        sdk >= PROMPT_WITH_CREDENTIAL_SDK -> UserPresenceMethod.Prompt
        else -> UserPresenceMethod.DeviceCredential
    }
}
