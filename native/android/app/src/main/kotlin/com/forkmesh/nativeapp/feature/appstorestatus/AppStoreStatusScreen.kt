package com.forkmesh.nativeapp.feature.appstorestatus

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.market.MarketFeature
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.market.AppStoreBuild
import com.forkmesh.nativeapp.market.AppStoreCounts
import com.forkmesh.nativeapp.market.AppStoreFeedback
import com.forkmesh.nativeapp.market.AppStoreGroup
import com.forkmesh.nativeapp.market.AppStoreReview
import com.forkmesh.nativeapp.market.AppStoreSnapshot
import com.forkmesh.nativeapp.market.AppStoreState
import com.forkmesh.nativeapp.market.AppStoreStatus
import com.forkmesh.nativeapp.market.AppStoreStatusReply
import com.forkmesh.nativeapp.market.AppStoreSubmission
import com.forkmesh.nativeapp.market.AppStoreTone
import com.forkmesh.nativeapp.market.AppStoreVersion
import com.forkmesh.nativeapp.market.PackagesException
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.text.NumberFormat
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

internal class AppStoreStatusScreen : Screen() {
    override val title: String get() = "App Store Status"
    private var reply by mutableStateOf<AppStoreStatusReply?>(null)
    private var loading by mutableStateOf(false)
    private var refreshing by mutableStateOf(false)
    private var error by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        fun reload(pulled: Boolean) {
            if (loading) return
            val client = MarketFeature.client()
            if (client == null) {
                error = PackagesException.sentence("offline")
                return
            }
            loading = true
            refreshing = pulled
            scope.launch {
                val result = withContext(Dispatchers.IO) { runCatching { AppStoreStatus.load(client) } }
                result.onSuccess { reply = it; error = "" }.onFailure { error = PackagesException.describe(it) }
                loading = false
                refreshing = false
            }
        }
        LaunchedEffect(Unit) { if (reply == null) reload(false) }
        DetailScaffold(
            title,
            navigator,
            actions = { FmIconButton("refresh", "Refresh", onPressed = if (loading) null else ({ reload(false) }), color = Fm.colors.textPrimary) },
        ) {
            FmPullToRefreshBox(isRefreshing = refreshing, onRefresh = { reload(true) }, modifier = Modifier.fillMaxSize()) {
                val current = reply
                LazyColumn(
                    Modifier.fillMaxSize(),
                    contentPadding = PaddingValues(FmSpace.x4),
                    verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
                ) {
                    if (error.isNotEmpty()) item { Notice(error) }
                    val snapshot = current?.snapshot
                    when {
                        current == null && loading -> item {
                            Box(Modifier.fillMaxWidth().padding(FmSpace.x6), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                        }
                        current == null -> item {
                            FmEmptyState("cloud_off", "Couldn't load App Store Status", message = "Pull down or tap Refresh to try again.")
                        }
                        snapshot == null -> item {
                            FmEmptyState(
                                "phone_iphone",
                                "Nothing published yet",
                                message = "Your desktop publishes this page when the \"iOS App Store status\" shortcut runs. " +
                                    "Run it from the desktop's Shortcuts page or from your phone, then pull down to refresh.",
                            )
                        }
                        else -> snapshotItems(current, snapshot)
                    }
                }
            }
        }
    }

    private fun LazyListScope.snapshotItems(reply: AppStoreStatusReply, snapshot: AppStoreSnapshot) {
        item(key = "header") { Header(reply, snapshot) }
        if (snapshot.attention.isNotEmpty()) item(key = "attention") { Attention(snapshot.attention) }

        section("App Store versions", "storefront", snapshot, AppStoreStatus.VERSIONS, snapshot.versions.size, "No App Store versions yet.")
        itemsIndexed(snapshot.versions, key = { index, _ -> "version-$index" }) { _, row -> VersionCard(row) }

        section("App Review submissions", "inventory_2", snapshot, AppStoreStatus.SUBMISSIONS, snapshot.submissions.size, "No App Review submissions.")
        itemsIndexed(snapshot.submissions, key = { index, _ -> "submission-$index" }) { _, row -> SubmissionCard(row) }

        item(key = "downloads-header") { FmSectionHeader("Downloads", Modifier.padding(top = FmSpace.x3), icon = "download") }
        item(key = "downloads") { Downloads(snapshot) }

        section("TestFlight builds", "flight", snapshot, AppStoreStatus.BUILDS, snapshot.builds.size, "No TestFlight builds.")
        itemsIndexed(snapshot.builds, key = { index, _ -> "build-$index" }) { _, row -> BuildCard(row) }

        section("TestFlight groups", "groups", snapshot, AppStoreStatus.GROUPS, snapshot.groups.size, "No TestFlight groups.")
        itemsIndexed(snapshot.orderedGroups, key = { index, _ -> "group-$index" }) { _, row -> GroupCard(row) }

        section("Tester feedback", "feedback", snapshot, AppStoreStatus.FEEDBACK, snapshot.feedback.size, "No tester feedback.")
        itemsIndexed(snapshot.feedback, key = { index, _ -> "feedback-$index" }) { _, row -> FeedbackCard(row) }

        section("Customer reviews", "reviews", snapshot, AppStoreStatus.REVIEWS, snapshot.reviews.size, "No customer reviews yet.")
        itemsIndexed(snapshot.reviews, key = { index, _ -> "review-$index" }) { _, row -> ReviewCard(row) }

        item(key = "footer") { Spacer(Modifier.padding(FmSpace.x3)) }
    }

    private fun LazyListScope.section(title: String, icon: String, snapshot: AppStoreSnapshot, key: String, count: Int, empty: String) {
        item(key = "$key-header") { FmSectionHeader(title, Modifier.padding(top = FmSpace.x3), icon = icon, count = count.takeIf { it > 0 }) }
        val unavailable = snapshot.unavailable(key)
        when {
            unavailable.isNotEmpty() -> item(key = "$key-unavailable") { Unavailable(unavailable) }
            count == 0 -> item(key = "$key-empty") { Muted(empty) }
        }
    }
}

@Composable
private fun Header(reply: AppStoreStatusReply, snapshot: AppStoreSnapshot) {
    val c = Fm.colors
    FmCard(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            FmIcon("phone_iphone", size = 28.dp, tint = c.textSecondary)
            Spacer(Modifier.width(FmSpace.x3))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                Text(
                    snapshot.bundleId.ifEmpty { "iOS app" },
                    style = Fm.mono(color = c.textPrimary),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                val published = reply.publishedAt.takeIf { it > 0 } ?: snapshot.generatedAt
                val facts = listOfNotNull(
                    if (published > 0) "Published ${relativeTime(published)}" else null,
                    snapshot.appId.takeIf { it.isNotEmpty() }?.let { "App ID $it" },
                )
                if (facts.isNotEmpty()) Text(facts.joinToString(" · "), style = Fm.text.bodySmall.copy(color = c.textSecondary))
            }
        }
    }
}

@Composable
private fun Attention(lines: List<String>) {
    val tone = Fm.colors.danger
    val shape = RoundedCornerShape(FmRadius.md)
    Column(
        Modifier
            .fillMaxWidth()
            .clip(shape)
            .background(tone.copy(alpha = .12f))
            .border(1.dp, tone.copy(alpha = .5f), shape)
            .padding(FmSpace.x3),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            FmIcon("error_outline", size = 18.dp, tint = tone)
            Spacer(Modifier.width(FmSpace.x2))
            Text("Needs action", style = Fm.text.titleSmall.copy(color = tone, fontWeight = FontWeight.SemiBold))
        }
        lines.forEach { line -> Text("• $line", style = Fm.text.bodyMedium.copy(color = Fm.colors.textPrimary)) }
    }
}

@Composable
private fun VersionCard(row: AppStoreVersion) {
    FmCard(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            TitleRow(AppStoreStatus.buildName(row.version, row.build)) { StatusChip(row.status) }
            val facts = listOfNotNull(
                row.releaseType.takeIf { it.isNotEmpty() }?.let { "Release ${humanize(it)}" },
                stampText(row.createdAt).takeIf { it.isNotEmpty() }?.let { "Created $it" },
            )
            if (facts.isNotEmpty()) Muted(facts.joinToString(" · "))
        }
    }
}

@Composable
private fun SubmissionCard(row: AppStoreSubmission) {
    FmCard(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            val submitted = stampText(row.submittedAt)
            TitleRow(if (submitted.isEmpty()) "Submission" else "Submitted $submitted") { StatusChip(row.status) }
            row.items.forEach { item ->
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(item.name.ifEmpty { "Item" }, Modifier.weight(1f), style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
                    StatusChip(item.status)
                }
            }
        }
    }
}

@Composable
private fun Downloads(snapshot: AppStoreSnapshot) {
    val downloads = snapshot.downloads
    val unavailable = snapshot.unavailable(AppStoreStatus.DOWNLOADS)
    when {
        unavailable.isNotEmpty() -> Unavailable(unavailable)
        !downloads.available -> Muted(downloads.note.ifEmpty { "Download figures are not available yet." })
        else -> FmCard(Modifier.fillMaxWidth()) {
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
                Figures(downloads.totalLabel, downloads.total)
                downloads.recent.forEach { span -> Figures(span.label, span.counts) }
                if (downloads.note.isNotEmpty()) Muted(downloads.note)
            }
        }
    }
}

@Composable
private fun Figures(label: String, counts: AppStoreCounts) {
    val c = Fm.colors
    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
        Text(label, style = Fm.text.labelSmall.copy(color = c.textTertiary))
        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Figure("Downloads", counts.downloads, Modifier.weight(1f))
            Figure("Redownloads", counts.redownloads, Modifier.weight(1f))
            Figure("Updates", counts.updates, Modifier.weight(1f))
        }
    }
}

@Composable
private fun Figure(label: String, value: Long, modifier: Modifier) {
    val c = Fm.colors
    Column(
        modifier
            .clip(RoundedCornerShape(FmRadius.md))
            .background(c.bgOverlay)
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
    ) {
        Text(NumberFormat.getIntegerInstance().format(value), style = Fm.text.titleMedium.copy(color = c.textPrimary), maxLines = 1)
        Text(label, style = Fm.text.bodySmall.copy(color = c.textSecondary), maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun BuildCard(row: AppStoreBuild) {
    FmCard(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            TitleRow(AppStoreStatus.buildName(row.version, row.build)) {
                if (row.expired) FmStatusBadge("Expired", toneColor(AppStoreTone.NEUTRAL))
            }
            stampText(row.uploadedAt).takeIf { it.isNotEmpty() }?.let { Muted("Uploaded $it") }
            LabelledStatus("Processing", row.processing)
            LabelledStatus("Internal testing", row.internal)
            LabelledStatus("External testing", row.external)
            LabelledStatus("Beta review", row.betaReview)
            if (row.groups.isNotEmpty()) {
                FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x1), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                    row.groups.forEach { name -> Tag(name) }
                }
            }
        }
    }
}

@Composable
private fun GroupCard(row: AppStoreGroup) {
    val c = Fm.colors
    val uri = LocalUriHandler.current
    FmCard(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            TitleRow(row.name.ifEmpty { "Group" }) {
                FmStatusBadge(if (row.internal) "Internal" else "External", if (row.internal) c.accent else c.textTertiary)
            }
            val facts = listOf(
                if (row.testers == 1) "1 tester" else "${row.testers} testers",
                if (row.allBuilds) "every build" else "chosen builds",
                if (row.feedbackEnabled) "feedback on" else "feedback off",
            )
            Muted(facts.joinToString(" · "))
            val link = row.openableLink
            if (link != null) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        link + if (row.publicLinkLimit > 0) " · limit ${row.publicLinkLimit}" else "",
                        Modifier.weight(1f),
                        style = Fm.mono(color = c.textSecondary),
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Spacer(Modifier.width(FmSpace.x2))
                    FmToneButton("Open", onPressed = { runCatching { uri.openUri(link) } }, icon = "open_in_new", tone = FmTone.Accent)
                }
            } else if (!row.internal) {
                Muted(if (row.publicLinkEnabled) "Public link on" else "No public link")
            }
        }
    }
}

@Composable
private fun FeedbackCard(row: AppStoreFeedback) {
    val c = Fm.colors
    FmCard(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmIcon(if (row.crash) "bug_report" else "photo_camera", size = 16.dp, tint = if (row.crash) c.danger else c.textTertiary)
                Spacer(Modifier.width(FmSpace.x2))
                Text(row.from, Modifier.weight(1f), style = Fm.text.bodyMedium.copy(color = c.textPrimary), maxLines = 1, overflow = TextOverflow.Ellipsis)
                stampText(row.createdAt).takeIf { it.isNotEmpty() }?.let { Text(it, style = Fm.text.labelSmall.copy(color = c.textTertiary)) }
            }
            if (row.device.isNotEmpty()) Muted(row.device)
            if (row.comment.isNotEmpty()) Text(row.comment, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
            else Muted(if (row.crash) "Crash report with no comment" else "Screenshot with no comment")
            ScreenshotStrip(row)
        }
    }
}

@Composable
private fun ReviewCard(row: AppStoreReview) {
    val c = Fm.colors
    FmCard(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Row(Modifier.semantics { contentDescription = "${row.rating} of 5 stars" }) {
                    repeat(5) { index ->
                        FmIcon(if (index < row.rating) "star" else "star_border", size = 16.dp, tint = if (index < row.rating) c.warning else c.textTertiary)
                    }
                }
                Spacer(Modifier.weight(1f))
                val meta = listOf(row.territory, stampText(row.createdAt)).filter { it.isNotEmpty() }.joinToString(" · ")
                if (meta.isNotEmpty()) Text(meta, style = Fm.text.labelSmall.copy(color = c.textTertiary))
            }
            if (row.title.isNotEmpty()) Text(row.title, style = Fm.text.titleSmall.copy(color = c.textPrimary, fontWeight = FontWeight.SemiBold))
            if (row.body.isNotEmpty()) Text(row.body, style = Fm.text.bodyMedium.copy(color = c.textSecondary))
        }
    }
}

@Composable
private fun TitleRow(title: String, trailing: @Composable () -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(
            title,
            Modifier.weight(1f),
            style = Fm.text.titleSmall.copy(color = Fm.colors.textPrimary, fontWeight = FontWeight.SemiBold),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        Spacer(Modifier.width(FmSpace.x2))
        trailing()
    }
}

@Composable
private fun LabelledStatus(label: String, status: AppStoreState) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.weight(1f), style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
        StatusChip(status)
    }
}

@Composable
private fun StatusChip(status: AppStoreState) {
    FmStatusBadge(
        status.label,
        toneColor(status.tone),
        Modifier.semantics { contentDescription = if (status.state.isEmpty()) status.label else "${status.label} (${status.state})" },
    )
}

@Composable
private fun Tag(text: String) {
    val c = Fm.colors
    Text(
        text,
        Modifier
            .clip(RoundedCornerShape(FmRadius.full))
            .background(c.bgOverlay)
            .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1),
        style = Fm.text.labelSmall.copy(color = c.textSecondary),
        maxLines = 1,
    )
}

@Composable
private fun Unavailable(text: String) {
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(FmRadius.md))
            .background(c.warning.copy(alpha = .1f))
            .padding(FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("warning", size = 16.dp, tint = c.warning)
        Spacer(Modifier.width(FmSpace.x2))
        Text(text, style = Fm.text.bodySmall.copy(color = c.textSecondary))
    }
}

@Composable
private fun Notice(text: String) {
    Text(text, Modifier.padding(vertical = FmSpace.x1), style = Fm.text.bodySmall.copy(color = Fm.colors.danger))
}

@Composable
private fun Muted(text: String) {
    Text(text, style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
}

@Composable
private fun toneColor(tone: AppStoreTone): Color = when (tone) {
    AppStoreTone.ACTION -> Fm.colors.danger
    AppStoreTone.PENDING -> Fm.colors.warning
    AppStoreTone.GOOD -> Fm.colors.success
    AppStoreTone.NEUTRAL -> Fm.colors.textTertiary
}

private val STAMP_FORMAT: DateTimeFormatter = DateTimeFormatter.ofPattern("d MMM yyyy, HH:mm")

private fun stampText(stamp: String): String {
    val ms = AppStoreStatus.epochMs(stamp) ?: return ""
    return STAMP_FORMAT.format(Instant.ofEpochMilli(ms).atZone(ZoneId.systemDefault()))
}

private fun humanize(raw: String): String = raw.lowercase().replace('_', ' ').trim()
