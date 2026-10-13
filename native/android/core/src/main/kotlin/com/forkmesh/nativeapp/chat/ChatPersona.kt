package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.account.guestChatName

data class ChatPersona(
    val name: String,
    val accountKind: String,
    val ownerUser: String = "",
) {
    val isGuest: Boolean get() = accountKind == "guest"

    fun stamp(frame: MutableMap<String, Any?>) {
        frame["accountKind"] = accountKind
        if (ownerUser.isNotEmpty()) frame["ownerUser"] = ownerUser
    }

    companion object {
        fun resolve(nodeKey: String, account: String, displayName: String = ""): ChatPersona {
            val user = dartTrimText(account)
            if (user.isEmpty()) return ChatPersona(name = guestChatName(nodeKey), accountKind = "guest")
            val display = dartTrimText(displayName)
            return ChatPersona(name = display.ifEmpty { user }, accountKind = "user", ownerUser = user)
        }
    }
}
