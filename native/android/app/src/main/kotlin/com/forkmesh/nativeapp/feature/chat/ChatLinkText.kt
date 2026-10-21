package com.forkmesh.nativeapp.feature.chat

import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.LinkAnnotation
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.text.TextLinkStyles
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.withLink
import androidx.compose.ui.text.withStyle
import com.forkmesh.nativeapp.chat.ChatMentionKind
import com.forkmesh.nativeapp.chat.ChatProfileLink
import com.forkmesh.nativeapp.chat.ChatSmartMentions
import com.forkmesh.nativeapp.chat.linkChatReferences
import com.forkmesh.nativeapp.chat.parseChatText
import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.collab.ReferenceLinks
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.ui.kit.appendLink
import com.forkmesh.nativeapp.ui.kit.rememberLinkOpener
import com.forkmesh.nativeapp.ui.theme.Fm

internal data class ChatMentionColors(val person: Color, val you: Color, val youBackground: Color)

@Composable
internal fun chatMentionColors(): ChatMentionColors {
    val c = Fm.colors
    return ChatMentionColors(
        person = c.accent,
        you = if (c.dark) Color(0xFFFFD866) else Color(0xFF8A5A00),
        youBackground = if (c.dark) Color(0x40F2C744) else Color(0x55F2C744),
    )
}

@Composable
fun ChatLinkText(
    text: String,
    style: TextStyle,
    modifier: Modifier = Modifier,
    repo: RepoRef? = ReferenceLinks.CHAT_REPO,
    linkColor: Color? = null,
    onTextLayout: (TextLayoutResult) -> Unit = {},
) {
    val accent = linkColor ?: Fm.colors.accent
    val mentions = if (linkColor == null) chatMentionColors() else null
    val own = Services.get<AuthService>()?.session?.collectAsState()?.value?.nodeName.orEmpty()
    val open = rememberLinkOpener()
    val annotated = remember(text, accent, mentions, own, repo, open) {
        buildAnnotatedString {
            for (segment in linkChatReferences(parseChatText(text), repo)) {
                val url = segment.url
                if (url == null) {
                    appendMentions(segment.text, listOf(own), accent, mentions, open)
                } else {
                    appendLink(segment.text, url, accent, open)
                }
            }
        }
    }
    Text(annotated, style = style, modifier = modifier, onTextLayout = onTextLayout)
}

private fun AnnotatedString.Builder.appendMentions(
    text: String,
    own: List<String>,
    accent: Color,
    mentions: ChatMentionColors?,
    open: (String) -> Unit,
) {
    if ('@' !in text) {
        append(text)
        return
    }
    val pieces = ChatSmartMentions.split(text, own)
    pieces.forEachIndexed { index, piece ->
        val link = ChatProfileLink.mentionLink(pieces, index)
        when {
            link != null -> {
                val color = mentions?.person ?: accent
                val weight = if (mentions != null) FontWeight.Bold else null
                withLink(LinkAnnotation.Clickable(link, TextLinkStyles(SpanStyle(color = color, fontWeight = weight))) { open(link) }) {
                    append(piece.text)
                }
            }
            mentions == null || piece.kind == ChatMentionKind.Text -> append(piece.text)
            piece.kind == ChatMentionKind.Person -> withStyle(SpanStyle(color = mentions.person, fontWeight = FontWeight.Bold)) { append(piece.text) }
            else -> withStyle(SpanStyle(color = mentions.you, background = mentions.youBackground, fontWeight = FontWeight.Bold)) {
                append(piece.text)
            }
        }
    }
}
