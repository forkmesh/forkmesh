package com.forkmesh.nativeapp.feature.composer

import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.BugReportAttachment
import com.forkmesh.nativeapp.api.InboxService
import com.forkmesh.nativeapp.ci.ApiCiClient
import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.composer.QuickActionError
import com.forkmesh.nativeapp.composer.QuickPromptActions
import com.forkmesh.nativeapp.composer.QuickTaskPeople
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.tools.DiagnosticsRuntime
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.notes.NotesApi
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.tasks.TasksApi
import com.forkmesh.nativeapp.tasks.TasksLogic
import com.forkmesh.nativeapp.tools.ReportBugDraft
import com.forkmesh.nativeapp.tools.sendBugReport

internal object AppPromptActions : QuickPromptActions {
    private fun api(): ApiService = Services.get<ApiService>() ?: throw QuickActionError("ForkMesh is not connected in this build.")

    private val account: String get() = Services.get<AuthService>()?.session?.value?.nodeName.orEmpty().lowercase()

    override fun reportBug(text: String, photos: List<PromptAttachment>) {
        val result = sendBugReport(
            api(),
            ReportBugDraft(message = text),
            "composer",
            DiagnosticsRuntime.build,
            attachments = photos.map { BugReportAttachment(it.name, it.mediaType, it.bytes) },
        )
        if (!result.ok) throw QuickActionError(result.message)
    }

    override fun taskPeople(): QuickTaskPeople {
        val data = TasksApi(api()).call("GET")
        val actor = (data["actor"] as? String).orEmpty().lowercase()
        val members = if (data["canManage"] == true) (data["members"] as? List<*>).orEmpty().filterIsInstance<String>() else emptyList()
        return QuickTaskPeople(actor, members)
    }

    override fun createTask(title: String, details: String, assignee: String, repository: String) {
        val body = linkedMapOf<String, Any?>(
            "title" to title,
            "details" to details,
            "department" to "general",
            "destination" to "department",
            "assigneeKind" to if (assignee.isEmpty()) "unassigned" else "user",
            "priority" to 50,
        )
        if (assignee.isNotEmpty()) body["assignee"] = assignee
        if (repository.isNotEmpty()) body["repository"] = repository
        try {
            TasksApi(api()).call("POST", "", body)
        } catch (error: QuickActionError) {
            throw error
        } catch (error: Exception) {
            throw QuickActionError(TasksLogic.errorText(error, "The task was not created."))
        }
    }

    override fun createIssue(owner: String, name: String, title: String, body: String) {
        val inbox = Services.get<InboxService>() ?: throw QuickActionError("Sign in on this device to file issues.")
        try {
            inbox.submitNewIssue(owner, name, title, body)
        } catch (error: Exception) {
            throw QuickActionError("The issue was not filed: ${error.message ?: error}")
        }
    }

    override fun postToActivityPub(owner: String, name: String, title: String, body: String) {
        val identity = Services.get<DeviceIdentity>()
        val who = account
        if (identity == null || who.isEmpty()) throw QuickActionError("Sign in on this device to post to ActivityPub.")
        try {
            ApiCiClient(api()).announcePost(
                RepoRef(owner, name),
                title,
                body,
                who,
                { bytes -> identity.sign(bytes) },
                System.currentTimeMillis(),
            )
        } catch (error: QuickActionError) {
            throw error
        } catch (error: Exception) {
            throw QuickActionError("Could not post to ActivityPub: ${error.message ?: error}")
        }
    }

    override fun takeNote(title: String, markdown: String) {
        try {
            NotesApi(api()).create(title, markdown)
        } catch (error: QuickActionError) {
            throw error
        } catch (error: OrgApiException) {
            throw QuickActionError(if (error.message.isEmpty()) "The note was not saved." else "The note was not saved: ${error.message}")
        } catch (error: Exception) {
            throw QuickActionError("The note was not saved.")
        }
    }
}
