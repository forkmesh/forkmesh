package com.forkmesh.nativeapp.feature.code.review

import androidx.compose.foundation.layout.Arrangement
import com.forkmesh.nativeapp.ui.theme.FmType
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.ci.ApiCiClient
import com.forkmesh.nativeapp.ci.CiClient
import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.collab.ApiPullMergeReader
import com.forkmesh.nativeapp.collab.ApiReviewLedgerReader
import com.forkmesh.nativeapp.collab.CollabJsonCall
import com.forkmesh.nativeapp.collab.CollabRecordWriter
import com.forkmesh.nativeapp.collab.CollabSigner
import com.forkmesh.nativeapp.collab.CollabTone
import com.forkmesh.nativeapp.collab.MergeQueueRemote
import com.forkmesh.nativeapp.collab.PullMergeAllRemote
import com.forkmesh.nativeapp.collab.PullMergeClient
import com.forkmesh.nativeapp.collab.RepoArchiveReader
import com.forkmesh.nativeapp.collab.RepoSettingsClient
import com.forkmesh.nativeapp.control.DesktopControlFamily
import com.forkmesh.nativeapp.control.DesktopControlFamilyName
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.code.navigationBarBottom
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.model.RepoTree
import com.forkmesh.nativeapp.security.DeployApprover
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

internal object CodeServices {
    val api: ApiService get() = Services.require()

    val identity: DeviceIdentity? get() = Services.get()

    val prefs: PrefStore? get() = Services.get()

    val account: String get() = Services.get<AuthService>()?.session?.value?.nodeName.orEmpty()

    val sessionToken: String get() = Services.get<AuthService>()?.session?.value?.sessionToken.orEmpty()

    val call: CollabJsonCall get() = { method, path, query, body -> api.jsonExchange(method, path, query, body) }

    fun ci(): CiClient = ApiCiClient(api)

    fun ref(repo: Repository): RepoRef = RepoRef(repo.owner, repo.name)

    fun writer(repo: Repository): CollabRecordWriter {
        val signer = identity?.let { CollabSigner(it, authorName = account) }
        return CollabRecordWriter(call, signer, repo.owner, repo.name)
    }

    fun ledgerReader() = ApiReviewLedgerReader(api)

    fun mergeClient() = PullMergeClient(ApiPullMergeReader(api), approver = DeployApprover({ identity }))

    fun settings(repo: Repository) = RepoSettingsClient(call, repo.owner, repo.name) { sessionToken }

    fun mergeQueue(): MergeQueueRemote {
        val family = DesktopControlFamily(DesktopControlFamilyName.MergeQueue, AgentsFeature.hub)
        return MergeQueueRemote { suffix, params, timeout -> family.call(suffix, params, timeout) }
    }

    fun pullMergeAll(): PullMergeAllRemote {
        val family = DesktopControlFamily(DesktopControlFamilyName.Pulls, AgentsFeature.hub)
        return PullMergeAllRemote { suffix, params, timeout -> family.call(suffix, params, timeout) }
    }

    fun desktopPaired(): Boolean = runCatching { AgentsFeature.hub.desktop != null }.getOrDefault(false)

    fun archiveReader(): RepoArchiveReader = object : RepoArchiveReader {
        override fun tree(owner: String, name: String, path: String, ref: String): RepoTree = api.tree(owner, name, path, ref)
        override fun rawBytes(owner: String, name: String, path: String, ref: String, maxBytes: Int): ByteArray =
            api.rawBytes(owner, name, path, ref, maxBytes)
    }

    fun isOwner(repo: Repository): Boolean {
        val who = account
        if (who.isEmpty()) return false
        if (who.equals(repo.owner, ignoreCase = true)) return true
        return try {
            api.orgProfile(api.readOwner(repo.owner, repo.name)).isOwner
        } catch (_: Exception) {
            false
        }
    }

    fun canManageIssues(repo: Repository): Boolean {
        val who = account
        if (who.isEmpty()) return false
        if (who.equals(repo.owner, ignoreCase = true)) return true
        return try {
            api.orgProfile(api.readOwner(repo.owner, repo.name)).canManage
        } catch (_: Exception) {
            false
        }
    }

    fun canMergePulls(repo: Repository): Boolean {
        if (account.isEmpty()) return false
        return try {
            api.orgProfile(api.readOwner(repo.owner, repo.name)).isOwner
        } catch (_: Exception) {
            false
        }
    }
}

internal suspend fun <T> io(block: () -> T): T = withContext(Dispatchers.IO) { block() }

internal class Tick {
    var value by mutableIntStateOf(0)
        private set

    fun bump() {
        value++
    }
}

@Composable
internal fun rememberTick(): Tick = remember { Tick() }

@Composable
internal fun toneOf(tone: CollabTone): Color {
    val c = Fm.colors
    return when (tone) {
        CollabTone.Success -> c.success
        CollabTone.Danger -> c.danger
        CollabTone.Accent, CollabTone.AccentEdge -> c.accent
        CollabTone.Secondary -> c.textSecondary
        CollabTone.Tertiary -> c.textTertiary
    }
}

internal fun fmTone(tone: CollabTone): FmTone = when (tone) {
    CollabTone.Success -> FmTone.Success
    CollabTone.Danger -> FmTone.Danger
    CollabTone.Accent, CollabTone.AccentEdge -> FmTone.Accent
    else -> FmTone.Neutral
}

@Composable
internal fun ScreenList(content: LazyListScope.() -> Unit) {
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x3, bottom = FmSpace.x6 + navigationBarBottom()),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
        content = content,
    )
}

@Composable
internal fun Note(text: String, color: Color = Fm.colors.textSecondary) {
    Text(text, color = color, style = Fm.text.bodySmall)
}

@Composable
internal fun Mono(text: String, color: Color = Fm.colors.textPrimary) {
    Text(text, color = color, style = Fm.text.bodySmall.copy(fontFamily = FmType.mono))
}

@Composable
internal fun Stack(content: @Composable () -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) { content() }
}
