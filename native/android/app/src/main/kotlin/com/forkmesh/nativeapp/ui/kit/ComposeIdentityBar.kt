package com.forkmesh.nativeapp.ui.kit

import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmType

fun composeIdentityName(nodeName: String, email: String): String = nodeName.ifEmpty { email }

@Composable
fun ComposeIdentityBar(
    name: String,
    modifier: Modifier = Modifier,
    avatarPng: String = "",
    verb: String? = null,
    padding: PaddingValues? = null,
) {
    val colors = Fm.colors
    val lead = if (verb.isNullOrEmpty()) "as " else "$verb as "
    Row(modifier.padding(padding ?: PaddingValues(vertical = 6.dp)), verticalAlignment = Alignment.CenterVertically) {
        FmAvatar(name = name, size = 28.dp, avatarPng = avatarPng, textStyle = Fm.text.labelMedium)
        Spacer(Modifier.width(8.dp))
        Text(
            buildAnnotatedString {
                append(lead)
                withStyle(SpanStyle(fontWeight = FmType.emphasis, color = colors.textPrimary)) { append(name) }
            },
            modifier = Modifier.weight(1f, fill = false),
            overflow = TextOverflow.Ellipsis,
            style = Fm.text.bodyMedium.copy(color = colors.textSecondary),
        )
    }
}
