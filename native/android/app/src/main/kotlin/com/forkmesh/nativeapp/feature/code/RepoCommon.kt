package com.forkmesh.nativeapp.feature.code

import com.forkmesh.nativeapp.feature.market.PluginMenuButton
import android.content.ClipData
import android.content.ClipboardManager
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.code.BadgeTone
import com.forkmesh.nativeapp.code.OwnerProofs
import com.forkmesh.nativeapp.code.RepoActivity
import com.forkmesh.nativeapp.code.RepoExplorer
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.ui.SoundCue
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmKeyValueRow
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSoundCues
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlin.coroutines.cancellation.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

internal val repoApi: ApiService get() = Services.require()

@Stable
internal class AsyncValue<T : Any> {
    var data: T? by mutableStateOf(null)
        private set
    var error: Throwable? by mutableStateOf(null)
        private set
    var waiting: Boolean by mutableStateOf(false)
        private set
    private var generation by mutableIntStateOf(0)
    private var loader: (() -> T)? = null
    private var completed = 0

    val started: Boolean get() = generation > 0

    fun reload(block: () -> T) {
        loader = block
        generation++
        waiting = true
    }

    @Composable
    fun Bind() {
        val current = generation
        LaunchedEffect(current) {
            if (current == 0 || completed == current) return@LaunchedEffect
            val block = loader ?: return@LaunchedEffect
            try {
                val value = withContext(Dispatchers.IO) { block() }
                if (generation == current) {
                    data = value
                    error = null
                    waiting = false
                    completed = current
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                if (generation == current) {
                    data = null
                    error = failure
                    waiting = false
                    completed = current
                }
            }
        }
    }
}

@Composable
internal fun navigationBarBottom(): Dp = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()

@Composable
internal fun toneColor(tone: BadgeTone): Color {
    val c = Fm.colors
    return when (tone) {
        BadgeTone.Success -> c.success
        BadgeTone.Danger -> c.danger
        BadgeTone.Warning -> c.warning
        BadgeTone.Accent -> c.accent
        BadgeTone.Secondary -> c.textSecondary
        BadgeTone.Tertiary -> c.textTertiary
    }
}

@Composable
internal fun CodeAppBar(
    title: String,
    navigator: Navigator,
    titleStyle: TextStyle = Fm.text.titleLarge,
    actions: @Composable RowScope.() -> Unit = {},
    bottom: (@Composable () -> Unit)? = null,
    pluginMenu: Boolean = true,
) {
    val c = Fm.colors
    Column(
        Modifier
            .fillMaxWidth()
            .background(c.bgRaised)
            .windowInsetsPadding(fmSafeAreaInsets(bottom = false)),
    ) {
        Row(
            Modifier
                .fillMaxWidth()
                .height(56.dp)
                .padding(end = FmSpace.x1),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(Modifier.width(56.dp), contentAlignment = Alignment.Center) {
                FmIconButton("arrow_back", "Back", onPressed = { navigator.pop() }, color = c.textPrimary, iconSize = 24.dp)
            }
            Spacer(Modifier.width(FmSpace.x4))
            Text(title, Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis, style = titleStyle)
            actions()
            if (pluginMenu) PluginMenuButton()
        }
        bottom?.invoke()
    }
}

@Composable
internal fun DetailScaffold(
    title: String,
    navigator: Navigator,
    actions: @Composable RowScope.() -> Unit = {},
    status: FmStatus? = null,
    pluginMenu: Boolean = true,
    content: @Composable BoxScope.() -> Unit,
) {
    Column(
        Modifier
            .fillMaxSize()
            .background(Fm.colors.bgBase),
    ) {
        CodeAppBar(title, navigator, actions = actions, pluginMenu = pluginMenu)
        if (status != null) {
            FmInlineStatus(
                status,
                Modifier
                    .background(Fm.colors.bgRaised)
                    .padding(horizontal = FmSpace.x4),
            )
        }
        Box(
            Modifier
                .weight(1f)
                .fillMaxWidth(),
            content = content,
        )
    }
}

@Composable
internal fun FmFilledButton(label: String, onClick: (() -> Unit)?, modifier: Modifier = Modifier, icon: String? = null) {
    val c = Fm.colors
    Button(
        onClick = {
            FmSoundCues.play(SoundCue.forLabel(label, SoundCue.CLICK))
            onClick?.invoke()
        },
        modifier = modifier.heightIn(min = 40.dp),
        enabled = onClick != null,
        shape = RoundedCornerShape(50),
        colors = ButtonDefaults.buttonColors(
            containerColor = c.textPrimary,
            contentColor = if (c.dark) c.bgBase else Color.White,
            disabledContainerColor = c.textPrimary.copy(alpha = .12f),
            disabledContentColor = c.textPrimary.copy(alpha = .38f),
        ),
        elevation = null,
        contentPadding = if (icon == null) PaddingValues(horizontal = 24.dp) else PaddingValues(start = 16.dp, end = 24.dp),
    ) {
        if (icon != null) {
            FmIcon(icon, size = 18.dp)
            Spacer(Modifier.width(FmSpace.x2))
        }
        Text(label)
    }
}

@Composable
internal fun FmOutlinedButton(label: String, onClick: (() -> Unit)?, modifier: Modifier = Modifier, icon: String? = null) {
    val c = Fm.colors
    val enabled = onClick != null
    OutlinedButton(
        onClick = {
            FmSoundCues.play(SoundCue.forLabel(label, SoundCue.CLICK))
            onClick?.invoke()
        },
        modifier = modifier.heightIn(min = 40.dp),
        enabled = enabled,
        shape = RoundedCornerShape(50),
        border = BorderStroke(1.dp, if (enabled) c.border else c.textPrimary.copy(alpha = .12f)),
        colors = ButtonDefaults.outlinedButtonColors(
            contentColor = c.textPrimary,
            disabledContentColor = c.textPrimary.copy(alpha = .38f),
        ),
        contentPadding = if (icon == null) PaddingValues(horizontal = 24.dp) else PaddingValues(start = 16.dp, end = 24.dp),
    ) {
        if (icon != null) {
            FmIcon(icon, size = 18.dp)
            Spacer(Modifier.width(FmSpace.x2))
        }
        Text(label)
    }
}

@Composable
internal fun InfoCard(title: String, modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    FmCard(modifier.fillMaxWidth(), radius = FmRadius.lg, padding = PaddingValues(FmSpace.x4)) {
        Column(Modifier.fillMaxWidth()) {
            Text(title, style = Fm.text.titleMedium)
            Spacer(Modifier.height(FmSpace.x3))
            content()
        }
    }
}

@Composable
internal fun LoadingCard(label: String, modifier: Modifier = Modifier) {
    InfoCard(label, modifier) { FmLinearProgressIndicator() }
}

@Composable
internal fun ErrorCard(message: String, onRetry: () -> Unit, modifier: Modifier = Modifier) {
    InfoCard("Something went wrong", modifier) {
        Text(message, color = Fm.colors.textSecondary)
        Spacer(Modifier.height(12.dp))
        FmOutlinedButton("Retry", onClick = onRetry)
    }
}

@Composable
internal fun EmptyCard(message: String, modifier: Modifier = Modifier) {
    InfoCard("Nothing here yet", modifier) { Text(message, color = Fm.colors.textSecondary) }
}

@Composable
internal fun RepoChip(icon: String, label: String, modifier: Modifier = Modifier) {
    val c = Fm.colors
    Row(
        modifier
            .background(c.bgBase, RoundedCornerShape(FmRadius.full))
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 14.dp, tint = c.textTertiary)
        Spacer(Modifier.width(FmSpace.x1))
        Text(label, style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.textSecondary))
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
internal fun ChipWrap(spacing: Dp = 8.dp, content: @Composable () -> Unit) {
    FlowRow(
        horizontalArrangement = Arrangement.spacedBy(spacing),
        verticalArrangement = Arrangement.spacedBy(spacing),
    ) { content() }
}

@Composable
internal fun SourceChip(source: String, modifier: Modifier = Modifier) {
    val label = RepoExplorer.sourceLabel(source) ?: return
    val c = Fm.colors
    val shape = RoundedCornerShape(FmRadius.full)
    Row(
        modifier
            .widthIn(max = 220.dp)
            .background(c.bgBase, shape)
            .border(1.dp, c.border, shape)
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("dns_outlined", size = 14.dp, tint = c.textTertiary)
        Spacer(Modifier.width(FmSpace.x1))
        Text(
            label,
            modifier = Modifier.weight(1f, fill = false),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.textSecondary),
        )
    }
}

@Composable
internal fun SmallBadge(text: String) {
    FmStatusBadge(label = text, color = toneColor(RepoActivity.badgeTone(text)))
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
internal fun MobileCard(
    icon: String,
    iconColor: Color,
    title: String,
    subtitle: String,
    badge: String = "",
    chips: List<String> = emptyList(),
    trailing: (@Composable () -> Unit)? = null,
    onTap: (() -> Unit)? = null,
) {
    val c = Fm.colors
    val trailingContent: (@Composable () -> Unit)? = trailing ?: if (badge.isEmpty()) null else ({ SmallBadge(badge) })
    FmListRow(
        title = title,
        titleContent = {
            Text(
                title,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.titleSmall.copy(color = c.textPrimary, lineHeight = FmType.body * 1.2f),
            )
        },
        subtitle = subtitle,
        subtitleContent = if (chips.isEmpty()) {
            null
        } else {
            {
                Column {
                    if (subtitle.isNotEmpty()) {
                        Text(
                            subtitle,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            style = Fm.text.bodySmall.copy(
                                fontWeight = FmType.emphasis,
                                color = c.textTertiary,
                                lineHeight = FmType.meta * 1.2f,
                            ),
                        )
                    }
                    Spacer(Modifier.height(FmSpace.x1))
                    FlowRow(
                        horizontalArrangement = Arrangement.spacedBy(6.dp),
                        verticalArrangement = Arrangement.spacedBy(6.dp),
                    ) {
                        for (chip in chips) SmallBadge(chip)
                    }
                }
            }
        },
        leading = { FmIconTile(icon, tint = iconColor) },
        trailing = trailingContent,
        showChevron = onTap != null,
        onTap = onTap,
    )
}

@Composable
internal fun PaddedCardList(padding: Dp = FmSpace.x4, bottomInset: Boolean = true, content: @Composable () -> Unit) {
    val inset = if (bottomInset) navigationBarBottom() else 0.dp
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = padding, top = padding, end = padding, bottom = padding + inset),
    ) {
        item { content() }
    }
}

internal fun <T> LazyListScope.dividedRows(items: List<T>, row: @Composable (T) -> Unit) {
    itemsIndexed(items) { index, item ->
        Column {
            row(item)
            if (index != items.lastIndex) FmRowDivider()
        }
    }
}

@Composable
internal fun <T : Any> AsyncList(value: AsyncValue<List<T>>, empty: String, row: @Composable (T) -> Unit) {
    value.Bind()
    Box(
        Modifier
            .fillMaxSize()
            .background(Fm.colors.listSurface),
    ) {
        val items = value.data
        val error = value.error
        when {
            value.waiting || !value.started -> PaddedCardList { LoadingCard("Loading…") }
            error != null -> PaddedCardList { EmptyCard("$error") }
            items.isNullOrEmpty() -> PaddedCardList { EmptyCard(empty) }
            else -> LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(bottom = FmSpace.x4 + navigationBarBottom()),
            ) {
                dividedRows(items, row)
            }
        }
    }
}

@Composable
internal fun Kv(label: String, value: String) {
    FmKeyValueRow(label = label, value = value)
}

@Composable
internal fun CopyableValue(label: String, value: String, copyLabel: String) {
    val c = Fm.colors
    val status = rememberFmStatus()
    val copy = rememberCopyText(status)
    Column {
        Text(label, style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.textSecondary))
        Spacer(Modifier.height(4.dp))
        Row(verticalAlignment = Alignment.Top) {
            SelectionContainer(Modifier.weight(1f)) {
                Text(value, color = c.textPrimary, lineHeight = FmType.small * 1.35f)
            }
            Spacer(Modifier.width(8.dp))
            FmIconButton("copy_all_outlined", "Copy $label", onPressed = { copy(copyLabel, value) }, iconSize = 18.dp)
        }
        FmInlineStatus(status)
    }
}

@Composable
internal fun rememberCopyText(status: FmStatus? = null): (label: String, value: String) -> Unit {
    val context = LocalContext.current
    return remember(context, status) {
        { label, value ->
            context.getSystemService(ClipboardManager::class.java)?.setPrimaryClip(ClipData.newPlainText(label, value))
            status?.show("$label copied")
        }
    }
}

internal class DesktopCommands(
    private val requireAccount: suspend (String) -> Boolean,
) {
    val status = FmStatus()

    suspend fun request(
        repo: Repository,
        command: String,
        target: String,
        payload: Map<String, Any?>,
        successMessage: String,
        failureMessage: String = RepoActivity.DESKTOP_REQUEST_NOT_QUEUED,
    ) {
        if (!requireAccount("request desktop control")) return
        try {
            val result = withContext(Dispatchers.IO) {
                val session = Services.get<AuthService>()?.session?.value
                val owner = OwnerProofs.desktopCommandOwner(repo, session?.nodeName ?: "")
                val proof = OwnerProofs.desktopCommand(
                    session = session,
                    identity = Services.get<DeviceIdentity>(),
                    owner = owner,
                    name = repo.name,
                    command = command,
                    target = target,
                )
                repoApi.desktopCommand(
                    owner,
                    repo.name,
                    ownerAccount = proof.ownerAccount,
                    command = command,
                    target = target,
                    ts = proof.ts,
                    sig = proof.sig,
                    payload = payload,
                )
            }
            if (result.ok) status.show(successMessage) else status.error(failureMessage)
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (error: Exception) {
            status.error("$error")
        }
    }
}

@Composable
internal fun rememberDesktopRoute(repo: Repository): Boolean {
    val availability = com.forkmesh.nativeapp.control.DesktopCommandAvailability.shared
    var available by remember(repo.fullName) { mutableStateOf(availability.available) }
    LaunchedEffect(repo.fullName) {
        available = withContext(Dispatchers.IO) { availability.refresh { repoApi.desktopCommandRouteAnswered(repo.owner, repo.name) } }
    }
    return available
}

@Composable
internal fun rememberDesktopCommands(): DesktopCommands {
    val requireAccount = AccountFeature.rememberRequireAccount()
    return remember(requireAccount) { DesktopCommands(requireAccount) }
}

@Composable
internal fun SmallSpinner(size: Dp, color: Color) {
    androidx.compose.material3.CircularProgressIndicator(
        modifier = Modifier.size(size),
        color = color,
        strokeWidth = 2.dp,
        trackColor = Color.Transparent,
    )
}
