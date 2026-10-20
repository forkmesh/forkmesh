package com.forkmesh.nativeapp.ui.kit

import com.forkmesh.nativeapp.ui.theme.FmHaptics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.role
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.clickable
import androidx.compose.foundation.background
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import com.forkmesh.nativeapp.ui.theme.FmPalette
import androidx.compose.ui.unit.sp
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.TextSelectionColors
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CheckboxColors
import androidx.compose.material3.CheckboxDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExtendedFloatingActionButton
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.FloatingActionButtonDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItemColors
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.MenuDefaults
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.PlainTooltip
import androidx.compose.material3.ProvideTextStyle
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TextFieldColors
import androidx.compose.material3.TooltipAnchorPosition
import androidx.compose.material3.TooltipBox
import androidx.compose.material3.TooltipDefaults
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.material3.rememberTooltipState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.DpOffset
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.SoundCue
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmMaterialTypography
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun FmTooltip(message: String, modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    val colors = Fm.colors
    TooltipBox(
        positionProvider = TooltipDefaults.rememberTooltipPositionProvider(TooltipAnchorPosition.Below),
        tooltip = {
            PlainTooltip(
                shape = RoundedCornerShape(FmRadius.sm),
                containerColor = if (colors.dark) colors.bgElevated else colors.textPrimary,
                contentColor = if (colors.dark) colors.textPrimary else Color.White,
            ) {
                Text(message, style = TextStyle(fontFamily = FmType.ui, fontSize = FmType.meta, fontWeight = FmType.medium))
            }
        },
        state = rememberTooltipState(),
        modifier = modifier,
        content = content,
    )
}

@Composable
fun FmTextButton(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    contentColor: Color = Fm.colors.accent,
    minHeight: Dp = 44.dp,
    contentPadding: PaddingValues = PaddingValues(horizontal = FmSpace.x3),
    textStyle: TextStyle? = null,
    content: @Composable RowScope.() -> Unit,
) {
    TextButton(
        onClick = {
            FmSoundCues.play(SoundCue.CLICK)
            onClick()
        },
        modifier = modifier.defaultMinSize(minHeight = minHeight),
        enabled = enabled,
        colors = ButtonDefaults.textButtonColors(contentColor = contentColor),
        contentPadding = contentPadding,
    ) {
        if (textStyle != null) ProvideTextStyle(textStyle) { content() } else content()
    }
}

@Composable
fun FmElevatedButton(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    content: @Composable RowScope.() -> Unit,
) {
    val colors = Fm.colors
    Button(
        onClick = {
            FmSoundCues.play(SoundCue.CLICK)
            onClick()
        },
        modifier = modifier,
        enabled = enabled,
        colors = ButtonDefaults.buttonColors(
            containerColor = colors.textPrimary,
            contentColor = if (colors.dark) colors.bgBase else Color.White,
        ),
        elevation = null,
        content = content,
    )
}

@Composable
fun FmFloatingActionButton(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit,
) {
    FloatingActionButton(
        onClick = {
            FmSoundCues.play(SoundCue.OPEN)
            onClick()
        },
        modifier = modifier,
        shape = MaterialTheme.shapes.large,
        containerColor = Fm.colors.accent,
        contentColor = Color.White,
        elevation = fmFabElevation(),
        content = content,
    )
}

@Composable
fun FmExtendedFloatingActionButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    icon: String? = null,
    expanded: Boolean = true,
) {
    val press = {
        FmSoundCues.play(SoundCue.forLabel(text, SoundCue.OPEN))
        onClick()
    }
    if (icon == null) {
        ExtendedFloatingActionButton(
            onClick = press,
            modifier = modifier,
            shape = MaterialTheme.shapes.large,
            containerColor = Fm.colors.accent,
            contentColor = Color.White,
            elevation = fmFabElevation(),
        ) { Text(text) }
    } else {
        ExtendedFloatingActionButton(
            text = { Text(text) },
            icon = { FmIcon(icon) },
            onClick = press,
            modifier = modifier,
            expanded = expanded,
            shape = MaterialTheme.shapes.large,
            containerColor = Fm.colors.accent,
            contentColor = Color.White,
            elevation = fmFabElevation(),
        )
    }
}

@Composable
private fun fmFabElevation() = FloatingActionButtonDefaults.elevation(
    defaultElevation = 2.dp,
    pressedElevation = 4.dp,
    focusedElevation = 4.dp,
    hoveredElevation = 4.dp,
)

@Composable
fun FmCircularProgressIndicator(
    modifier: Modifier = Modifier,
    progress: (() -> Float)? = null,
    color: Color = Fm.colors.accent,
    strokeWidth: Dp = 4.dp,
) {
    if (progress == null) {
        CircularProgressIndicator(
            modifier = modifier.size(36.dp),
            color = color,
            strokeWidth = strokeWidth,
            trackColor = Color.Transparent,
            strokeCap = StrokeCap.Square,
        )
    } else {
        CircularProgressIndicator(
            progress = progress,
            modifier = modifier.size(36.dp),
            color = color,
            strokeWidth = strokeWidth,
            trackColor = Color.Transparent,
            strokeCap = StrokeCap.Butt,
            gapSize = 0.dp,
        )
    }
}

@Composable
fun FmLinearProgressIndicator(
    modifier: Modifier = Modifier,
    progress: (() -> Float)? = null,
    color: Color = Fm.colors.accent,
    trackColor: Color = Fm.colors.bgOverlay,
) {
    if (progress == null) {
        LinearProgressIndicator(
            modifier = modifier.fillMaxWidth(),
            color = color,
            trackColor = trackColor,
            strokeCap = StrokeCap.Butt,
            gapSize = 0.dp,
        )
    } else {
        LinearProgressIndicator(
            progress = progress,
            modifier = modifier.fillMaxWidth(),
            color = color,
            trackColor = trackColor,
            strokeCap = StrokeCap.Butt,
            gapSize = 0.dp,
            drawStopIndicator = {},
        )
    }
}

object FmTextFieldDefaults {
    val contentPadding = PaddingValues(horizontal = FmSpace.x4, vertical = FmSpace.x3)
    val shape = RoundedCornerShape(FmRadius.lg)

    @Composable
    fun colors(): TextFieldColors {
        val c = Fm.colors
        val disabled = c.textPrimary.copy(alpha = .38f)
        return OutlinedTextFieldDefaults.colors(
            focusedTextColor = c.textPrimary,
            unfocusedTextColor = c.textPrimary,
            disabledTextColor = disabled,
            errorTextColor = c.textPrimary,
            focusedContainerColor = c.bgRaised,
            unfocusedContainerColor = c.bgRaised,
            disabledContainerColor = c.bgRaised,
            errorContainerColor = c.bgRaised,
            cursorColor = c.accent,
            errorCursorColor = c.danger,
            selectionColors = TextSelectionColors(handleColor = c.accent, backgroundColor = c.accent.copy(alpha = .28f)),
            focusedBorderColor = c.accent,
            unfocusedBorderColor = c.border,
            disabledBorderColor = c.textPrimary.copy(alpha = .12f),
            errorBorderColor = c.danger,
            focusedLeadingIconColor = c.textSecondary,
            unfocusedLeadingIconColor = c.textSecondary,
            disabledLeadingIconColor = disabled,
            errorLeadingIconColor = c.textSecondary,
            focusedTrailingIconColor = c.textSecondary,
            unfocusedTrailingIconColor = c.textSecondary,
            disabledTrailingIconColor = disabled,
            errorTrailingIconColor = c.danger,
            focusedPlaceholderColor = c.textTertiary,
            unfocusedPlaceholderColor = c.textTertiary,
            disabledPlaceholderColor = c.textTertiary,
            errorPlaceholderColor = c.textTertiary,
            focusedSupportingTextColor = c.textSecondary,
            unfocusedSupportingTextColor = c.textSecondary,
            disabledSupportingTextColor = disabled,
            errorSupportingTextColor = c.danger,
        )
    }
}

@Composable
fun FmTextField(
    value: String,
    onValueChange: (String) -> Unit,
    modifier: Modifier = Modifier,
    hint: String? = null,
    helperText: String? = null,
    errorText: String? = null,
    enabled: Boolean = true,
    readOnly: Boolean = false,
    minLines: Int = 1,
    maxLines: Int = 1,
    autofocus: Boolean = false,
    leading: (@Composable () -> Unit)? = null,
    trailing: (@Composable () -> Unit)? = null,
    keyboardOptions: KeyboardOptions = KeyboardOptions.Default,
    keyboardActions: KeyboardActions = KeyboardActions.Default,
    visualTransformation: VisualTransformation = VisualTransformation.None,
    textStyle: TextStyle = Fm.text.bodyLarge,
) {
    val interaction = remember { MutableInteractionSource() }
    val colors = FmTextFieldDefaults.colors()
    val c = Fm.colors
    val label = LocalFmFieldLabel.current
    val focus = remember { FocusRequester() }
    if (autofocus) LaunchedEffect(Unit) { focus.requestFocus() }
    val singleLine = maxLines == 1
    val isError = errorText != null
    val supporting = errorText ?: helperText
    BasicTextField(
        value = value,
        onValueChange = FmUserActivity.typing(onValueChange),
        modifier = modifier
            .fillMaxWidth()
            .focusRequester(focus)
            .then(if (label != null) Modifier.semantics { contentDescription = label } else Modifier),
        enabled = enabled,
        readOnly = readOnly,
        textStyle = textStyle.copy(color = if (enabled) c.textPrimary else c.textPrimary.copy(alpha = .38f)),
        keyboardOptions = keyboardOptions,
        keyboardActions = keyboardActions,
        singleLine = singleLine,
        maxLines = maxLines,
        minLines = minLines,
        visualTransformation = visualTransformation,
        interactionSource = interaction,
        cursorBrush = SolidColor(if (isError) c.danger else c.accent),
        decorationBox = { inner ->
            OutlinedTextFieldDefaults.DecorationBox(
                value = value,
                innerTextField = inner,
                enabled = enabled,
                singleLine = singleLine,
                visualTransformation = visualTransformation,
                interactionSource = interaction,
                isError = isError,
                placeholder = hint?.let { text -> { Text(text, style = textStyle.copy(color = Color.Unspecified)) } },
                leadingIcon = leading,
                trailingIcon = trailing,
                supportingText = supporting?.let { text -> { Text(text) } },
                colors = colors,
                contentPadding = FmTextFieldDefaults.contentPadding,
                container = {
                    OutlinedTextFieldDefaults.Container(
                        enabled = enabled,
                        isError = isError,
                        interactionSource = interaction,
                        colors = colors,
                        shape = FmTextFieldDefaults.shape,
                        focusedBorderThickness = 1.2.dp,
                        unfocusedBorderThickness = 1.dp,
                    )
                },
            )
        },
    )
}

@Composable
fun FmDropdownMenu(
    expanded: Boolean,
    onDismissRequest: () -> Unit,
    modifier: Modifier = Modifier,
    offset: DpOffset = DpOffset(0.dp, 0.dp),
    content: @Composable ColumnScope.() -> Unit,
) {
    val c = Fm.colors
    DropdownMenu(
        expanded = expanded,
        onDismissRequest = onDismissRequest,
        modifier = modifier.fmTapSounds().widthIn(min = 250.dp),
        offset = offset,
        shape = RoundedCornerShape(FmMenu.corner),
        containerColor = FmMenu.surface(c),
        tonalElevation = 0.dp,
        shadowElevation = 8.dp,
        border = BorderStroke(0.5.dp, if (c.dark) Color.White.copy(alpha = 0.08f) else Color.Black.copy(alpha = 0.04f)),
        content = content,
    )
}

@Composable
fun FmDropdownMenuItem(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    icon: String? = null,
    enabled: Boolean = true,
    textColor: Color? = null,
) {
    val c = Fm.colors
    val tone = textColor ?: c.textPrimary
    DropdownMenuItem(
        text = { Text(text, style = FmMaterialTypography.bodyLarge.copy(fontSize = 17.sp, lineHeight = 22.sp, fontWeight = FmType.regular)) },
        onClick = {
            FmSoundCues.play(SoundCue.forLabel(text, SoundCue.CLICK))
            onClick()
        },
        modifier = modifier.heightIn(min = 44.dp),
        leadingIcon = icon?.let { name -> { FmIcon(name, size = 20.dp, outlined = true) } },
        enabled = enabled,
        contentPadding = PaddingValues(horizontal = FmSpace.x4),
        colors = MenuDefaults.itemColors(
            textColor = tone,
            leadingIconColor = tone,
            disabledTextColor = c.textPrimary.copy(alpha = .38f),
            disabledLeadingIconColor = c.textPrimary.copy(alpha = .38f),
        ),
    )
}

object FmListTileDefaults {
    @Composable
    fun colors(): ListItemColors {
        val c = Fm.colors
        return ListItemDefaults.colors(
            containerColor = Color.Transparent,
            headlineColor = c.textPrimary,
            leadingIconColor = c.textSecondary,
            trailingIconColor = c.textSecondary,
            supportingColor = c.textPrimary,
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun FmPullToRefreshBox(
    isRefreshing: Boolean,
    onRefresh: () -> Unit,
    modifier: Modifier = Modifier,
    contentAlignment: Alignment = Alignment.TopStart,
    content: @Composable BoxScope.() -> Unit,
) {
    PullToRefreshBox(
        isRefreshing = isRefreshing,
        onRefresh = {
            FmSoundCues.play(SoundCue.CLICK)
            onRefresh()
        },
        modifier = modifier,
        contentAlignment = contentAlignment,
        content = content,
    )
}

@Composable
fun FmCheckbox(
    checked: Boolean,
    onCheckedChange: ((Boolean) -> Unit)?,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    colors: CheckboxColors = CheckboxDefaults.colors(
        checkedColor = Fm.colors.accent,
        uncheckedColor = Fm.colors.textTertiary,
        checkmarkColor = Fm.colors.onAccent,
    ),
) {
    Checkbox(
        checked = checked,
        onCheckedChange = onCheckedChange?.let { change ->
            { on: Boolean ->
                FmSoundCues.play(SoundCue.toggle(on))
                change(on)
            }
        },
        modifier = modifier,
        enabled = enabled,
        colors = colors,
    )
}

object FmMenu {
    val corner = 22.dp

    fun surface(c: FmPalette): Color = if (c.dark) Color(0xFF2C2C2E) else Color(0xFFFFFFFF)
}

@Composable
fun FmActionMenu(
    expanded: Boolean,
    onDismissRequest: () -> Unit,
    modifier: Modifier = Modifier,
    offset: DpOffset = DpOffset(0.dp, 0.dp),
    content: @Composable ColumnScope.() -> Unit,
) {
    val c = Fm.colors
    DropdownMenu(
        expanded = expanded,
        onDismissRequest = onDismissRequest,
        modifier = modifier.fmTapSounds().width(260.dp),
        offset = offset,
        shape = RoundedCornerShape(32.dp),
        containerColor = FmMenu.surface(c),
        tonalElevation = 0.dp,
        shadowElevation = 8.dp,
        border = BorderStroke(0.5.dp, if (c.dark) Color.White.copy(alpha = 0.08f) else Color.Black.copy(alpha = 0.04f)),
    ) {
        Column(Modifier.padding(horizontal = 14.dp, vertical = 6.dp), verticalArrangement = Arrangement.spacedBy(8.dp), content = content)
    }
}

@Composable
fun FmActionMenuButton(text: String, onClick: () -> Unit, destructive: Boolean = false, enabled: Boolean = true) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    Box(
        Modifier
            .fillMaxWidth()
            .height(48.dp)
            .clip(CircleShape)
            .background(c.textPrimary.copy(alpha = if (c.dark) 0.1f else 0.06f))
            .clickable(enabled = enabled) {
                haptics.tap(SoundCue.forLabel(text, SoundCue.CLICK))
                onClick()
            }
            .semantics { role = Role.Button },
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text,
            Modifier.padding(horizontal = FmSpace.x4),
            style = FmMaterialTypography.bodyLarge.copy(fontSize = 17.sp, lineHeight = 22.sp, color = if (!enabled) c.textDisabled else if (destructive) c.danger else c.accent),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

