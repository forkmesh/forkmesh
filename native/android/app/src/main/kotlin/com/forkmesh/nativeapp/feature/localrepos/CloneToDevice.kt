package com.forkmesh.nativeapp.feature.localrepos

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.localrepos.LocalPullResult
import com.forkmesh.nativeapp.localrepos.LocalRepoRecord
import com.forkmesh.nativeapp.localrepos.LocalRepoStore
import com.forkmesh.nativeapp.localrepos.PublishService
import com.forkmesh.nativeapp.localrepos.cloneToDevice
import com.forkmesh.nativeapp.localrepos.cloneToDeviceNote
import com.forkmesh.nativeapp.localrepos.describeLocalRepoFailure
import com.forkmesh.nativeapp.localrepos.suggestCloneName
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmDialogActions
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlin.coroutines.cancellation.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

internal suspend fun showCloneToDevice(
    store: LocalRepoStore,
    messenger: FmMessenger,
    requireAccount: suspend (String) -> Boolean,
    owner: String,
    name: String,
    description: String = "",
    status: FmStatus? = null,
): LocalRepoRecord? {
    val publisher = Services.get<PublishService>() ?: return null
    if (!requireAccount("clone to this device")) return null
    val session = LocalReposFeature.session()
    withContext(Dispatchers.IO) { store.ensureLoaded() }
    store.record("$owner/$name")?.let { return it }
    val local = messenger.showDialog<String> { done ->
        RepoNameDialog(
            title = "Clone to this device",
            initial = suggestCloneName(store.records, name),
            hint = "Folder name on this device",
            confirmLabel = "Clone",
            note = cloneToDeviceNote,
            done = done,
        )
    } ?: return null
    val label = busyLabel("Cloning $owner/$name")
    val outcome = messenger.runBusy(label, onCancel = store::cancelActive) {
        withContext(Dispatchers.IO) {
            cloneToDevice(
                store = store,
                publisher = publisher,
                session = session,
                owner = owner,
                name = name,
                localName = local,
                description = description,
                onStatus = { text -> label.value = text },
            )
        }
    }
    if (outcome.message.isNotEmpty()) status?.report(outcome.message, isError = outcome.record == null && outcome.existing == null)
    return outcome.record
}

internal suspend fun syncFromForkMesh(
    store: LocalRepoStore,
    messenger: FmMessenger,
    requireAccount: suspend (String) -> Boolean,
    record: LocalRepoRecord,
    onOpen: () -> Unit,
    status: FmStatus? = null,
): LocalPullResult? {
    val publisher = Services.get<PublishService>() ?: return null
    if (!requireAccount("sync")) return null
    val label = busyLabel("Syncing ${record.publishedAs ?: record.name}")
    var cancelled = false
    var failure: String? = null
    val result = messenger.runBusy(label, onCancel = {
        cancelled = true
        store.cancelActive()
    }) {
        try {
            withContext(Dispatchers.IO) { store.sync(record, publisher) { progress -> label.value = progress.label } }
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            failure = describeLocalRepoFailure(error)
            null
        }
    }
    if (cancelled) return null
    when {
        result != null -> status?.show(result.message, "Open", onOpen)
        failure != null -> status?.error(failure.orEmpty(), "Open", onOpen)
    }
    return result
}

@Composable
internal fun RepoNameDialog(
    title: String,
    initial: String,
    hint: String,
    confirmLabel: String,
    done: (String?) -> Unit,
    note: String? = null,
) {
    var text by remember { mutableStateOf(initial) }
    var problem by remember { mutableStateOf<String?>(null) }
    FmAlertDialog(
        onDismissRequest = { done(null) },
        title = title,
        actions = {
            FmDialogActions(
                onCancel = { done(null) },
                onConfirm = {
                    val trimmed = text.trim()
                    val error = LocalRepoStore.validateName(trimmed)
                    if (error != null) problem = error else done(trimmed)
                },
                confirmLabel = confirmLabel,
            )
        },
    ) {
        Column {
            FmTextField(
                value = text,
                onValueChange = {
                    text = it
                    problem = null
                },
                hint = hint,
                errorText = problem,
                autofocus = true,
            )
            if (note != null) {
                Spacer(Modifier.height(FmSpace.x3))
                Text(note, style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
            }
        }
    }
}
