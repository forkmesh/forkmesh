package com.forkmesh.nativeapp.feature.code.collab

import com.forkmesh.nativeapp.feature.market.PluginMenuButton
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsBottomHeight
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AssistChip
import androidx.compose.material3.AssistChipDefaults
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.ListItem
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.collab.CollabChip
import com.forkmesh.nativeapp.collab.CollabTone
import com.forkmesh.nativeapp.collab.badgeTone
import com.forkmesh.nativeapp.ui.kit.ComposeIdentityBar
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmListTileDefaults
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.LinkedText
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.kit.composeIdentityName
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType

@Composable
internal fun toneColor(tone: CollabTone): Color {
    val colors = Fm.colors
    return when (tone) {
        CollabTone.Success -> colors.success
        CollabTone.Danger -> colors.danger
        CollabTone.Accent -> colors.accent
        CollabTone.AccentEdge -> colors.accentEdge
        CollabTone.Secondary -> colors.textSecondary
        CollabTone.Tertiary -> colors.textTertiary
    }
}

internal fun TextStyle.withHeight(factor: Float): TextStyle = copy(lineHeight = fontSize * factor)

@Composable
internal fun CollabScaffold(
    topBar: @Composable () -> Unit,
    modifier: Modifier = Modifier,
    bottomBar: (@Composable () -> Unit)? = null,
    content: @Composable BoxScope.() -> Unit,
) {
    Column(
        modifier
            .fillMaxSize()
            .background(Fm.colors.bgBase),
    ) {
        topBar()
        Box(
            Modifier
                .weight(1f)
                .fillMaxWidth(),
            content = content,
        )
        bottomBar?.invoke()
    }
}

@Composable
internal fun CollabTopBar(
    navigator: Navigator,
    title: String = "",
    leadingIcon: String = "arrow_back",
    leadingLabel: String = "Back",
    titleContent: (@Composable () -> Unit)? = null,
) {
    val colors = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .background(colors.bgRaised)
            .windowInsetsPadding(fmSafeAreaInsets(bottom = false))
            .height(64.dp)
            .padding(start = 4.dp, end = FmSpace.x4),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIconButton(
            icon = leadingIcon,
            label = leadingLabel,
            onPressed = { navigator.pop() },
            color = colors.textPrimary,
            iconSize = 24.dp,
        )
        Spacer(Modifier.width(20.dp))
        Box(Modifier.weight(1f)) {
            if (titleContent != null) {
                titleContent()
            } else {
                Text(
                    title,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.titleLarge.copy(color = colors.textPrimary),
                )
            }
        }
        PluginMenuButton()
    }
}

@Composable
internal fun NavigationBarSpacer() {
    Spacer(Modifier.windowInsetsBottomHeight(WindowInsets.navigationBars))
}

@Composable
internal fun MessageList(
    modifier: Modifier = Modifier,
    padding: Dp = FmSpace.x4,
    content: @Composable () -> Unit,
) {
    LazyColumn(modifier.fillMaxSize(), contentPadding = PaddingValues(padding)) {
        item { content() }
        item { NavigationBarSpacer() }
    }
}

@Composable
internal fun InfoCard(
    title: String,
    modifier: Modifier = Modifier,
    content: @Composable ColumnScope.() -> Unit,
) {
    FmCard(
        modifier.fillMaxWidth(),
        radius = FmRadius.lg,
        padding = PaddingValues(FmSpace.x4),
    ) {
        Column(Modifier.fillMaxWidth()) {
            Text(title, style = Fm.text.titleMedium.copy(color = Fm.colors.textPrimary))
            Spacer(Modifier.height(FmSpace.x3))
            content()
        }
    }
}

@Composable
internal fun LoadingCard(label: String) {
    InfoCard(label) { FmLinearProgressIndicator() }
}

@Composable
internal fun EmptyCard(message: String) {
    InfoCard("Nothing here yet") {
        Text(message, color = Fm.colors.textSecondary)
    }
}

@Composable
internal fun ChipView(chip: CollabChip) {
    val colors = Fm.colors
    Row(
        Modifier
            .background(colors.bgBase, RoundedCornerShape(FmRadius.full))
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(chip.icon, size = 14.dp, tint = colors.textTertiary)
        Spacer(Modifier.width(FmSpace.x1))
        Text(chip.label, style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = colors.textSecondary))
    }
}

@Composable
internal fun SmallBadge(text: String) {
    FmStatusBadge(label = text, color = toneColor(badgeTone(text)))
}

@Composable
internal fun MobileCard(
    icon: String,
    iconColor: Color,
    title: String,
    subtitle: String,
    badge: String = "",
    onTap: (() -> Unit)? = null,
) {
    FmListRow(
        title = title,
        titleContent = {
            Text(
                title,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.titleSmall.copy(color = Fm.colors.textPrimary).withHeight(1.2f),
            )
        },
        subtitle = subtitle,
        leading = { FmIconTile(icon, tint = iconColor) },
        trailing = if (badge.isEmpty()) null else { { SmallBadge(badge) } },
        showChevron = onTap != null,
        onTap = onTap,
    )
}

@Composable
internal fun <T> AsyncListBody(
    state: Loadable<List<T>>,
    empty: String,
    itemContent: @Composable (T) -> Unit,
) {
    Box(
        Modifier
            .fillMaxSize()
            .background(Fm.colors.listSurface),
    ) {
        when (state) {
            Loadable.Loading -> MessageList { LoadingCard("Loading…") }
            is Loadable.Failed -> MessageList { EmptyCard(state.message) }
            is Loadable.Ready -> if (state.value.isEmpty()) {
                MessageList { EmptyCard(empty) }
            } else {
                LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = 88.dp)) {
                    itemsIndexed(state.value) { index, item ->
                        if (index > 0) FmRowDivider()
                        itemContent(item)
                    }
                    item { NavigationBarSpacer() }
                }
            }
        }
    }
}

@Composable
internal fun SelectableBody(
    text: String,
    modifier: Modifier = Modifier,
    style: TextStyle = LocalTextStyle.current,
    repo: RepoRef? = null,
) {
    SelectionContainer(modifier) {
        LinkedText(text, repo, style = style)
    }
}

@Composable
internal fun CopyableValue(label: String, value: String, copyLabel: String, tag: String) {
    val colors = Fm.colors
    val status = rememberFmStatus()
    val copy = rememberCopyText(status)
    Column(Modifier.fillMaxWidth()) {
        Text(label, style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = colors.textSecondary))
        Spacer(Modifier.height(4.dp))
        Row(verticalAlignment = Alignment.Top) {
            SelectableBody(
                value,
                Modifier.weight(1f),
                style = LocalTextStyle.current.copy(color = colors.textPrimary).withHeight(1.35f),
            )
            Spacer(Modifier.width(8.dp))
            FmIconButton(
                icon = "copy_all_outlined",
                label = "Copy $label",
                onPressed = { copy(copyLabel, value) },
                modifier = Modifier.testTag(tag),
                iconSize = 18.dp,
            )
        }
        FmInlineStatus(status)
    }
}

private val StadiumShape = RoundedCornerShape(percent = 50)

@Composable
internal fun CollabFilledButton(
    label: String,
    icon: String?,
    onClick: (() -> Unit)?,
    modifier: Modifier = Modifier,
) {
    val colors = Fm.colors
    Button(
        onClick = { onClick?.invoke() },
        modifier = modifier,
        enabled = onClick != null,
        shape = StadiumShape,
        colors = ButtonDefaults.buttonColors(
            containerColor = colors.textPrimary,
            contentColor = if (colors.dark) colors.bgBase else Color.White,
            disabledContainerColor = colors.textPrimary.copy(alpha = .12f),
            disabledContentColor = colors.textPrimary.copy(alpha = .38f),
        ),
        elevation = null,
        contentPadding = if (icon == null) ButtonDefaults.ContentPadding else ButtonDefaults.ButtonWithIconContentPadding,
    ) {
        if (icon != null) {
            FmIcon(icon, size = 18.dp)
            Spacer(Modifier.width(ButtonDefaults.IconSpacing))
        }
        Text(label)
    }
}

@Composable
internal fun CollabOutlinedButton(
    label: String,
    icon: String?,
    onClick: (() -> Unit)?,
    modifier: Modifier = Modifier,
    busy: Boolean = false,
) {
    val colors = Fm.colors
    OutlinedButton(
        onClick = { onClick?.invoke() },
        modifier = modifier,
        enabled = onClick != null,
        shape = StadiumShape,
        colors = ButtonDefaults.outlinedButtonColors(
            contentColor = colors.textPrimary,
            disabledContentColor = colors.textPrimary.copy(alpha = .38f),
        ),
        border = BorderStroke(1.dp, colors.border),
        contentPadding = if (icon == null && !busy) ButtonDefaults.ContentPadding else ButtonDefaults.ButtonWithIconContentPadding,
    ) {
        if (busy) {
            FmCircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
            Spacer(Modifier.width(ButtonDefaults.IconSpacing))
        } else if (icon != null) {
            FmIcon(icon, size = 18.dp)
            Spacer(Modifier.width(ButtonDefaults.IconSpacing))
        }
        Text(label)
    }
}

@Composable
internal fun SheetHeader(title: String, subtitle: String? = null, subtitleMaxLines: Int = Int.MAX_VALUE) {
    ListItem(
        headlineContent = { Text(title, fontWeight = FmType.emphasis) },
        supportingContent = subtitle?.let { text ->
            { Text(text, maxLines = subtitleMaxLines, overflow = TextOverflow.Ellipsis) }
        },
        colors = FmListTileDefaults.colors(),
    )
}

@Composable
internal fun SheetTile(title: String, icon: String, iconTint: Color? = null, onClick: () -> Unit) {
    ListItem(
        headlineContent = { Text(title) },
        modifier = Modifier.clickable(onClick = onClick),
        leadingContent = { FmIcon(icon, tint = iconTint ?: LocalContentColor.current) },
        colors = FmListTileDefaults.colors(),
    )
}

@Composable
internal fun TimelineTile(
    icon: String,
    iconTint: Color,
    title: String,
    author: String,
    modifier: Modifier = Modifier,
    body: String = "",
    onTap: (() -> Unit)? = null,
    repo: RepoRef? = null,
) {
    val colors = Fm.colors
    val shape = RoundedCornerShape(FmRadius.lg)
    Row(
        modifier
            .padding(bottom = 10.dp)
            .fillMaxWidth()
            .clip(shape)
            .background(colors.bgBase)
            .then(if (onTap != null) Modifier.clickable(onClick = onTap) else Modifier)
            .padding(12.dp),
        verticalAlignment = Alignment.Top,
    ) {
        FmIcon(icon, size = 18.dp, tint = iconTint)
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f)) {
            Text(title, fontWeight = FmType.emphasis)
            if (author.isNotEmpty()) {
                Spacer(Modifier.height(2.dp))
                Text(author, style = Fm.text.bodySmall.copy(color = colors.textSecondary))
            }
            if (body.trim().isNotEmpty()) {
                Spacer(Modifier.height(8.dp))
                SelectableBody(body.trim(), repo = repo)
            }
        }
    }
}

@Composable
internal fun CollabActionChip(label: String, icon: String, onClick: (() -> Unit)?, modifier: Modifier = Modifier) {
    val colors = Fm.colors
    AssistChip(
        onClick = { onClick?.invoke() },
        label = { Text(label, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        modifier = modifier,
        enabled = onClick != null,
        leadingIcon = { FmIcon(icon, size = 16.dp) },
        colors = AssistChipDefaults.assistChipColors(
            labelColor = colors.textPrimary,
            leadingIconContentColor = colors.textPrimary,
            disabledLabelColor = colors.textPrimary.copy(alpha = .38f),
            disabledLeadingIconContentColor = colors.textPrimary.copy(alpha = .38f),
        ),
        border = AssistChipDefaults.assistChipBorder(
            enabled = onClick != null,
            borderColor = colors.border,
            disabledBorderColor = colors.textPrimary.copy(alpha = .12f),
        ),
    )
}

@Composable
internal fun CollabChoiceChip(label: String, selected: Boolean, onClick: () -> Unit) {
    val colors = Fm.colors
    FilterChip(
        selected = selected,
        onClick = onClick,
        label = { Text(label) },
        leadingIcon = if (selected) {
            { FmIcon("check", size = 18.dp) }
        } else {
            null
        },
        colors = FilterChipDefaults.filterChipColors(
            containerColor = Color.Transparent,
            labelColor = colors.textPrimary,
            selectedContainerColor = colors.bgOverlay,
            selectedLabelColor = colors.textPrimary,
            selectedLeadingIconColor = colors.textPrimary,
        ),
        border = FilterChipDefaults.filterChipBorder(
            enabled = true,
            selected = selected,
            borderColor = colors.border,
            selectedBorderColor = Color.Transparent,
        ),
    )
}

@Composable
internal fun SessionIdentityBar(verb: String) {
    val session = rememberSession() ?: return
    ComposeIdentityBar(
        name = composeIdentityName(session.nodeName, session.email),
        avatarPng = session.avatarPng,
        verb = verb,
    )
}

@Composable
internal fun CollabDialog(
    title: String,
    width: Dp,
    onDismiss: () -> Unit,
    actions: @Composable RowScope.() -> Unit,
    content: @Composable ColumnScope.() -> Unit,
) {
    FmAlertDialog(
        onDismissRequest = onDismiss,
        title = title,
        containerColor = Fm.colors.bgOverlay,
        shape = RoundedCornerShape(FmRadius.lg),
        actions = actions,
    ) {
        Column(
            Modifier
                .width(width)
                .verticalScroll(rememberScrollState()),
            content = content,
        )
    }
}
