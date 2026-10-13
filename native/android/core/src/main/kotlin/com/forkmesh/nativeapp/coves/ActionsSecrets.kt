package com.forkmesh.nativeapp.coves

import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.json.jsonLong

data class ActionsSecretRow(val name: String, val updatedAtMs: Long = 0)

data class ActionsSecretConfirmation(val token: String, val action: String, val name: String, val summary: String)

class ActionsSecretException(message: String) : Exception(message) {
    companion object {
        val invalidName get() = ActionsSecretException("Use letters, digits and underscores, starting with a letter or underscore (at most 128).")
        val valueTooLarge get() = ActionsSecretException("Values are limited to 16 KB.")
        fun desktop(code: String) = ActionsSecretException(ActionsSecrets.message(code))
    }
}

object ActionsSecrets {
    const val FAMILY = "secrets."
    const val MAX_NAME_LENGTH = 128
    const val MAX_VALUE_BYTES = 16 * 1024
    const val MAX_ROWS = 500

    fun validName(name: String): Boolean {
        if (name.isEmpty() || name.length > MAX_NAME_LENGTH) return false
        name.forEachIndexed { index, c ->
            val letter = c in 'A'..'Z' || c in 'a'..'z' || c == '_'
            val digit = c in '0'..'9'
            if (!(letter || (digit && index > 0))) return false
        }
        return true
    }

    fun message(code: String): String = when (code) {
        "unsupported" -> "Your desktop needs an update to manage Actions secrets from the phone."
        "not_owner" -> "Only this desktop's owner can manage its Actions secrets."
        "confirmation_expired" -> "The confirmation expired. Try again."
        "confirmation_mismatch", "confirmation_missing" -> "The confirmation did not match. Try again."
        "invalid_name" -> "That name is not a valid variable name."
        "value_too_large" -> "Values are limited to 16 KB."
        "not_found" -> "That variable no longer exists on the desktop."
        else -> "The desktop refused: $code."
    }

    fun rows(answer: Map<String, Any?>): List<ActionsSecretRow> {
        val raw = answer["variables"] as? List<*> ?: emptyList<Any?>()
        val seen = HashSet<String>()
        val out = ArrayList<ActionsSecretRow>()
        for (value in raw.take(MAX_ROWS)) {
            val obj = value as? Map<*, *> ?: continue
            val name = obj["name"] as? String ?: continue
            if (!validName(name) || !seen.add(name)) continue
            out.add(ActionsSecretRow(name, jsonLong(obj["updatedAtMs"])))
        }
        return out.sortedBy { it.name.lowercase() }
    }
}

class ActionsSecretsClient(private val channel: ControlCalling) {
    private fun call(method: String, params: Map<String, Any?> = emptyMap()): Map<String, Any?> {
        val answer = channel.request(ActionsSecrets.FAMILY + method, params, 20_000)
        val error = answer["error"] as? String
        if (!error.isNullOrEmpty()) throw ActionsSecretException.desktop(error)
        return answer
    }

    fun list(): List<ActionsSecretRow> = ActionsSecrets.rows(call("list"))

    fun prepare(action: String, name: String): ActionsSecretConfirmation {
        if (!ActionsSecrets.validName(name)) throw ActionsSecretException.invalidName
        val answer = call("prepare", linkedMapOf("action" to action, "name" to name))
        val token = (answer["token"] as? String).orEmpty()
        if (token.isEmpty()) throw ActionsSecretException.desktop("confirmation_missing")
        return ActionsSecretConfirmation(token, action, name, (answer["summary"] as? String).orEmpty())
    }

    fun set(name: String, value: String, confirmation: ActionsSecretConfirmation) {
        if (!ActionsSecrets.validName(name)) throw ActionsSecretException.invalidName
        if (value.toByteArray(Charsets.UTF_8).size > ActionsSecrets.MAX_VALUE_BYTES) throw ActionsSecretException.valueTooLarge
        call("set", linkedMapOf("name" to name, "value" to value, "token" to confirmation.token))
    }

    fun delete(name: String, confirmation: ActionsSecretConfirmation) {
        if (!ActionsSecrets.validName(name)) throw ActionsSecretException.invalidName
        call("delete", linkedMapOf("name" to name, "token" to confirmation.token))
    }
}

class ActionsSecretsModel(private val client: ActionsSecretsClient) {
    @Volatile var rows: List<ActionsSecretRow> = emptyList()
        private set
    @Volatile var loading = false
        private set
    @Volatile var error: String? = null
        private set
    @Volatile var busy = false
        private set

    fun load() {
        loading = true
        try {
            rows = client.list()
            error = null
        } catch (failure: Exception) {
            error = failure.message ?: failure.toString()
        } finally {
            loading = false
        }
    }

    fun save(name: String, value: String): Boolean {
        val trimmed = name.trim()
        busy = true
        return try {
            if (value.toByteArray(Charsets.UTF_8).size > ActionsSecrets.MAX_VALUE_BYTES) throw ActionsSecretException.valueTooLarge
            val confirmation = client.prepare("set", trimmed)
            client.set(trimmed, value, confirmation)
            error = null
            load()
            true
        } catch (failure: Exception) {
            error = failure.message ?: failure.toString()
            false
        } finally {
            busy = false
        }
    }

    fun delete(name: String): Boolean {
        busy = true
        return try {
            val confirmation = client.prepare("delete", name)
            client.delete(name, confirmation)
            error = null
            load()
            true
        } catch (failure: Exception) {
            error = failure.message ?: failure.toString()
            false
        } finally {
            busy = false
        }
    }
}
