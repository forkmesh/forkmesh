package com.forkmesh.nativeapp.feature.code

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.code.MirrorImport
import com.forkmesh.nativeapp.code.MirrorImportOutcome
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmDialogActions
import com.forkmesh.nativeapp.ui.kit.FmField
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal suspend fun FmMessenger.showMirrorImportSheet(): String? =
    showDialog<String> { done -> MirrorImportSheet(onClose = done) }

@Composable
internal fun MirrorImportSheet(onClose: (String?) -> Unit) {
    val c = Fm.colors
    val scope = rememberCoroutineScope()
    var url by remember { mutableStateOf("") }
    var token by remember { mutableStateOf("") }
    var problem by remember { mutableStateOf<String?>(null) }
    var status by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }

    fun submit() {
        val invalid = MirrorImport.validateSourceUrl(url)
        if (invalid != null) {
            problem = invalid
            return
        }
        val account = Services.get<AuthService>()?.session?.value?.nodeName
        if (account.isNullOrEmpty()) {
            status = MirrorImport.SIGN_IN
            return
        }
        busy = true
        status = MirrorImport.READING
        scope.launch {
            val outcome = withContext(Dispatchers.IO) { MirrorImport.submit(repoApi, url, token, account) }
            busy = false
            when (outcome) {
                is MirrorImportOutcome.Done -> onClose(outcome.notice)
                is MirrorImportOutcome.Status -> status = outcome.message
                is MirrorImportOutcome.Problem -> problem = outcome.message
            }
        }
    }

    FmAlertDialog(
        onDismissRequest = { onClose(null) },
        title = MirrorImport.TITLE,
        containerColor = c.bgOverlay,
        shape = RoundedCornerShape(FmRadius.lg),
        actionsPadding = PaddingValues(start = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x3),
        actions = {
            FmDialogActions(
                onCancel = { onClose(null) },
                onConfirm = { submit() },
                confirmLabel = MirrorImport.CONFIRM,
                busy = busy,
            )
        },
        content = {
            Column(
                Modifier
                    .width(480.dp)
                    .verticalScroll(rememberScrollState()),
            ) {
                FmField(MirrorImport.URL_LABEL) {
                    FmTextField(
                        value = url,
                        onValueChange = {
                            url = it
                            if (problem != null) problem = null
                        },
                        hint = MirrorImport.URL_HINT,
                        errorText = problem,
                        autofocus = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri, autoCorrectEnabled = false),
                    )
                }
                Spacer(Modifier.height(FmSpace.x3))
                FmField(MirrorImport.TOKEN_LABEL) {
                    FmTextField(
                        value = token,
                        onValueChange = { token = it },
                        helperText = MirrorImport.TOKEN_HELPER,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password, autoCorrectEnabled = false),
                        visualTransformation = PasswordVisualTransformation(),
                    )
                }
                Spacer(Modifier.height(FmSpace.x3))
                Text(
                    MirrorImport.DISCLAIMER,
                    style = Fm.text.bodySmall.copy(color = c.textTertiary, lineHeight = FmType.meta * 1.4f),
                )
                if (status.isNotEmpty()) {
                    Spacer(Modifier.height(FmSpace.x3))
                    Text(
                        status,
                        style = Fm.text.bodySmall.copy(
                            color = if (busy) c.textSecondary else c.danger,
                            lineHeight = FmType.meta * 1.4f,
                        ),
                    )
                }
            }
        },
    )
}
