package com.forkmesh.nativeapp.ui.kit

import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.LinkAnnotation
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextLinkStyles
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.withLink
import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.collab.ReferenceLinks
import com.forkmesh.nativeapp.collab.ReferenceRun
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.ui.theme.Fm

val LocalReferenceRepo = staticCompositionLocalOf<RepoRef?> { null }

@Composable
fun rememberLinkOpener(): (String) -> Unit {
    val context = LocalContext.current
    val uriHandler = LocalUriHandler.current
    return remember(context, uriHandler) {
        { url: String ->
            val handled = try {
                NavigationFeature.openUri(context, url)
            } catch (_: Exception) {
                false
            }
            if (!handled) runCatching { uriHandler.openUri(url) }
        }
    }
}

fun AnnotatedString.Builder.appendLink(text: String, url: String, accent: Color, open: (String) -> Unit) {
    val styles = TextLinkStyles(style = SpanStyle(color = accent, textDecoration = TextDecoration.Underline))
    withLink(LinkAnnotation.Clickable(url, styles) { open(url) }) { append(text) }
}

fun linkedAnnotated(runs: List<ReferenceRun>, accent: Color, open: (String) -> Unit): AnnotatedString =
    AnnotatedString.Builder().apply {
        for (run in runs) {
            val url = run.url
            if (url == null) append(run.text) else appendLink(run.text, url, accent, open)
        }
    }.toAnnotatedString()

@Composable
fun LinkedText(
    text: String,
    repo: RepoRef?,
    modifier: Modifier = Modifier,
    style: TextStyle = LocalTextStyle.current,
    commits: Boolean = true,
) {
    val accent = Fm.colors.accent
    val open = rememberLinkOpener()
    val runs = remember(text, repo, commits) { ReferenceLinks.runs(text, repo, commits) }
    if (runs.none { it.url != null }) {
        Text(text, modifier = modifier, style = style)
    } else {
        val annotated = remember(runs, accent, open) { linkedAnnotated(runs, accent, open) }
        Text(annotated, modifier = modifier, style = style)
    }
}
