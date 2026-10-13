package com.forkmesh.nativeapp.feature.agents

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.FmFilledButton
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.runners.RunnerAttention
import com.forkmesh.nativeapp.runners.RunnerAttentionApi
import com.forkmesh.nativeapp.runners.RunnerLoginSession
import com.forkmesh.nativeapp.runners.RunnerLoginText
import com.forkmesh.nativeapp.runners.RunnerReply
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import com.forkmesh.nativeapp.ui.navigation.Screen
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal class RunnerSignInScreen(
    runner: String,
    provider: String,
    private val openLoginId: String,
) : Screen() {
    private val session = RunnerLoginSession(runner, provider)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var tick by mutableIntStateOf(0)
    private var code by mutableStateOf("")
    private var sending by mutableStateOf(false)
    private var inputError by mutableStateOf<String?>(null)
    private var fallbackUrl by mutableStateOf("")
    private var started = false
    private var refreshed = false

    override val title: String
        get() = "Sign in ${RunnerAttention.providerLong(session.provider)} on ${session.runner}"

    override fun onDisposed() {
        scope.cancel()
        if (session.needsCancelOnClose) {
            val api = RunnerAttentionFeature.api()
            val runner = session.runner
            val id = session.id
            Thread {
                runCatching { api?.cancel(runner, id) }
                RunnerAttentionFeature.refreshAfterSignIn()
            }.start()
        }
    }

    private suspend fun call(block: () -> RunnerReply): RunnerReply? =
        withContext(Dispatchers.IO) { runCatching(block).getOrNull() }

    private fun start(openUrl: (String) -> Unit) {
        if (started) return
        started = true
        scope.launch {
            val api = RunnerAttentionFeature.api()
            if (api == null || !api.hasSession) {
                session.markFailed("Sign in to your ForkMesh account on this phone first. Opening the Runners page instead.")
                fallback(api, openUrl)
                return@launch
            }
            if (RunnerAttention.validLoginId(openLoginId)) {
                session.attach(openLoginId)
            } else {
                val reply = call { api.startLogin(session.runner, session.provider) }
                if (reply == null) {
                    session.markFailed("Could not reach ForkMesh. Check the connection and try again.")
                    tick++
                    return@launch
                }
                session.applyStartReply(reply.status, reply.body)
                if (reply.status == 401 || reply.status == 403) {
                    fallback(api, openUrl)
                    return@launch
                }
            }
            tick++
            poll(api)
        }
    }

    private fun fallback(api: RunnerAttentionApi?, openUrl: (String) -> Unit) {
        val url = api?.dashboardUrl(session.runner, session.provider) ?: ""
        fallbackUrl = url
        tick++
        if (url.isNotEmpty()) openUrl(url)
    }

    private suspend fun poll(api: RunnerAttentionApi) {
        while (scope.isActive && session.open) {
            val reply = call { api.output(session.runner, session.id, session.afterSeq) }
            if (reply != null) {
                if (reply.status in 200..299) session.applyOutputReply(reply.body) else session.applyOutputStatus(reply.status)
                tick++
                if (reply.ok && session.more) continue
            }
            delay(RunnerLoginText.POLL_MS)
        }
        finished()
    }

    private fun finished() {
        if (refreshed || !session.ended) return
        refreshed = true
        RunnerAttentionFeature.refreshAfterSignIn()
    }

    private fun send() {
        val api = RunnerAttentionFeature.api() ?: return
        val data = RunnerLoginText.codeSubmission(code)
        if (data.isEmpty() || !RunnerLoginText.inputValid(data)) {
            inputError = "Paste the code the sign-in page shows: plain text, at most 4096 bytes."
            return
        }
        inputError = null
        sending = true
        scope.launch {
            val reply = call { api.input(session.runner, session.id, data) }
            when {
                reply == null -> inputError = "Could not reach ForkMesh. Try again."
                reply.status in 200..299 -> code = ""
                else -> inputError = reply.body.str("message").ifEmpty { "The code was not sent (HTTP ${reply.status})." }
            }
            sending = false
        }
    }

    private fun cancel() {
        val api = RunnerAttentionFeature.api() ?: return
        if (!session.needsCancelOnClose) return
        scope.launch {
            val reply = call { api.cancel(session.runner, session.id) }
            session.applyCancelReply(reply?.body ?: emptyMap())
            tick++
            finished()
        }
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val uri = LocalUriHandler.current
        val clipboard = LocalClipboardManager.current
        val openUrl: (String) -> Unit = { url -> runCatching { uri.openUri(url) } }
        LaunchedEffect(Unit) { start(openUrl) }
        DetailScaffold(title, navigator) {
            tick.let { }
            Column(
                Modifier
                    .fillMaxSize()
                    .navigationBarsPadding()
                    .imePadding()
                    .verticalScroll(rememberScrollState())
                    .padding(FmSpace.x4)
                    .testTag("runner-sign-in"),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
            ) {
                val statusColor = when {
                    session.error.isNotEmpty() || session.state == "failed" || session.state == "expired" -> c.danger
                    session.state == "succeeded" -> c.success
                    else -> c.textSecondary
                }
                Text(session.statusText, style = Fm.text.bodyMedium.copy(color = statusColor))
                for (link in session.urls.take(3)) {
                    val host = runCatching { java.net.URI(link).host }.getOrNull().orEmpty()
                    Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                        FmFilledButton("Open $host", onClick = { openUrl(link) }, icon = "open_in_new")
                        FmToneButton("Copy link", onPressed = { clipboard.setText(AnnotatedString(link)) }, icon = "content_copy")
                    }
                }
                val screen = session.screenText.trim()
                if (screen.isNotEmpty()) {
                    SelectionContainer {
                        Text(
                            screen,
                            modifier = Modifier
                                .fillMaxWidth()
                                .heightIn(min = 120.dp)
                                .clip(RoundedCornerShape(FmRadius.md))
                                .background(Color(0xFF111111))
                                .padding(FmSpace.x3),
                            style = TextStyle(fontFamily = FmType.mono, fontSize = 12.sp, lineHeight = 15.sp, color = Color(0xFFE5E5E5)),
                        )
                    }
                }
                if (session.open) {
                    FmTextField(
                        value = code,
                        onValueChange = { code = it },
                        modifier = Modifier.fillMaxWidth(),
                        hint = "Paste the code from the sign-in page",
                        errorText = inputError,
                        enabled = !sending,
                    )
                    Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                        FmToneButton("Send code", onPressed = if (sending) null else ({ send() }), icon = "send", tone = FmTone.Accent, filled = true)
                        FmToneButton("Cancel sign-in", onPressed = { cancel() }, icon = "close", tone = FmTone.Danger)
                    }
                } else if (session.ended) {
                    Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                        FmToneButton("Done", onPressed = { navigator.pop() }, tone = FmTone.Accent, filled = true)
                        if (fallbackUrl.isNotEmpty()) {
                            FmToneButton("Open Runners page", onPressed = { openUrl(fallbackUrl) }, icon = "open_in_new")
                        }
                    }
                }
            }
        }
    }
}
