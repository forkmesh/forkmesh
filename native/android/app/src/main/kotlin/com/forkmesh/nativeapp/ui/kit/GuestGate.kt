package com.forkmesh.nativeapp.ui.kit

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmColors
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace

const val GUEST_CHAT_CHANNEL = "general"

enum class GuestSignInChoice { LogIn, SignUp }

suspend fun FmMessenger.showGuestSignInSheet(action: String): GuestSignInChoice? =
    showModalBottomSheet<GuestSignInChoice>(isScrollControlled = true) { close ->
        GuestSignInSheet(action = action, onChoice = close)
    }

@Composable
fun GuestSignInSheet(
    action: String,
    onChoice: (GuestSignInChoice?) -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = Fm.colors
    val shape = RoundedCornerShape(FmRadius.md)
    Column(
        modifier
            .fillMaxWidth()
            .testTag("guest-sign-in-sheet")
            .background(colors.bgRaised)
            .drawBehind {
                val rule = 1.dp.toPx()
                drawLine(colors.border, Offset(0f, rule / 2), Offset(size.width, rule / 2), rule)
            }
            .padding(start = FmSpace.x4, top = FmSpace.x5, end = FmSpace.x4, bottom = FmSpace.x4),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        FmIcon("lock_outline_rounded", size = 28.dp, tint = colors.textSecondary)
        Spacer(Modifier.height(FmSpace.x3))
        Text(
            "Sign in to $action",
            modifier = Modifier.fillMaxWidth(),
            textAlign = TextAlign.Center,
            style = Fm.text.titleLarge,
        )
        Spacer(Modifier.height(FmSpace.x2))
        Text(
            "You are exploring ForkMesh as a guest. You can browse repositories, issues, pull requests and chat in " +
                "#$GUEST_CHAT_CHANNEL. Sign in whenever you want to do more.",
            modifier = Modifier.fillMaxWidth(),
            textAlign = TextAlign.Center,
            style = Fm.text.bodyMedium.copy(color = colors.textSecondary),
        )
        Spacer(Modifier.height(FmSpace.x5))
        Button(
            onClick = { onChoice(GuestSignInChoice.LogIn) },
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = 48.dp)
                .testTag("guest-sheet-log-in"),
            shape = shape,
            colors = ButtonDefaults.buttonColors(
                containerColor = colors.textPrimary,
                contentColor = if (colors.dark) FmColors.darkBgBase else FmColors.bgRaised,
            ),
        ) {
            Text("Log in", style = MaterialTheme.typography.titleMedium)
        }
        Spacer(Modifier.height(FmSpace.x2))
        OutlinedButton(
            onClick = { onChoice(GuestSignInChoice.SignUp) },
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = 48.dp)
                .testTag("guest-sheet-sign-up"),
            shape = shape,
            border = BorderStroke(1.dp, colors.border),
            colors = ButtonDefaults.outlinedButtonColors(contentColor = colors.textPrimary),
        ) {
            Text("Create an account", style = MaterialTheme.typography.titleMedium)
        }
        Spacer(Modifier.height(FmSpace.x1))
        FmTextButton(
            onClick = { onChoice(null) },
            modifier = Modifier.fillMaxWidth(),
            contentColor = colors.textSecondary,
        ) {
            Text("Keep exploring")
        }
    }
}

@Composable
fun GuestComposeNotice(
    action: String,
    onLogIn: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = Fm.colors
    val shape = RoundedCornerShape(FmRadius.md)
    Row(
        modifier
            .fillMaxWidth()
            .testTag("guest-compose-notice")
            .background(colors.bgOverlay, shape)
            .border(1.dp, colors.border, shape)
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("lock_outline_rounded", size = 16.dp, tint = colors.textTertiary)
        Spacer(Modifier.width(FmSpace.x2))
        Text(
            "Sign in to $action",
            modifier = Modifier.weight(1f),
            style = Fm.text.bodyMedium.copy(color = colors.textSecondary),
        )
        FmTextButton(onClick = onLogIn, minHeight = 40.dp) { Text("Log in") }
    }
}

@Composable
fun GuestModeBanner(onSignIn: () -> Unit, modifier: Modifier = Modifier) {
    val colors = Fm.colors
    Row(
        modifier
            .fillMaxWidth()
            .testTag("guest-mode-banner")
            .background(colors.accentSubtle)
            .padding(start = FmSpace.x4, top = FmSpace.x1, end = FmSpace.x2, bottom = FmSpace.x1),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("visibility_outlined", size = 16.dp, tint = colors.textSecondary)
        Spacer(Modifier.width(FmSpace.x2))
        Text(
            "Guest mode - browsing read-only",
            modifier = Modifier.weight(1f),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            style = Fm.text.bodySmall.copy(color = colors.textSecondary),
        )
        FmTextButton(
            onClick = onSignIn,
            modifier = Modifier.testTag("guest-banner-sign-in"),
            minHeight = 36.dp,
            contentPadding = PaddingValues(horizontal = FmSpace.x3),
        ) {
            Text("Sign in")
        }
    }
}
