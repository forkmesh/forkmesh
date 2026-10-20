package com.forkmesh.nativeapp.feature.tools.vault

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.fillMaxWidth
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
import com.forkmesh.nativeapp.feature.account.AccountOutlinedButton
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.vault.VaultException
import com.forkmesh.nativeapp.vault.VaultImportReview
import com.forkmesh.nativeapp.vault.VaultImportStart
import com.forkmesh.nativeapp.vault.VaultScreenLogic
import com.forkmesh.nativeapp.vault.VaultTransfer
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal object VaultTransferCopy {
    const val HEADER = "Move passwords"
    const val EXPLANATION =
        "Bring logins in from Apple Passwords, DuckDuckGo, or another password manager, or move them out again. Duplicates are skipped."
    const val IMPORT_BUTTON = "Import from a file"
    const val EXPORT_BUTTON = "Export to a file"
    const val FILE_HELP =
        "Apple Passwords on a Mac: File → Export All Passwords; on iPhone, Settings → Apps → Safari → Export writes a ZIP file you can import here. Chrome, Firefox, Bitwarden, 1Password and DuckDuckGo each export a CSV file from their password settings. To move logins out, export a file and import it in the other app."
    const val EXPORT_CONFIRM_TITLE = "Export every login unencrypted?"
    const val EXPORT_CONFIRM_MESSAGE =
        "The file holds your passwords in plain text. Save it only where the app you are moving to can import it, and delete it afterwards. Apple Passwords and DuckDuckGo can both import it."
}

private fun displayName(context: Context, uri: Uri): String =
    runCatching {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0) else null
        }
    }.getOrNull() ?: VaultTransfer.EXPORT_FILE_NAME

private fun readFile(context: Context, uri: Uri): ByteArray {
    val size = runCatching {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.SIZE), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst() && !cursor.isNull(0)) cursor.getLong(0) else null
        }
    }.getOrNull() ?: 0L
    if (size > VaultTransfer.MAX_FILE_BYTES) throw VaultException("csv_too_large")
    val stream = context.contentResolver.openInputStream(uri) ?: throw VaultException("csv_unreadable")
    return stream.use { input ->
        val out = java.io.ByteArrayOutputStream()
        val chunk = ByteArray(64 * 1024)
        while (true) {
            val read = input.read(chunk)
            if (read < 0) break
            out.write(chunk, 0, read)
            if (out.size() > VaultTransfer.MAX_FILE_BYTES) throw VaultException("csv_too_large")
        }
        out.toByteArray()
    }
}

@Composable
internal fun ColumnScope.VaultTransferSection(vault: VaultService) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var busy by remember { mutableStateOf(false) }
    var message by remember { mutableStateOf("") }
    var review by remember { mutableStateOf<VaultImportReview?>(null) }
    var confirmingExport by remember { mutableStateOf(false) }
    var exportText by remember { mutableStateOf<Pair<String, Int>?>(null) }

    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            val existing = vault.existingItems()
            val start = try {
                val bytes = withContext(Dispatchers.IO) { readFile(context, uri) }
                withContext(Dispatchers.Default) { VaultTransfer.prepareFile(bytes, existing) }
            } catch (error: VaultException) {
                VaultImportStart.Message(VaultTransfer.errorText(error))
            } catch (_: Exception) {
                VaultImportStart.Message(VaultTransfer.FILE_UNREADABLE)
            }
            when (start) {
                is VaultImportStart.Review -> review = start.review
                is VaultImportStart.Message -> message = start.text
            }
        }
    }
    val saver = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("text/csv")) { uri: Uri? ->
        val file = exportText
        exportText = null
        if (uri == null || file == null) return@rememberLauncherForActivityResult
        scope.launch {
            message = try {
                withContext(Dispatchers.IO) {
                    context.contentResolver.openOutputStream(uri, "wt")?.use { it.write(file.first.toByteArray(Charsets.UTF_8)) }
                        ?: throw IllegalStateException("no output stream")
                }
                VaultTransfer.savedText(file.second, displayName(context, uri))
            } catch (error: Exception) {
                "The file was not saved (${error.message ?: error.javaClass.simpleName})."
            }
        }
    }

    FmSectionHeader(VaultTransferCopy.HEADER)
    Text(VaultTransferCopy.EXPLANATION, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
    AccountOutlinedButton(
        VaultTransferCopy.IMPORT_BUTTON,
        if (busy) null else ({
            message = ""
            picker.launch(arrayOf("text/csv", "text/comma-separated-values", "text/plain", "application/zip", "application/octet-stream", "*/*"))
        }),
        modifier = Modifier.fillMaxWidth().testTag("vault-import-file"),
        icon = "upload_file",
    )
    AccountOutlinedButton(
        VaultTransferCopy.EXPORT_BUTTON,
        if (busy) null else ({
            message = ""
            confirmingExport = true
        }),
        modifier = Modifier.fillMaxWidth().testTag("vault-export-file"),
        icon = "download",
    )
    Text(VaultTransferCopy.FILE_HELP, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
    if (message.isNotEmpty()) {
        Text(message, Modifier.fillMaxWidth().testTag("vault-transfer-message"), style = Fm.text.bodyMedium)
    }

    if (confirmingExport) {
        FmConfirmDialog(
            title = VaultTransferCopy.EXPORT_CONFIRM_TITLE,
            body = VaultTransferCopy.EXPORT_CONFIRM_MESSAGE,
            confirmLabel = "Export",
            destructive = true,
            onConfirm = {
                confirmingExport = false
                try {
                    val logins = vault.exportLogins()
                    if (logins.isEmpty()) {
                        message = VaultTransfer.NOTHING_TO_EXPORT
                    } else {
                        exportText = VaultTransfer.writeCsv(logins) to logins.size
                        saver.launch(VaultTransfer.EXPORT_FILE_NAME)
                    }
                } catch (error: Exception) {
                    message = VaultScreenLogic.errorText(error)
                }
            },
            onDismissRequest = { confirmingExport = false },
        )
    }
    review?.let { pending ->
        FmConfirmDialog(
            title = pending.title,
            body = pending.message,
            confirmLabel = "Import",
            onConfirm = {
                review = null
                if (!pending.canImport) return@FmConfirmDialog
                busy = true
                scope.launch {
                    message = try {
                        VaultTransfer.reportText(vault.importLogins(pending.logins), pending.source)
                    } catch (error: Exception) {
                        VaultScreenLogic.errorText(error)
                    }
                    busy = false
                }
            },
            onDismissRequest = { review = null },
        )
    }
}
