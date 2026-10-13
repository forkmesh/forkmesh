package com.forkmesh.nativeapp.account.projects

data class ProjectInvitation(
    val projectName: String,
    val inviter: String,
    val origin: String,
)

object ProjectInvite {
    private val SEPARATORS = Regex("[,;\\s]+")
    private val ADDRESS = Regex("^[^@\\s<>\"]+@[^@\\s<>\"]+\\.[^@\\s<>\".]+$")

    fun host(invitation: ProjectInvitation): String {
        val origin = invitation.origin.trim().trimEnd('/')
        val host = origin.substringAfter("://", origin)
        return host.ifEmpty { ProjectDefaults.PROD_LABEL }
    }

    fun invitationLink(invitation: ProjectInvitation): String {
        val origin = invitation.origin.trim().trimEnd('/')
        if (!origin.startsWith("https://") && !origin.startsWith("http://")) return ""
        val inviter = invitation.inviter.trim()
        if (inviter.isEmpty()) return "$origin/signup"
        return "$origin/r/${percentEncode(inviter)}"
    }

    fun directMessageText(invitation: ProjectInvitation): String {
        val host = host(invitation)
        return "I’d like you to join ${projectLabel(invitation)} on $host. To open it in ForkMesh, " +
            "choose Add project and use $host as its instance."
    }

    fun emailSubject(invitation: ProjectInvitation): String {
        val inviter = invitation.inviter.trim()
        if (inviter.isEmpty()) return "You’re invited to ${projectLabel(invitation)} on ForkMesh"
        return "$inviter invited you to ${projectLabel(invitation)} on ForkMesh"
    }

    fun emailBody(invitation: ProjectInvitation): String {
        val inviter = invitation.inviter.trim()
        val host = host(invitation)
        val link = invitationLink(invitation)
        val body = StringBuilder("Hi,\n\n")
        body.append("I’d like you to join ${projectLabel(invitation)} on ForkMesh. It lives on the instance $host.\n\n")
        if (link.isNotEmpty()) body.append("Accept the invitation and create your account here:\n$link\n\n")
        body.append(
            "Then, in the ForkMesh app, choose Add project and use $host as its instance. " +
                "Reply with your account name and I’ll add you.\n",
        )
        if (inviter.isNotEmpty()) body.append("\n$inviter\n")
        return body.toString()
    }

    data class Recipients(val addresses: List<String>, val invalid: String)

    fun emailRecipients(input: String): Recipients {
        val out = ArrayList<String>()
        for (part in input.split(SEPARATORS)) {
            if (part.isEmpty()) continue
            if (!ADDRESS.matches(part)) return Recipients(emptyList(), part)
            if (out.none { it.equals(part, ignoreCase = true) }) out.add(part)
        }
        return Recipients(out, "")
    }

    fun mailtoUrl(recipients: List<String>, invitation: ProjectInvitation): String {
        val to = recipients.joinToString(",") { percentEncode(it.trim(), keep = "@+") }
        return "mailto:$to?subject=${mailtoField(emailSubject(invitation))}&body=${mailtoField(emailBody(invitation))}"
    }

    fun looksLikeEmail(text: String): Boolean = text.contains('@') && !text.trim().startsWith("@") &&
        emailRecipients(text).let { it.invalid.isEmpty() && it.addresses.isNotEmpty() }

    private fun projectLabel(invitation: ProjectInvitation): String {
        val name = invitation.projectName.trim()
        return if (name.isEmpty()) "my project" else "“$name”"
    }

    private fun mailtoField(value: String): String =
        percentEncode(value.replace("\r\n", "\n").replace("\n", "\r\n"))

    private fun percentEncode(value: String, keep: String = ""): String {
        val out = StringBuilder()
        for (byte in value.toByteArray(Charsets.UTF_8)) {
            val code = byte.toInt() and 0xff
            val char = code.toChar()
            val unreserved = char in 'A'..'Z' || char in 'a'..'z' || char in '0'..'9' || char in "-._~"
            if (code < 0x80 && (unreserved || char in keep)) {
                out.append(char)
            } else {
                out.append('%').append("%02X".format(code))
            }
        }
        return out.toString()
    }
}
