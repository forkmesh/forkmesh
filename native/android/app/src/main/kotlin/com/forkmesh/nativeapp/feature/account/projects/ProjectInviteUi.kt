package com.forkmesh.nativeapp.feature.account.projects

import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import com.forkmesh.nativeapp.account.projects.Project
import com.forkmesh.nativeapp.account.projects.ProjectInvitation
import com.forkmesh.nativeapp.account.projects.ProjectInvite
import com.forkmesh.nativeapp.chat.DirectMessageException
import com.forkmesh.nativeapp.feature.account.AccountLabeledField
import com.forkmesh.nativeapp.feature.chat.ChatDepth
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.copyMessageText
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
fun InviteToProjectDialog(projects: ProjectsService, project: Project, done: (Unit?) -> Unit) {
    val c = Fm.colors
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val status = rememberFmStatus()
    val self = remember(project.id) {
        projects.sessionFor(project.id)?.nodeName?.trim()?.lowercase().orEmpty()
    }
    val invitation = remember(project.id, self) { ProjectInvitation(project.label, self, if (project.hasInstance) project.httpOrigin else "") }
    val host = ProjectInvite.host(invitation)
    val link = ProjectInvite.invitationLink(invitation)
    val direct = ChatDepth.direct
    val canInviteAccount = project.hasInstance && self.isNotEmpty() && direct != null
    var account by remember { mutableStateOf("") }
    var email by remember { mutableStateOf("") }
    var sending by remember { mutableStateOf(false) }

    fun openEmail() {
        val recipients = ProjectInvite.emailRecipients(email)
        if (recipients.invalid.isNotEmpty()) {
            status.error("“${recipients.invalid}” is not an email address.")
            return
        }
        val intent = Intent(Intent.ACTION_SENDTO, Uri.parse(ProjectInvite.mailtoUrl(recipients.addresses, invitation)))
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        try {
            context.startActivity(intent)
            status.show("Your email app has the invitation ready to send.")
        } catch (_: ActivityNotFoundException) {
            copyMessageText(context, ProjectInvite.emailSubject(invitation) + "\n\n" + ProjectInvite.emailBody(invitation))
            status.show("No email app opened, so the invitation was copied. Paste it into an email.")
        }
    }

    fun sendInvite() {
        val who = account.trim().removePrefix("@").lowercase()
        if (who.isEmpty()) {
            status.error("Enter their account name on $host.")
            return
        }
        if (ProjectInvite.looksLikeEmail(account)) {
            email = account.trim()
            account = ""
            status.show("That is an email address, so it moved to the email invitation. Tap Open email to send it.")
            return
        }
        if (who == self) {
            status.error("You are already in this project.")
            return
        }
        val service = direct ?: return
        sending = true
        status.show("Sending the invitation to @$who…")
        scope.launch {
            val error = withContext(Dispatchers.IO) {
                try {
                    val conversation = service.start(who).conversation
                    if (service.send(conversation, ProjectInvite.directMessageText(invitation))) null
                    else "Could not send the invitation to @$who."
                } catch (e: DirectMessageException) {
                    if (e.code == "user_not_found") "@$who is not on $host. Invite them by email below instead."
                    else e.message ?: "Could not send the invitation to @$who."
                } catch (e: Exception) {
                    "Could not send the invitation to @$who."
                }
            }
            sending = false
            if (error == null) {
                account = ""
                status.show("Invitation sent to @$who. It is in your direct messages.")
            } else {
                status.error(error)
            }
        }
    }

    FmAlertDialog(
        onDismissRequest = { done(null) },
        title = "Invite to ${project.label}",
        containerColor = c.bgOverlay,
        shape = RoundedCornerShape(FmRadius.lg),
        actions = {
            FmTextButton(onClick = { done(null) }) { Text("Done") }
        },
        content = {
            Text(
                "Invite someone who already has an account on $host, or email anyone else an invitation link.",
                style = Fm.text.bodySmall.copy(color = c.textSecondary),
            )
            Spacer(Modifier.height(FmSpace.x3))
            AccountLabeledField(
                label = "Someone on $host",
                value = account,
                onValueChange = { account = it },
                modifier = Modifier.testTag("project-invite-account"),
                hint = "Their ForkMesh account name",
            )
            if (!canInviteAccount) {
                Text(
                    when {
                        !project.hasInstance -> "This project has no instance yet, so invite people by email."
                        self.isEmpty() -> "Sign in to invite people on $host directly."
                        else -> "Messages are not connected yet."
                    },
                    style = Fm.text.bodySmall.copy(color = c.textTertiary),
                    modifier = Modifier.padding(bottom = FmSpace.x2),
                )
            }
            Button(
                onClick = { sendInvite() },
                enabled = canInviteAccount && !sending,
                modifier = Modifier.testTag("project-invite-send"),
            ) {
                Text(if (sending) "Sending…" else "Send invite")
            }
            Spacer(Modifier.height(FmSpace.x4))
            AccountLabeledField(
                label = "Anyone else, by email",
                value = email,
                onValueChange = { email = it },
                modifier = Modifier.testTag("project-invite-email"),
                hint = "name@example.com (optional)",
            )
            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                Button(onClick = { openEmail() }, modifier = Modifier.testTag("project-invite-open-email")) {
                    Text("Open email")
                }
                if (link.isNotEmpty()) {
                    FmTextButton(onClick = { copyMessageText(context, link, status) }) { Text("Copy link") }
                }
            }
            if (link.isNotEmpty()) {
                Text(link, style = Fm.text.bodySmall.copy(color = c.textTertiary), modifier = Modifier.padding(top = FmSpace.x2))
            }
            FmInlineStatus(status, Modifier.padding(top = FmSpace.x2))
        },
    )
}
