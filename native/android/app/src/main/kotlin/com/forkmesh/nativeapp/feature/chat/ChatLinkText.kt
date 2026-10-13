package com.forkmesh.nativeapp.feature.chat

import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.chat.linkChatReferences
import com.forkmesh.nativeapp.chat.parseChatText
import com.forkmesh.nativeapp.collab.ReferenceLinks
import com.forkmesh.nativeapp.ui.kit.appendLink
import com.forkmesh.nativeapp.ui.kit.rememberLinkOpener
import com.forkmesh.nativeapp.ui.theme.Fm

@Composable
fun ChatLinkText(text: String, style: TextStyle, modifier: Modifier = Modifier, repo: RepoRef? = ReferenceLinks.CHAT_REPO) {
    val accent = Fm.colors.accent
    val open = rememberLinkOpener()
    val annotated = remember(text, accent, repo, open) {
        buildAnnotatedString {
            for (segment in linkChatReferences(parseChatText(text), repo)) {
                val url = segment.url
                if (url == null) {
                    append(segment.text)
                } else {
                    appendLink(segment.text, url, accent, open)
                }
            }
        }
    }
    Text(annotated, style = style, modifier = modifier)
}
