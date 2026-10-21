package com.forkmesh.nativeapp.feature.account

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.net.Uri
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonColors
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.kit.FmField
import com.forkmesh.nativeapp.ui.kit.FmSafeInput
import com.forkmesh.nativeapp.ui.kit.FmUserActivity
import com.forkmesh.nativeapp.ui.kit.LocalFmFieldLabel
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel

fun openExternalUrl(context: Context, url: String): Boolean = try {
    context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    true
} catch (_: ActivityNotFoundException) {
    false
}

fun isDebugBuild(context: Context): Boolean =
    (context.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0

abstract class AccountScreen : Screen() {
    protected val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    override fun onDisposed() {
        scope.cancel()
    }
}

@Composable
internal fun VGap(height: Dp) {
    Spacer(Modifier.height(height))
}

@Composable
internal fun HGap(width: Dp) {
    Spacer(Modifier.width(width))
}

@Composable
fun AccountFilledField(
    value: String,
    onValueChange: (String) -> Unit,
    modifier: Modifier = Modifier,
    hint: String? = null,
    fillColor: Color = Fm.colors.bgBase,
    verticalPadding: Dp = FmSpace.x3,
    obscure: Boolean = false,
    autofocus: Boolean = false,
    keyboardOptions: KeyboardOptions = KeyboardOptions.Default,
    keyboardActions: KeyboardActions = KeyboardActions.Default,
    textStyle: TextStyle = Fm.text.bodyLarge,
) {
    val c = Fm.colors
    val interaction = remember { MutableInteractionSource() }
    val focus = remember { FocusRequester() }
    if (autofocus) LaunchedEffect(Unit) { focus.requestFocus() }
    val label = LocalFmFieldLabel.current
    val transformation = if (obscure) remember { PasswordVisualTransformation() } else VisualTransformation.None
    val colors = OutlinedTextFieldDefaults.colors(
        focusedTextColor = c.textPrimary,
        unfocusedTextColor = c.textPrimary,
        focusedContainerColor = fillColor,
        unfocusedContainerColor = fillColor,
        disabledContainerColor = fillColor,
        cursorColor = c.accent,
        focusedBorderColor = c.accent,
        unfocusedBorderColor = Color.Transparent,
        disabledBorderColor = Color.Transparent,
        focusedPlaceholderColor = c.textTertiary,
        unfocusedPlaceholderColor = c.textTertiary,
    )
    BasicTextField(
        value = value,
        onValueChange = if (obscure) FmUserActivity.typing(onValueChange) else FmSafeInput.typing(onValueChange),
        modifier = modifier
            .fillMaxWidth()
            .focusRequester(focus)
            .then(if (label != null) Modifier.semantics { contentDescription = label } else Modifier),
        textStyle = textStyle.copy(color = c.textPrimary),
        keyboardOptions = keyboardOptions,
        keyboardActions = keyboardActions,
        singleLine = true,
        visualTransformation = transformation,
        interactionSource = interaction,
        cursorBrush = SolidColor(c.accent),
        decorationBox = { inner ->
            OutlinedTextFieldDefaults.DecorationBox(
                value = value,
                innerTextField = inner,
                enabled = true,
                singleLine = true,
                visualTransformation = transformation,
                interactionSource = interaction,
                placeholder = hint?.let { text -> { Text(text, style = textStyle.copy(color = Color.Unspecified)) } },
                colors = colors,
                contentPadding = PaddingValues(horizontal = FmSpace.x4, vertical = verticalPadding),
                container = {
                    OutlinedTextFieldDefaults.Container(
                        enabled = true,
                        isError = false,
                        interactionSource = interaction,
                        colors = colors,
                        shape = RoundedCornerShape(FmRadius.md),
                        focusedBorderThickness = 1.2.dp,
                        unfocusedBorderThickness = 1.dp,
                    )
                },
            )
        },
    )
}

@Composable
fun AccountLabeledField(
    label: String,
    value: String,
    onValueChange: (String) -> Unit,
    modifier: Modifier = Modifier,
    hint: String? = null,
    obscure: Boolean = false,
    autofocus: Boolean = false,
) {
    FmField(label = label, modifier = modifier.padding(bottom = FmSpace.x3)) {
        AccountFilledField(
            value = value,
            onValueChange = onValueChange,
            hint = hint,
            obscure = obscure,
            autofocus = autofocus,
        )
    }
}

@Composable
fun AccountFilledButton(
    label: String,
    icon: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    colors: ButtonColors = ButtonDefaults.buttonColors(),
) {
    Button(
        onClick = onClick,
        modifier = modifier,
        enabled = enabled,
        colors = colors,
        contentPadding = ButtonDefaults.ButtonWithIconContentPadding,
    ) {
        FmIcon(icon, size = 18.dp)
        Spacer(Modifier.width(ButtonDefaults.IconSpacing))
        Text(label)
    }
}

@Composable
fun AccountOutlinedButton(
    label: String,
    onClick: (() -> Unit)?,
    modifier: Modifier = Modifier,
    icon: String? = null,
    contentColor: Color? = null,
    borderColor: Color? = null,
    minHeight: Dp? = null,
) {
    OutlinedButton(
        onClick = { onClick?.invoke() },
        modifier = if (minHeight != null) modifier.defaultMinSize(minHeight = minHeight) else modifier,
        enabled = onClick != null,
        colors = if (contentColor != null) {
            ButtonDefaults.outlinedButtonColors(contentColor = contentColor)
        } else {
            ButtonDefaults.outlinedButtonColors()
        },
        border = if (borderColor != null) BorderStroke(1.dp, borderColor) else ButtonDefaults.outlinedButtonBorder(onClick != null),
        contentPadding = if (icon != null) ButtonDefaults.ButtonWithIconContentPadding else ButtonDefaults.ContentPadding,
    ) {
        if (icon != null) {
            FmIcon(icon, size = 18.dp)
            Spacer(Modifier.width(ButtonDefaults.IconSpacing))
        }
        Text(label)
    }
}
