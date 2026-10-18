package com.forkmesh.nativeapp.feature.workflows

import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.security.SecretStore
import com.forkmesh.nativeapp.workflows.HttpWorkflowTransport
import com.forkmesh.nativeapp.workflows.WorkflowCatalog
import com.forkmesh.nativeapp.workflows.WorkflowCredential
import com.forkmesh.nativeapp.workflows.WorkflowDraft
import com.forkmesh.nativeapp.workflows.WorkflowEngine
import com.forkmesh.nativeapp.workflows.WorkflowPhoneCredentials
import com.forkmesh.nativeapp.workflows.WorkflowPhoneRunner
import com.forkmesh.nativeapp.workflows.WorkflowRun
import com.forkmesh.nativeapp.workflows.WorkflowTargets
import com.forkmesh.nativeapp.workflows.WorkflowsApi

object WorkflowsFeature {
    private const val OWNER = "existing"

    fun install() {
        NavigationFeature.register(Route.Kind.Workflows, OWNER) { WorkflowsScreen() }
    }
}

class WorkflowsSession(val api: WorkflowsApi, secrets: SecretStore) {
    val phone = WorkflowPhoneCredentials(secrets)
    val engine: WorkflowEngine? by lazy { runCatching { WorkflowEngine.bundled() }.getOrNull() }
    val catalog = mutableStateOf(WorkflowCatalog.EMPTY)
    val credentials = mutableStateOf<List<WorkflowCredential>>(emptyList())
    val phoneRuns = mutableStateMapOf<String, List<WorkflowRun>>()
    val phoneIds = mutableStateOf<Set<String>>(emptySet())

    fun loadCatalog() {
        if (catalog.value.core.isNotEmpty()) return
        catalog.value = engine?.let { WorkflowCatalog(it.catalog()) } ?: api.catalog()
    }

    fun loadCredentials() {
        credentials.value = api.credentials()
        phoneIds.value = credentials.value.map { it.id }.filter { phone.has(it) }.toSet()
    }

    fun check(draft: WorkflowDraft): String {
        val error = engine?.clean(draft.graph)?.error ?: return ""
        return if (error.isEmpty()) "" else com.forkmesh.nativeapp.workflows.WorkflowsText.error(error)
    }

    fun runOnPhone(draft: WorkflowDraft): WorkflowRun? {
        val engine = engine ?: return null
        val targets = WorkflowTargets(api, phone).resolve(draft)
        val run = WorkflowPhoneRunner(engine, HttpWorkflowTransport(engine)).run(draft, targets.credentials, workflows = targets.workflows)
        val key = draft.id.ifEmpty { "draft" }
        phoneRuns[key] = (listOf(run) + (phoneRuns[key] ?: emptyList())).take(30)
        return run
    }

    fun missingOnPhone(draft: WorkflowDraft): List<String> = draft.nodes.filter { node ->
        val kind = catalog.value.kind(node.type)
        !node.disabled && kind != null && kind.needsCredential && (node.credential.isEmpty() || !phone.has(node.credential))
    }.map { it.name }

    fun savePhone(credential: WorkflowCredential, values: Map<String, String>) {
        phone.save(credential.id, credential.integration, values)
        phoneIds.value = phoneIds.value + credential.id
    }

    fun removePhone(id: String) {
        phone.remove(id)
        phoneIds.value = phoneIds.value - id
    }

    companion object {
        @Volatile
        private var current: Pair<ApiService, WorkflowsSession>? = null

        fun get(): WorkflowsSession? {
            val api = Services.get<ApiService>() ?: return null
            current?.let { (owner, session) -> if (owner === api) return session }
            val session = WorkflowsSession(WorkflowsApi(api), Services.get<SecretStore>() ?: MemorySecretStore())
            current = api to session
            return session
        }
    }
}
