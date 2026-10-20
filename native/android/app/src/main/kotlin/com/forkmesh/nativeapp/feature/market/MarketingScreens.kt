package com.forkmesh.nativeapp.feature.market

import android.content.ClipData
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
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
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import com.forkmesh.nativeapp.feature.composer.PhotoPicker
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.feature.tools.copyToClipboard
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.market.MarketingCampaignAd
import com.forkmesh.nativeapp.market.MarketingChannelStats
import com.forkmesh.nativeapp.market.MarketingContent
import com.forkmesh.nativeapp.market.MarketingDraft
import com.forkmesh.nativeapp.market.MarketingForms
import com.forkmesh.nativeapp.market.MarketingKind
import com.forkmesh.nativeapp.market.MarketingModel
import com.forkmesh.nativeapp.market.MarketingOverview
import com.forkmesh.nativeapp.market.MarketingPerformanceRow
import com.forkmesh.nativeapp.market.MarketingReadyPost
import com.forkmesh.nativeapp.market.MarketingReadyPosts
import com.forkmesh.nativeapp.market.MarketingRecord
import com.forkmesh.nativeapp.market.MarketingSection
import com.forkmesh.nativeapp.market.MarketingSuggestion
import com.forkmesh.nativeapp.market.MarketingText
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmExtendedFloatingActionButton
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmKeyValueRow
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

internal object MarketingRevision {
    var value by mutableIntStateOf(0)
        private set

    fun bump() {
        value++
    }
}

internal class MarketingRunner(private val scope: CoroutineScope, private val model: MarketingModel) {
    var busy by mutableIntStateOf(0)
        private set
    val status: FmStatus get() = shared

    fun <T> run(work: MarketingModel.() -> T, done: (T) -> Unit = {}) {
        busy++
        status.clear()
        scope.launch {
            val result = withContext(Dispatchers.IO) { model.work() }
            busy--
            MarketingRevision.bump()
            val notice = model.notice
            if (notice.isNotEmpty()) {
                model.notice = ""
                status.report(notice, result == false)
            } else if ((result == false) && model.errorText.isNotEmpty()) {
                status.error(model.errorText)
            }
            done(result)
        }
    }

    companion object {
        private val shared = FmStatus()
    }
}

@Composable
internal fun rememberMarketingRunner(model: MarketingModel): MarketingRunner {
    val scope = rememberCoroutineScope()
    return remember(model) { MarketingRunner(scope, model) }
}

@Composable
internal fun MarketingRunnerStatus(runner: MarketingRunner, model: MarketingModel, modifier: Modifier = Modifier) {
    val status = runner.status
    FmInlineStatusLine(
        status.message,
        status.isError,
        modifier,
        actionLabel = status.actionLabel,
        onAction = status::runAction,
        onDismiss = {
            if (status.isError && model.errorText == status.message) model.errorText = ""
            status.clear()
        },
    )
}

internal object MarketingTint {
    val green = Color(0xFF16A34A)
    val orange = Color(0xFFEA580C)

    fun level(level: String): Color = when (level) {
        "warning" -> orange
        "win" -> green
        else -> Color(0xFF3B82F6)
    }

    fun icon(level: String): String = when (level) {
        "warning" -> "warning"
        "action" -> "arrow_circle_right"
        "setup" -> "build"
        "win" -> "star"
        else -> "info"
    }
}

private class MarketingScreenImpl(section: MarketingSection) : Screen() {
    override val title: String get() = "Marketing"

    private val model = MarketFeature.client()?.let { MarketingModel(it) }
    private var section by mutableStateOf(section)
    private var loading by mutableStateOf(true)
    private var reload by mutableIntStateOf(1)
    private var loadOrg by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val model = model ?: return
        val runner = rememberMarketingRunner(model)
        LaunchedEffect(reload) {
            loading = true
            withContext(Dispatchers.IO) { model.refresh() }
            loading = false
            MarketingRevision.bump()
        }
        LaunchedEffect(loadOrg) {
            if (loadOrg == 0) return@LaunchedEffect
            loading = true
            withContext(Dispatchers.IO) { model.reload() }
            loading = false
            MarketingRevision.bump()
        }
        @Suppress("UNUSED_EXPRESSION") MarketingRevision.value
        val overview = model.overview
        Box(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            Column(Modifier.fillMaxSize()) {
                ToolsAppBar("Marketing", navigator) {
                    FmIconButton("refresh", "Refresh", onPressed = { reload++ }, color = Fm.colors.textPrimary)
                }
                OrganizationPicker(model.packages.organizations, model.organization, "Marketing") { name ->
                    model.packages.organization = name
                    loadOrg++
                }
                MarketingSectionChips(section) { section = it }
                ErrorLine(if (model.errorText == runner.status.message) "" else model.errorText)
                when {
                    loading && overview == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                    overview == null -> FmEmptyState("extension", "Nothing to show", message = "Install Marketing for an organization in the Market.")
                    else -> LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = 96.dp)) {
                        item(key = "runner-status") { MarketingRunnerStatus(runner, model, Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) }
                        marketingSection(section, model, overview, runner, navigator) { section = it }
                    }
                }
            }
            if (overview != null) addAction(model)?.let { (label, draft) ->
                FmExtendedFloatingActionButton(
                    label, onClick = { navigator.push(MarketingFormScreen(model, draft)) },
                    modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp), icon = "add",
                )
            }
        }
    }

    private fun addAction(model: MarketingModel): Pair<String, MarketingDraft>? {
        val kind = when (section) {
            MarketingSection.OVERVIEW, MarketingSection.PLAN -> MarketingKind.CONTENT
            MarketingSection.CAMPAIGNS -> MarketingKind.CAMPAIGN
            MarketingSection.CHANNELS -> MarketingKind.POST
            MarketingSection.ADS -> MarketingKind.AD
            MarketingSection.GROWTH -> MarketingKind.EXPERIMENT
            MarketingSection.REDDIT -> MarketingKind.REDDIT_THREAD
            MarketingSection.PERFORMANCE -> return null
        }
        val draft = model.newDraft(kind) ?: return null
        return kind.title to draft
    }
}

@Composable
private fun MarketingSectionChips(section: MarketingSection, onSelect: (MarketingSection) -> Unit) {
    LazyRow(
        Modifier.fillMaxWidth().background(Fm.colors.bgRaised),
        contentPadding = PaddingValues(horizontal = 12.dp, vertical = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        items(MarketingSection.entries) { item ->
            CrmChip(item.title, Fm.colors.accent, filled = item == section, onTap = { onSelect(item) })
        }
    }
}

private fun LazyListScope.marketingSection(
    section: MarketingSection,
    model: MarketingModel,
    overview: MarketingOverview,
    runner: MarketingRunner,
    navigator: Navigator,
    select: (MarketingSection) -> Unit,
) {
    when (section) {
        MarketingSection.OVERVIEW -> overviewSection(model, overview, navigator, select)
        MarketingSection.PLAN -> item { MarketingContentPane(model, overview, runner, navigator) }
        MarketingSection.CAMPAIGNS -> campaignsSection(model, overview, runner, navigator)
        MarketingSection.CHANNELS -> item { MarketingChannelsPane(model, overview, runner, navigator) }
        MarketingSection.ADS -> item { MarketingAdsPane(model, overview, runner, navigator) }
        MarketingSection.GROWTH -> growthSection(model, overview, runner, navigator)
        MarketingSection.REDDIT -> redditSection(model, overview, runner, navigator)
        MarketingSection.PERFORMANCE -> item { MarketingPerformancePane(model, overview, runner, navigator) }
    }
}

@Composable
internal fun MarketingHeader(title: String, count: Int? = null) {
    FmSectionHeader(title, Modifier.padding(horizontal = 16.dp, vertical = 8.dp), count = count)
}

@Composable
internal fun MarketingNote(text: String) {
    Text(text, Modifier.padding(horizontal = 16.dp, vertical = 4.dp), style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
}

@Composable
internal fun MarketingRecordRow(
    record: MarketingRecord, overview: MarketingOverview, working: Boolean = false, onTap: () -> Unit, onLongPress: (() -> Unit)? = null, activity: String = "",
) {
    val facts = (record.facts(overview) + record.dates.map { "${it.label} ${shortDate(it.ms)}" }).joinToString(" · ")
    FmListRow(
        record.title.ifEmpty { "Untitled" },
        subtitle = if (activity.isEmpty()) facts.ifEmpty { null } else null,
        subtitleContent = if (activity.isEmpty()) null else ({
            Column {
                if (facts.isNotEmpty()) Text(facts, style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                Text(activity, style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
            }
        }),
        trailing = when {
            working -> ({ FmCircularProgressIndicator() })
            record.needsAttention -> ({ FmStatusBadge("Due", MarketingTint.orange) })
            else -> null
        },
        onTap = onTap,
        onLongPress = onLongPress,
    )
}

@Composable
internal fun MarketingInsightRow(level: String, title: String, detail: String, onTap: (() -> Unit)? = null) {
    FmListRow(
        title, subtitle = detail.ifEmpty { null }, leading = { FmIcon(MarketingTint.icon(level), tint = MarketingTint.level(level)) },
        onTap = onTap, showChevron = onTap != null,
    )
}

@Composable
internal fun MarketingStatusMenu(kind: MarketingKind, current: String, open: Boolean, onDismiss: () -> Unit, onPick: (String) -> Unit, extra: @Composable () -> Unit = {}) {
    FmDropdownMenu(expanded = open, onDismissRequest = onDismiss) {
        kind.statuses.filter { it != current }.forEach { status ->
            FmDropdownMenuItem("Move to ${MarketingText.title(status)}", onClick = { onDismiss(); onPick(status) })
        }
        extra()
    }
}

private fun LazyListScope.overviewSection(model: MarketingModel, overview: MarketingOverview, navigator: Navigator, select: (MarketingSection) -> Unit) {
    item {
        MarketingHeader("This organization")
        FmKeyValueRow("Content published", overview.published.toString(), Modifier.padding(horizontal = 16.dp))
        FmKeyValueRow("Queued or scheduled", overview.queued.toString(), Modifier.padding(horizontal = 16.dp))
        FmKeyValueRow("Followers", MarketingText.count(overview.followers), Modifier.padding(horizontal = 16.dp))
        FmKeyValueRow("Reach", MarketingText.count(overview.reach), Modifier.padding(horizontal = 16.dp))
        FmKeyValueRow("Engagements", MarketingText.count(overview.engagements), Modifier.padding(horizontal = 16.dp))
        FmListRow("Channels on track", subtitle = "${overview.healthCounts["good"] ?: 0} of ${overview.channels.size}", onTap = { select(MarketingSection.CHANNELS) })
        FmListRow(
            "Active ads",
            subtitle = "${overview.ads.count { it.status == "active" }}" + if (overview.adsNeedingAttention > 0) " · ${overview.adsNeedingAttention} off pace" else "",
            onTap = { select(MarketingSection.ADS) },
        )
        FmListRow("Experiments running", subtitle = "${overview.experimentsRunning} of ${overview.experimentsRunningLimit}", onTap = { select(MarketingSection.GROWTH) })
        FmListRow("Reddit threads to answer", subtitle = overview.openRedditThreads.size.toString(), onTap = { select(MarketingSection.REDDIT) })
    }
    if (overview.suggestions.isNotEmpty()) {
        item { MarketingHeader("Next steps", overview.suggestions.size) }
        items(overview.suggestions, key = { it.id }) { suggestion ->
            MarketingInsightRow(suggestion.level, suggestion.title, suggestion.detail) { openSuggestion(model, overview, suggestion, navigator) }
        }
    }
    item {
        MarketingHeader("Tools")
        if (overview.assistantAvailable) FmListRow("Ask the marketing assistant", leading = { FmIcon("auto_awesome") }, onTap = { navigator.push(MarketingAssistantScreen(model, "")) })
        FmListRow("Weekly report", leading = { FmIcon("description") }, onTap = { navigator.push(MarketingReportScreen(model)) })
        FmListRow("Build a tagged link", leading = { FmIcon("link") }, onTap = { navigator.push(MarketingLinkScreen(model)) })
        if (overview.canManageSettings) {
            if (overview.x.connected) {
                FmListRow("Post to X as @${overview.x.username}", leading = { FmIcon("send") }, onTap = { navigator.push(MarketingFormScreen(model, MarketingForms.xPostDraft(overview))) })
            }
            FmListRow("Marketing settings", leading = { FmIcon("settings") }, onTap = { navigator.push(MarketingSettingsScreen(model)) })
        }
        if (overview.x.connected && overview.x.lastPostedAt > 0) MarketingNote("Last posted to X ${relativeTime(overview.x.lastPostedAt)}.")
    }
}

private fun openSuggestion(model: MarketingModel, overview: MarketingOverview, suggestion: MarketingSuggestion, navigator: Navigator) {
    if (suggestion.actionType == "channel") {
        navigator.push(MarketingChannelScreen(model, suggestion.action.str("channel")))
        return
    }
    val draft = MarketingForms.suggestionDraft(suggestion, overview, model.zone) ?: return
    if (suggestion.actionType == "campaign-edit") navigator.push(MarketingCampaignScreen(model, draft.id)) else navigator.push(MarketingFormScreen(model, draft))
}

@Composable
private fun MarketingContentPane(model: MarketingModel, overview: MarketingOverview, runner: MarketingRunner, navigator: Navigator) {
    var query by remember { mutableStateOf("") }
    var status by remember { mutableStateOf("") }
    var channel by remember { mutableStateOf("") }
    var statusMenu by remember { mutableStateOf(false) }
    var channelMenu by remember { mutableStateOf(false) }
    Column {
        MarketingReadyPostCard(model, runner)
        FmTextField(query, { query = it }, Modifier.padding(horizontal = 16.dp, vertical = 8.dp), hint = "Search content")
        Row(Modifier.padding(horizontal = 16.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Box {
                FmToneButton(if (status.isEmpty()) "Every status" else MarketingText.title(status), onPressed = { statusMenu = true }, icon = "filter_list")
                FmDropdownMenu(expanded = statusMenu, onDismissRequest = { statusMenu = false }) {
                    FmDropdownMenuItem("Every status", onClick = { statusMenu = false; status = "" })
                    MarketingContent.STATUSES.forEach { item -> FmDropdownMenuItem(MarketingText.title(item), onClick = { statusMenu = false; status = item }) }
                }
            }
            Box {
                FmToneButton(if (channel.isEmpty()) "Every channel" else overview.channelName(channel), onPressed = { channelMenu = true }, icon = "hub")
                FmDropdownMenu(expanded = channelMenu, onDismissRequest = { channelMenu = false }) {
                    FmDropdownMenuItem("Every channel", onClick = { channelMenu = false; channel = "" })
                    overview.content.map { it.channel }.distinct().sorted().forEach { item ->
                        FmDropdownMenuItem(overview.channelName(item), onClick = { channelMenu = false; channel = item })
                    }
                }
            }
        }
        MarketingNote("${overview.published} published · ${overview.queued} queued")
        val items = overview.content(query, status, channel)
        if (items.isEmpty()) FmEmptyState("campaign", "No content planned")
        (if (status.isEmpty()) MarketingContent.STATUSES else listOf(status)).forEach { group ->
            val rows = items.filter { it.status == group }
            if (rows.isNotEmpty()) {
                MarketingHeader(MarketingText.title(group), rows.size)
                rows.forEach { item -> ContentRow(model, overview, item, runner, navigator) }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun MarketingReadyPostCard(model: MarketingModel, runner: MarketingRunner) {
    val ready = model.readyX ?: return
    val post = ready.post
    val context = LocalContext.current
    val notice = rememberFmStatus()
    val scope = rememberCoroutineScope()
    val missing = model.readyImageMissing
    LaunchedEffect(post?.id, missing) {
        if (missing) runner.run({ loadReadyPost() })
    }
    val image by produceState<ImageBitmap?>(null, post?.id, post?.imageDataUrl?.length) {
        value = post?.let { withContext(Dispatchers.Default) { readyBitmap(it) } }
    }
    val working = model.isWorking("ready-post")
    MarketingHeader("Ready for X")
    FmCard(Modifier.fillMaxWidth(), padding = PaddingValues(12.dp), margin = PaddingValues(horizontal = 16.dp)) {
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            if (post == null) {
                Text(MarketingReadyPosts.EMPTY_TEXT, style = Fm.text.bodyMedium, color = Fm.colors.textSecondary)
                ready.last?.let { last ->
                    val facts = listOf(last.statusTitle, relativeTime(last.closedAt), if (last.closedBy.isEmpty()) "" else "by ${last.closedBy}")
                    Text("Last post: " + facts.filter { it.isNotEmpty() }.joinToString(" · "), style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                    if (last.text.isNotEmpty()) Text(last.text, style = Fm.text.bodySmall, color = Fm.colors.textSecondary, maxLines = 3)
                    if (last.url.startsWith("https://")) FmToneButton("Open post", onPressed = { crmOpen(context, last.url) }, icon = "open_in_new")
                }
            } else {
                val bitmap = image
                if (bitmap != null) {
                    Image(
                        bitmap, post.imageAlt.ifEmpty { null }, Modifier.fillMaxWidth().clip(RoundedCornerShape(FmRadius.md)),
                        contentScale = ContentScale.FillWidth,
                    )
                } else if (post.hasImage) {
                    Box(Modifier.fillMaxWidth().height(120.dp), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                }
                SelectionContainer { Text(post.text, style = Fm.text.bodyMedium, color = Fm.colors.textPrimary) }
                Text(post.lengthText, style = Fm.text.labelSmall, color = if (post.overLimit) Fm.colors.danger else Fm.colors.textTertiary)
                if (post.why.isNotEmpty()) Text(post.why, style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                Text(post.written(relativeTime(post.createdAt)), style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    FmToneButton("Post to X", onPressed = {
                        scope.launch {
                            if (model.readyImageMissing) {
                                withContext(Dispatchers.IO) { model.loadReadyPost() }
                                MarketingRevision.bump()
                            }
                            val current = model.readyX?.post?.takeIf { it.id == post.id } ?: post
                            val direct = postReadyToX(context, current)
                            notice.show(if (direct) "Text copied too, in case X drops it." else "Text copied. Pick X to post.")
                        }
                    }, icon = "send", tone = FmTone.Accent, filled = true)
                    FmToneButton("Copy text", onPressed = {
                        copyToClipboard(context, post.text)
                        notice.show("Text copied.")
                    }, icon = "content_copy")
                    if (post.hasImage && Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                        FmToneButton("Save image", onPressed = if (post.imageDataUrl.isEmpty()) null else ({
                            scope.launch {
                                val saved = withContext(Dispatchers.IO) { saveReadyImage(context, post) }
                                if (saved) notice.show("Image saved to Pictures/ForkMesh.") else notice.error("The image could not be saved.")
                            }
                        }), icon = "download")
                    }
                    FmToneButton("Mark posted", onPressed = if (working) null else ({ runner.run({ closeReadyPost("posted") }) }), icon = "check", tone = FmTone.Success)
                    FmToneButton("Skip", onPressed = if (working) null else ({ runner.run({ closeReadyPost("skipped") }) }), icon = "skip_next")
                }
                FmInlineStatus(notice)
            }
        }
    }
}

private const val X_PACKAGE = "com.twitter.android"
private const val READY_SHARE_DIR = "marketing-share"

private fun readyBitmap(post: MarketingReadyPost): ImageBitmap? {
    val bytes = post.imageBytes() ?: return null
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
    if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null
    var sample = 1
    while (bounds.outWidth / (sample * 2) >= MarketImages.FULL_WIDTH) sample *= 2
    return BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply { inSampleSize = sample })?.asImageBitmap()
}

private fun readyImageUri(context: Context, post: MarketingReadyPost): Uri? {
    val bytes = post.imageBytes() ?: return null
    return runCatching {
        val folder = File(context.cacheDir, READY_SHARE_DIR).apply { mkdirs() }
        val file = File(folder, "ready-x." + post.imageExtension)
        file.writeBytes(bytes)
        FileProvider.getUriForFile(context, PhotoPicker.authority(context), file)
    }.getOrNull()
}

private suspend fun postReadyToX(context: Context, post: MarketingReadyPost): Boolean {
    copyToClipboard(context, post.text)
    val uri = withContext(Dispatchers.IO) { readyImageUri(context, post) }
    val send = Intent(Intent.ACTION_SEND).putExtra(Intent.EXTRA_TEXT, post.text)
    if (uri != null) {
        send.setType("image/*").putExtra(Intent.EXTRA_STREAM, uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        send.clipData = ClipData.newRawUri(post.imageAlt, uri)
    } else {
        send.setType("text/plain")
    }
    val direct = Intent(send).setPackage(X_PACKAGE).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    if (runCatching { context.startActivity(direct) }.isSuccess) return true
    runCatching { context.startActivity(Intent.createChooser(send, "Post to X").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
    return false
}

private fun saveReadyImage(context: Context, post: MarketingReadyPost): Boolean {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return false
    val bytes = post.imageBytes() ?: return false
    val values = ContentValues().apply {
        put(MediaStore.Images.Media.DISPLAY_NAME, "forkmesh-x-${post.id.take(8)}.${post.imageExtension}")
        put(MediaStore.Images.Media.MIME_TYPE, post.imageMime)
        put(MediaStore.Images.Media.RELATIVE_PATH, Environment.DIRECTORY_PICTURES + "/ForkMesh")
    }
    val resolver = context.contentResolver
    val uri = runCatching { resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values) }.getOrNull() ?: return false
    return runCatching { resolver.openOutputStream(uri)?.use { it.write(bytes) } != null }.getOrDefault(false)
}

@Composable
private fun ContentRow(model: MarketingModel, overview: MarketingOverview, item: MarketingContent, runner: MarketingRunner, navigator: Navigator) {
    var menu by remember { mutableStateOf(false) }
    val context = LocalContext.current
    Box {
        FmListRow(
            item.title,
            subtitle = listOf(
                overview.channelName(item.channel), item.campaign, shortDate(item.scheduledAt),
                if (item.publishedAt > 0) "Published ${shortDate(item.publishedAt)}" else "",
            ).filter { it.isNotEmpty() }.joinToString(" · "),
            trailing = if (model.isWorking(item.id)) ({ FmCircularProgressIndicator() }) else null,
            onTap = { navigator.push(MarketingFormScreen(model, MarketingDraft.of(MarketingKind.CONTENT, overview, item.json, item.id, model.zone))) },
            onLongPress = { menu = true },
        )
        MarketingStatusMenu(MarketingKind.CONTENT, item.status, menu, { menu = false }, { status -> runner.run({ setStatus(MarketingKind.CONTENT, item.id, status) }) }) {
            if (item.channel == "x" && overview.x.connected && overview.canManageSettings && item.status != "published") {
                FmDropdownMenuItem("Post to X now", onClick = { menu = false; navigator.push(MarketingFormScreen(model, MarketingForms.xPostDraft(overview, item))) }, icon = "send")
            }
            if (item.url.startsWith("https://")) FmDropdownMenuItem("Open published post", onClick = { menu = false; crmOpen(context, item.url) }, icon = "open_in_new")
            FmDropdownMenuItem("Delete", onClick = { menu = false; runner.run({ delete(MarketingKind.CONTENT, item.id) }) }, icon = "delete", textColor = Fm.colors.danger)
        }
    }
}

private fun LazyListScope.campaignsSection(model: MarketingModel, overview: MarketingOverview, runner: MarketingRunner, navigator: Navigator) {
    if (overview.campaigns.isEmpty()) item { FmEmptyState("flag", "No campaigns yet", message = "A campaign names one audience, one message, dates and a target.") }
    if (overview.campaignTemplates.isNotEmpty()) {
        item { MarketingHeader("Start from a template") }
        items(overview.campaignTemplates) { template ->
            FmListRow(template.str("name"), subtitle = template.str("label").ifEmpty { null }, leading = { FmIcon("content_copy") }, onTap = {
                navigator.push(MarketingFormScreen(model, MarketingForms.templateDraft(template, overview, model.zone)))
            })
        }
    }
    val open = overview.campaigns.filter { it.status !in setOf("completed", "archived") }
    val closed = overview.campaigns - open.toSet()
    listOf("Open" to open, "Finished" to closed).forEach { (title, rows) ->
        if (rows.isNotEmpty()) {
            item { MarketingHeader(title, rows.size) }
            items(rows, key = { "c-" + it.id }) { campaign ->
                var menu by remember { mutableStateOf(false) }
                Box {
                    MarketingRecordRow(campaign, overview, model.isWorking(campaign.id), onTap = { navigator.push(MarketingCampaignScreen(model, campaign.id)) }, onLongPress = { menu = true })
                    MarketingStatusMenu(MarketingKind.CAMPAIGN, campaign.status, menu, { menu = false }, { status -> runner.run({ setStatus(MarketingKind.CAMPAIGN, campaign.id, status) }) }) {
                        FmDropdownMenuItem("Delete", onClick = { menu = false; runner.run({ delete(MarketingKind.CAMPAIGN, campaign.id) }) }, icon = "delete", textColor = Fm.colors.danger)
                    }
                }
            }
        }
    }
}

@Composable
private fun MarketingChannelsPane(model: MarketingModel, overview: MarketingOverview, runner: MarketingRunner, navigator: Navigator) {
    var filter by remember { mutableStateOf("") }
    Column {
        LazyRow(contentPadding = PaddingValues(horizontal = 16.dp, vertical = 4.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            item { CrmChip("All", Fm.colors.accent, filled = filter.isEmpty(), onTap = { filter = "" }) }
            items(listOf("good", "attention", "stale", "setup", "paused")) { state ->
                CrmChip("${MarketingChannelStats.HEALTH_TITLES[state]} (${overview.healthCounts[state] ?: 0})", Fm.colors.accent, filled = filter == state, onTap = { filter = state })
            }
        }
        Row(Modifier.padding(horizontal = 16.dp, vertical = 4.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            if (overview.staleChannels.isNotEmpty()) FmToneButton("Update stale", onPressed = { runner.run({ refreshStale() }) }, icon = "refresh")
            if (overview.canManageSettings) FmToneButton("Add channels", onPressed = { navigator.push(MarketingAddChannelsScreen(model)) }, icon = "add", tone = FmTone.Accent)
        }
        MarketingHeader("Channels")
        val rows = overview.channels.filter { filter.isEmpty() || it.health == filter }
        if (rows.isEmpty()) MarketingNote("No channels here.")
        rows.forEach { channel ->
            MarketingChannelRow(overview, channel, model.isWorking("refresh:" + channel.id)) { navigator.push(MarketingChannelScreen(model, channel.id)) }
        }
        MarketingNote("Tap a channel to log its numbers, refresh it, or change its link. Channels the server cannot read are updated with the browser extension on a computer, or by hand here.")
        MarketingHeader("Posted links", overview.posts.size)
        if (overview.posts.isEmpty()) MarketingNote("Track the views, likes and replies of things you posted on any channel.")
        overview.posts.forEach { post ->
            MarketingRecordRow(post, overview, model.isWorking(post.id), onTap = {
                navigator.push(MarketingFormScreen(model, MarketingDraft.of(MarketingKind.POST, overview, post.json, post.id, model.zone)))
            })
        }
    }
}

@Composable
internal fun MarketingChannelRow(overview: MarketingOverview, channel: MarketingChannelStats, refreshing: Boolean, onTap: () -> Unit) {
    FmListRow(
        overview.channelName(channel.id),
        subtitleContent = {
            Column {
                Text(MarketingChannelFacts.text(channel), style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                if (channel.missingChecks.isNotEmpty()) Text(channel.missingChecks.joinToString(" · "), style = Fm.text.labelSmall, color = MarketingTint.orange)
                Text(if (channel.observedAt > 0) "Updated ${shortDate(channel.observedAt)}" else "Never updated", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
            }
        },
        trailing = {
            if (refreshing) FmCircularProgressIndicator()
            else if (channel.healthTitle.isNotEmpty()) FmStatusBadge(channel.healthTitle, if (channel.health == "good") MarketingTint.green else if (channel.health == "paused") Fm.colors.textTertiary else MarketingTint.orange)
        },
        onTap = onTap,
    )
}

internal object MarketingChannelFacts {
    fun text(channel: MarketingChannelStats): String {
        if (channel.isEmail) {
            val parts = mutableListOf("${MarketingText.count(channel.outbound)} emails sent in 30d", "${MarketingText.count(channel.closedDeals)} closed deals")
            if (channel.closedValue > 0) parts.add("${MarketingText.count(channel.closedValue)} won")
            return parts.joinToString(" · ")
        }
        if (channel.isAppStore) {
            val parts = mutableListOf("${MarketingText.count(channel.downloads)} downloads in 30d")
            parts.add(if (channel.rating > 0) String.format(java.util.Locale.ROOT, "%.1f★ from %s ratings", channel.rating, MarketingText.count(channel.ratings)) else "${MarketingText.count(channel.ratings)} ratings")
            return parts.joinToString(" · ")
        }
        if (channel.isGoogle) return "${MarketingText.count(channel.visits)} visits from Google in 30d · ${MarketingText.count(channel.indexedPages)} pages on Google"
        val parts = mutableListOf<String>()
        if (channel.isGitHub) {
            parts.add("${MarketingText.count(channel.stars)} stars")
            channel.starsChange?.let { parts.add(MarketingText.signed(it) + " in 30d") }
            parts.add("${MarketingText.count(channel.forks)} forks")
        }
        parts.add("${MarketingText.count(channel.followers)} followers")
        channel.followersChange?.let { parts.add(MarketingText.signed(it) + " in 30d") }
        channel.engagementRate?.let { parts.add(String.format(java.util.Locale.ROOT, "%.1f%% engagement", it)) }
        parts.add("${channel.postsLast30d} posts in 30d")
        if (channel.rating > 0) parts.add(String.format(java.util.Locale.ROOT, "%.1f★ from %s reviews", channel.rating, MarketingText.count(channel.reviews)))
        return parts.joinToString(" · ")
    }
}

@Composable
private fun MarketingAdsPane(model: MarketingModel, overview: MarketingOverview, runner: MarketingRunner, navigator: Navigator) {
    var query by remember { mutableStateOf("") }
    var status by remember { mutableStateOf("") }
    val context = LocalContext.current
    Column {
        if (overview.adTotals.isNotEmpty()) {
            MarketingHeader("Totals")
            overview.adTotals.forEach { totals ->
                FmListRow("${totals.currency} · ${totals.active} active of ${totals.ads}", subtitle = totals.facts.joinToString(" · "), showChevron = false)
            }
        }
        FmTextField(query, { query = it }, Modifier.padding(horizontal = 16.dp, vertical = 8.dp), hint = "Search ads")
        LazyRow(contentPadding = PaddingValues(horizontal = 16.dp, vertical = 4.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            item { CrmChip("All", Fm.colors.accent, filled = status.isEmpty(), onTap = { status = "" }) }
            items(MarketingCampaignAd.STATUSES) { item -> CrmChip(MarketingText.title(item), Fm.colors.accent, filled = status == item, onTap = { status = item }) }
        }
        val ads = overview.ads(status, query)
        if (ads.isEmpty()) MarketingNote("No ads here.")
        ads.forEach { ad ->
            var menu by remember(ad.id) { mutableStateOf(false) }
            Box {
                FmListRow(
                    ad.name.ifEmpty { ad.headline },
                    subtitleContent = {
                        Column {
                            Text(listOf(overview.channelName(ad.channel), ad.platform, ad.campaign, MarketingText.title(ad.status)).filter { it.isNotEmpty() }.joinToString(" · "), style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                            Text(
                                listOf(ad.spendLabel, "${MarketingText.count(ad.impressions)} impressions", "${MarketingText.count(ad.clicks)} clicks", if (ad.pacing.isEmpty()) "" else MarketingText.title(ad.pacing)).filter { it.isNotEmpty() }.joinToString(" · "),
                                style = Fm.text.labelSmall, color = if (ad.needsAttention) MarketingTint.orange else Fm.colors.textTertiary,
                            )
                            if (ad.linkChecks.isNotEmpty()) Text(ad.linkChecks.joinToString(" "), style = Fm.text.labelSmall, color = MarketingTint.orange)
                        }
                    },
                    trailing = if (model.isWorking(ad.id)) ({ FmCircularProgressIndicator() }) else null,
                    onTap = { navigator.push(MarketingFormScreen(model, MarketingDraft.of(MarketingKind.AD, overview, ad.json, ad.id, model.zone))) },
                    onLongPress = { menu = true },
                )
                MarketingStatusMenu(MarketingKind.AD, ad.status, menu, { menu = false }, { next -> runner.run({ setStatus(MarketingKind.AD, ad.id, next) }) }) {
                    FmDropdownMenuItem("Log results", onClick = { menu = false; navigator.push(MarketingFormScreen(model, MarketingDraft.of(MarketingKind.AD_RESULTS, overview, ad.json, ad.id, model.zone))) }, icon = "query_stats")
                    if (ad.consoleUrl.startsWith("https://")) FmDropdownMenuItem("Open the ads manager", onClick = { menu = false; crmOpen(context, ad.consoleUrl) }, icon = "open_in_new")
                    FmDropdownMenuItem("Delete", onClick = { menu = false; runner.run({ delete(MarketingKind.AD, ad.id) }) }, icon = "delete", textColor = Fm.colors.danger)
                }
            }
        }
        MarketingNote("Long-press an ad to log results, pause or resume it, or open its ads manager.")
    }
}

private fun LazyListScope.growthSection(model: MarketingModel, overview: MarketingOverview, runner: MarketingRunner, navigator: Navigator) {
    item {
        MarketingHeader("Funnel")
        if (overview.funnelReport.bottleneckReason.isNotEmpty()) MarketingNote(overview.funnelReport.bottleneckReason)
        overview.funnelReport.rates.forEach { rate ->
            FmListRow(rate.label, subtitle = rate.text, titleColor = if (rate.id == overview.funnelReport.bottleneck || rate.belowTarget) MarketingTint.orange else null, showChevron = false)
        }
        Row(Modifier.padding(horizontal = 16.dp, vertical = 4.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            model.newDraft(MarketingKind.FUNNEL_WEEK)?.let { draft -> FmToneButton("Log a week", onPressed = { navigator.push(MarketingFormScreen(model, draft)) }, icon = "add") }
            if (overview.canManageSettings) model.newDraft(MarketingKind.TARGETS)?.let { draft -> FmToneButton("Targets", onPressed = { navigator.push(MarketingFormScreen(model, draft)) }, icon = "track_changes") }
        }
    }
    if (overview.funnel.isNotEmpty()) {
        item { MarketingHeader("Weeks", overview.funnel.size) }
        items(overview.funnel, key = { "w-" + it.id }) { week ->
            MarketingRecordRow(week, overview, model.isWorking(week.id), onTap = {
                navigator.push(MarketingFormScreen(model, MarketingDraft.of(MarketingKind.FUNNEL_WEEK, overview, week.json, week.id, model.zone)))
            })
        }
    }
    item {
        MarketingHeader("Experiments", overview.experiments.size)
        MarketingNote("${overview.experimentsRunning} running of ${overview.experimentsRunningLimit} at a time. Score ideas by impact, confidence and ease.")
        if (overview.experiments.isEmpty()) MarketingNote("No experiments yet.")
    }
    items(overview.experiments, key = { "e-" + it.id }) { experiment ->
        var menu by remember { mutableStateOf(false) }
        Box {
            MarketingRecordRow(experiment, overview, model.isWorking(experiment.id), onTap = {
                navigator.push(MarketingFormScreen(model, MarketingDraft.of(MarketingKind.EXPERIMENT, overview, experiment.json, experiment.id, model.zone)))
            }, onLongPress = { menu = true })
            MarketingStatusMenu(MarketingKind.EXPERIMENT, experiment.status, menu, { menu = false }, { status -> runner.run({ setStatus(MarketingKind.EXPERIMENT, experiment.id, status) }) })
        }
    }
    item { MarketingHeader("Keywords", overview.keywords.size) }
    items(overview.keywords, key = { "k-" + it.id }) { keyword ->
        MarketingRecordRow(keyword, overview, model.isWorking(keyword.id), onTap = {
            navigator.push(MarketingFormScreen(model, MarketingDraft.of(MarketingKind.KEYWORD, overview, keyword.json, keyword.id, model.zone)))
        })
    }
    item {
        Row(Modifier.padding(horizontal = 16.dp, vertical = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            model.newDraft(MarketingKind.KEYWORD)?.let { draft -> FmToneButton("Add keyword", onPressed = { navigator.push(MarketingFormScreen(model, draft)) }, icon = "add") }
            FmToneButton("Import", onPressed = { navigator.push(MarketingKeywordImportScreen(model)) }, icon = "download")
            FmToneButton("Tagged link", onPressed = { navigator.push(MarketingLinkScreen(model)) }, icon = "link")
        }
    }
}

private fun LazyListScope.redditSection(model: MarketingModel, overview: MarketingOverview, runner: MarketingRunner, navigator: Navigator) {
    item {
        MarketingHeader("Reddit desk")
        FmKeyValueRow("To answer", overview.redditSummary.int("open").toString(), Modifier.padding(horizontal = 16.dp))
        FmKeyValueRow("Replied this week", overview.redditSummary.int("repliedThisWeek").toString(), Modifier.padding(horizontal = 16.dp))
        FmKeyValueRow("Skipped this week", overview.redditSummary.int("skippedThisWeek").toString(), Modifier.padding(horizontal = 16.dp))
        FmKeyValueRow("Monitored", overview.redditSummary.int("monitored").toString(), Modifier.padding(horizontal = 16.dp))
        MarketingNote("Read each subreddit's rules, disclose who you work for, and link with the tagged landing link.")
        MarketingHeader("Watch")
        val context = LocalContext.current
        overview.redditSubreddits.forEach { subreddit ->
            FmListRow(
                "r/" + subreddit.name, subtitle = if (subreddit.rulesRead) "Rules on file" + if (subreddit.rules.isEmpty()) "" else ": " + subreddit.rules.joinToString(" · ") else "Rules not read yet",
                trailing = { FmToneButton("Rules", onPressed = { crmOpen(context, subreddit.rulesUrl) }) },
                onTap = { crmOpen(context, subreddit.url) },
            )
        }
        overview.redditQueries.forEach { query -> FmListRow(query.name, leading = { FmIcon("search") }, onTap = { crmOpen(context, query.url) }) }
        MarketingHeader("Your accounts", overview.redditAccounts.size)
        if (overview.redditAccounts.isEmpty()) MarketingNote("No Reddit accounts yet.")
        overview.redditAccounts.forEach { account ->
            FmListRow(
                "u/" + account.name, leading = { FmIcon("account_circle") },
                onTap = { crmOpen(context, account.url.ifEmpty { "https://www.reddit.com/user/${account.name}/submitted/" }) },
            )
        }
        if (overview.canManageSettings) model.newDraft(MarketingKind.REDDIT_WATCH)?.let { draft ->
            FmToneButton("Edit the watch list and accounts", onPressed = { navigator.push(MarketingFormScreen(model, draft)) }, Modifier.padding(horizontal = 16.dp, vertical = 4.dp), icon = "edit")
        }
    }
    MarketingKind.REDDIT_THREAD.statuses.forEach { status ->
        val threads = overview.redditThreads.filter { it.status == status }
        if (threads.isNotEmpty()) {
            item { MarketingHeader(MarketingText.title(status), threads.size) }
            items(threads, key = { "r-" + it.id }) { thread ->
                var menu by remember { mutableStateOf(false) }
                val context = LocalContext.current
                Column {
                    Box {
                        MarketingRecordRow(thread, overview, model.isWorking(thread.id), onTap = {
                            navigator.push(MarketingFormScreen(model, MarketingDraft.of(MarketingKind.REDDIT_THREAD, overview, thread.json, thread.id, model.zone)))
                        }, onLongPress = { menu = true }, activity = thread.redditActivity(System.currentTimeMillis()))
                        MarketingStatusMenu(MarketingKind.REDDIT_THREAD, thread.status, menu, { menu = false }, { next -> runner.run({ setStatus(MarketingKind.REDDIT_THREAD, thread.id, next) }) }) {
                            FmDropdownMenuItem("Open thread", onClick = { menu = false; crmOpen(context, thread.json.str("url")) }, icon = "open_in_new")
                            if (thread.json.str("taggedLink").isNotEmpty()) FmDropdownMenuItem("Copy tagged link", onClick = { menu = false; crmCopy(context, thread.json.str("taggedLink")) }, icon = "content_copy")
                            FmDropdownMenuItem("Delete", onClick = { menu = false; runner.run({ delete(MarketingKind.REDDIT_THREAD, thread.id) }) }, icon = "delete", textColor = Fm.colors.danger)
                        }
                    }
                    FmToneButton("Copy ChatGPT reply prompt", onPressed = { copyToClipboard(context, thread.redditReplyPrompt) }, Modifier.padding(start = 16.dp, bottom = 8.dp), icon = "content_copy")
                }
            }
        }
    }
    if (overview.redditThreads.isEmpty()) item { FmEmptyState("forum", "No threads logged", message = "Log a thread worth answering from the watch list.") }
}

@Composable
private fun MarketingPerformancePane(model: MarketingModel, overview: MarketingOverview, runner: MarketingRunner, navigator: Navigator) {
    var days by remember { mutableIntStateOf(model.performanceDays) }
    LaunchedEffect(days, model.organization) {
        model.performanceDays = days
        runner.run({ loadPerformance() })
    }
    val performance = model.performance
    Column {
        Row(Modifier.padding(horizontal = 16.dp, vertical = 8.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            listOf(7, 28, 90).forEach { window -> CrmChip("$window days", Fm.colors.accent, filled = days == window, onTap = { days = window }) }
        }
        if (performance != null && performance.projects.size > 1) {
            var projectMenu by remember { mutableStateOf(false) }
            Box {
                FmListRow(performance.projects.firstOrNull { it.id == performance.project }?.name ?: "Analytics project", subtitle = "Analytics project", onTap = { projectMenu = true })
                FmDropdownMenu(expanded = projectMenu, onDismissRequest = { projectMenu = false }) {
                    performance.projects.forEach { project ->
                        FmDropdownMenuItem(project.name, onClick = { projectMenu = false; model.performanceProject = project.id; runner.run({ loadPerformance() }) })
                    }
                }
            }
        }
        if (overview.canManageSettings) model.newDraft(MarketingKind.GOALS)?.let { draft ->
            FmToneButton("Conversion goals", onPressed = { navigator.push(MarketingFormScreen(model, draft)) }, Modifier.padding(horizontal = 16.dp), icon = "flag")
        }
        if (performance == null) {
            Box(Modifier.fillMaxWidth().padding(24.dp), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
            return@Column
        }
        if (performance.ok) {
            MarketingHeader("Visits from campaigns")
            FmKeyValueRow("Visits", MarketingText.count(performance.sessions), Modifier.padding(horizontal = 16.dp))
            FmKeyValueRow("Change", MarketingText.signed(performance.sessionsChange), Modifier.padding(horizontal = 16.dp))
            performance.conversions?.let { FmKeyValueRow("Conversions", MarketingText.count(it), Modifier.padding(horizontal = 16.dp)) }
        }
        if (performance.insights.isNotEmpty()) {
            MarketingHeader("Insights")
            performance.insights.forEach { MarketingInsightRow(it.level, it.title, it.detail) }
        }
        PerformanceRows("Campaigns", performance.campaigns)
        PerformanceRows("Channels", performance.channels)
        PerformanceRows("Landing pages", performance.landing)
        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun PerformanceRows(title: String, rows: List<MarketingPerformanceRow>) {
    if (rows.isEmpty()) return
    MarketingHeader(title, rows.size)
    rows.take(25).forEach { row -> FmListRow(row.label, subtitle = row.facts, showChevron = false) }
}

@Suppress("FunctionName")
fun MarketingScreen(section: MarketingSection): Screen = MarketingScreenImpl(section)
