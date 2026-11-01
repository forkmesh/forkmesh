package com.forkmesh.nativeapp.ci

import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.modelInt

data class ActionSecret(val name: String, val updatedAtMs: Long = 0, val updatedBy: String = "") {
    companion object {
        fun fromJson(json: Map<String, Any?>): ActionSecret? {
            val name = json.str("name").trim()
            if (!ActionSecrets.NAME.matches(name)) return null
            return ActionSecret(name, maxOf(0L, modelInt(json["updatedAt"])), json.str("updatedBy").trim().take(64))
        }
    }
}

object ActionSecrets {
    val NAME = Regex("^[A-Z_][A-Z0-9_]{0,63}$")
    const val VALUE_MAX_BYTES = 48 * 1024
    const val FORBIDDEN = "Only the repository owner and organization admins can manage secrets."

    fun cleanName(raw: String): String = raw.trim().uppercase()

    fun nameProblem(name: String): String? = when {
        name.isEmpty() -> "Give the secret a name."
        !NAME.matches(name) -> CiError(400, "invalid_name").message
        else -> null
    }

    fun valueProblem(value: String): String? {
        val bytes = value.toByteArray(Charsets.UTF_8).size
        return when {
            bytes == 0 -> "Enter the secret's value."
            bytes > VALUE_MAX_BYTES -> CiError(400, "invalid_value").message
            else -> null
        }
    }

    fun explain(failure: Exception): String = when {
        failure is CiError && (failure.code == "forbidden" || failure.status == 403) -> FORBIDDEN
        else -> failure.message ?: "$failure"
    }
}

class ActionSecretsModel(private val client: CiClient, val repo: RepoRef) {
    @Volatile var secrets: List<ActionSecret> = emptyList()
        private set
    @Volatile var loaded: Boolean = false
        private set
    @Volatile var error: String? = null
        private set
    @Volatile var notice: CiNotice? = null
        private set
    @Volatile var busy: Boolean = false
        private set

    fun exists(name: String): Boolean = secrets.any { it.name == ActionSecrets.cleanName(name) }

    fun load() {
        try {
            secrets = client.actionSecrets(repo).sortedBy { it.name }
            error = null
        } catch (failure: Exception) {
            error = ActionSecrets.explain(failure)
        } finally {
            loaded = true
        }
    }

    fun save(rawName: String, value: String): Boolean {
        val name = ActionSecrets.cleanName(rawName)
        val problem = ActionSecrets.nameProblem(name) ?: ActionSecrets.valueProblem(value)
        if (problem != null) {
            notice = CiNotice(problem, false)
            return false
        }
        val replacing = exists(name)
        return act {
            client.putActionSecret(repo, name, value)
            if (replacing) "Replaced $name." else "Added $name."
        }
    }

    fun delete(name: String): Boolean = act {
        client.deleteActionSecret(repo, name)
        "Deleted $name."
    }

    private fun act(block: () -> String): Boolean {
        busy = true
        notice = null
        val ok = try {
            notice = CiNotice(block(), true)
            true
        } catch (failure: Exception) {
            notice = CiNotice(ActionSecrets.explain(failure), false)
            false
        } finally {
            busy = false
        }
        if (ok) load()
        return ok
    }
}
