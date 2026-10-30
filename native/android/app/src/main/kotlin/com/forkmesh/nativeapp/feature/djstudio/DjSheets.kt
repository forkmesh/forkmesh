package com.forkmesh.nativeapp.feature.djstudio

import android.content.ClipboardManager
import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.djstudio.DjDeckSide
import com.forkmesh.nativeapp.djstudio.DjDrumPad
import com.forkmesh.nativeapp.djstudio.DjLoadTarget
import com.forkmesh.nativeapp.djstudio.DjPersonStatus
import com.forkmesh.nativeapp.djstudio.DjSearchResult
import com.forkmesh.nativeapp.djstudio.DjStudio
import com.forkmesh.nativeapp.shell.FmIosSlider
import com.forkmesh.nativeapp.ui.kit.FmActionMenu
import com.forkmesh.nativeapp.ui.kit.FmActionMenuButton
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmPromptDialog
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.ForkMeshTheme

@OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)
@Composable
internal fun DjSheetHost(model: DjStudioModel) {
    val sheet = model.sheet ?: return
    ForkMeshTheme(dark = true, messenger = false) {
        FmModalBottomSheet(onDismissRequest = { model.sheet = null }, isScrollControlled = sheet !is DjSheet.Pads, showDragHandle = true) {
            when (sheet) {
                is DjSheet.Load -> DjLoadSheet(model)
                is DjSheet.Deck -> DjDeckSheet(model, model.deck(sheet.side))
                DjSheet.Pads -> DjPadsSheet(model)
                DjSheet.Info -> DjInfoSheet(model)
                DjSheet.Jam -> DjJamSheet(model)
                DjSheet.Crate -> DjCrateSheet(model)
                is DjSheet.Song -> DjSongEditor(model, sheet.id)
                is DjSheet.Similar -> DjSimilarSheet(model, sheet.side)
            }
        }
    }
}

@Composable
internal fun DjPromptHost(model: DjStudioModel) {
    val prompt = model.prompt ?: return
    val (title, initial) = when (prompt) {
        is DjPrompt.NewPlaylist -> "New Playlist" to ""
        is DjPrompt.NewTag -> "New Tag" to ""
        is DjPrompt.RenamePlaylist -> "Rename Playlist" to model.crate.playlist(prompt.id)?.name.orEmpty()
        is DjPrompt.RenameTag -> "Rename Tag" to prompt.name
    }
    FmPromptDialog(
        title = title,
        onConfirm = { text ->
            model.prompt = null
            when (prompt) {
                is DjPrompt.NewPlaylist -> model.newPlaylist(text, prompt.ids)
                is DjPrompt.NewTag -> model.addTags(text, prompt.id)
                is DjPrompt.RenamePlaylist -> model.renamePlaylist(prompt.id, text)
                is DjPrompt.RenameTag -> model.renameTag(prompt.name, text)
            }
        },
        onDismissRequest = { model.prompt = null },
        hint = if (prompt is DjPrompt.NewTag) "chill, warm up" else "Name",
        initialValue = initial,
        helperText = if (prompt is DjPrompt.RenameTag) "Renaming to a tag that exists merges the two." else null,
        confirmLabel = if (prompt is DjPrompt.RenamePlaylist || prompt is DjPrompt.RenameTag) "Rename" else "Create",
    )
}

@Composable
internal fun DjSheetTitle(title: String, trailing: (@Composable () -> Unit)? = null) {
    Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(title, style = Fm.text.titleMedium, color = Fm.colors.textPrimary, modifier = Modifier.weight(1f))
        trailing?.invoke()
    }
}

@Composable
internal fun DjSection(title: String, trailing: (@Composable () -> Unit)? = null) {
    Row(Modifier.fillMaxWidth().padding(start = 16.dp, end = 8.dp, top = 14.dp, bottom = 4.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(title.uppercase(), fontSize = 12.sp, fontWeight = FontWeight.SemiBold, color = Fm.colors.textTertiary, modifier = Modifier.weight(1f))
        trailing?.invoke()
    }
}

@Composable
internal fun DjRow(
    title: String,
    detail: String = "",
    thumbnail: String? = null,
    icon: String? = null,
    trailing: (@Composable () -> Unit)? = null,
    tint: Color? = null,
    onLongPress: (() -> Unit)? = null,
    onTap: (() -> Unit)? = null,
) {
    Row(
        Modifier.fillMaxWidth().heightIn(min = 48.dp)
            .then(if (onTap != null || onLongPress != null) Modifier.djPress(title, onLongPress) { onTap?.invoke() } else Modifier)
            .padding(horizontal = 16.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        if (thumbnail != null) DjThumb(thumbnail, Modifier.size(72.dp, 40.dp).clip(RoundedCornerShape(4.dp)))
        if (icon != null) FmIcon(icon, size = 22.dp, tint = tint ?: Fm.colors.textSecondary, outlined = true)
        Column(Modifier.weight(1f)) {
            Text(title, color = tint ?: Fm.colors.textPrimary, fontSize = 15.sp, fontWeight = FontWeight.Medium, maxLines = 2, overflow = TextOverflow.Ellipsis)
            if (detail.isNotEmpty()) Text(detail, color = Fm.colors.textSecondary, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        trailing?.invoke()
    }
}

internal fun detailOf(author: String, seconds: Double, plays: Int): String =
    listOf(author, if (seconds > 0) DjStudio.clock(seconds) else "", DjStudio.playsLabel(plays)).filter { it.isNotEmpty() }.joinToString(" · ")

@Composable
internal fun DjSongMenu(model: DjStudioModel, id: String, expanded: Boolean, onDismiss: () -> Unit, forget: Boolean = false) {
    FmActionMenu(expanded, onDismiss) {
        FmActionMenuButton("Load on Deck A", { onDismiss(); model.loadTrack(model.song(id), DjDeckSide.A) })
        FmActionMenuButton("Load on Deck B", { onDismiss(); model.loadTrack(model.song(id), DjDeckSide.B) })
        FmActionMenuButton("Add to the Queue", { onDismiss(); model.song(id).let { model.enqueue(it.id, it.title, it.author) } })
        model.crate.playlists.forEach { list ->
            FmActionMenuButton((if (id in list.songs) "✓ " else "Add to ") + list.name, { onDismiss(); model.toggleInPlaylist(id, list.id) })
        }
        FmActionMenuButton("New Playlist with This Song", { onDismiss(); model.prompt = DjPrompt.NewPlaylist(listOf(id)) })
        FmActionMenuButton("Tags and Playlists…", { onDismiss(); model.editSong(id) })
        FmActionMenuButton(if (model.crate.shuffles(id)) "Don't Shuffle" else "Shuffle Again", { onDismiss(); model.toggleShuffle(id) })
        if (forget) FmActionMenuButton("Remove from Recently Played", { onDismiss(); model.forgetPlayed(id) }, destructive = true)
        FmActionMenuButton("Delete from Library", { onDismiss(); model.removeSong(id) }, destructive = true)
    }
}

@Composable
private fun DjResultRow(model: DjStudioModel, result: DjSearchResult, plays: Int, forget: Boolean = false) {
    var menu by remember { mutableStateOf(false) }
    val loaded = model.searchTarget.side?.let { model.deck(it).videoId == result.id } ?: model.shownQueue.any { it.videoId == result.id }
    Box {
        DjRow(
            result.title, detailOf(result.author, result.seconds, plays), thumbnail = result.thumbnail,
            trailing = { FmIcon(if (loaded) "check_circle" else "add_circle", size = 22.dp, tint = Fm.colors.accent) },
            onLongPress = { menu = true },
        ) {
            model.place(result.id, result.title, result.author)
            model.sheet = null
        }
        DjSongMenu(model, result.id, menu, { menu = false }, forget)
    }
}

@Composable
private fun DjLoadSheet(model: DjStudioModel) {
    val context = LocalContext.current
    val targets = DjLoadTarget.entries
    DjSheetTitle(if (model.searchTarget == DjLoadTarget.QUEUE) "Add to Queue" else "Load ${model.searchTarget.label}")
    FmSegmentTabBar(targets.map { FmSegmentTab(it.label) }, targets.indexOf(model.searchTarget), { model.searchTarget = targets[it] })
    Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        FmTextField(
            model.searchText, { model.searchText = it }, Modifier.weight(1f), hint = "Search YouTube or paste a link", autofocus = true,
            leading = { FmIcon("search", size = 20.dp, tint = Fm.colors.textTertiary) },
            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search, autoCorrectEnabled = false),
            keyboardActions = KeyboardActions(onSearch = { model.search() }),
        )
        if (model.searching) {
            FmCircularProgressIndicator(Modifier.size(24.dp))
        } else {
            DjRoundButton(36.dp, "content_paste", "Paste a link") {
                val clip = context.getSystemService(ClipboardManager::class.java)?.primaryClip
                val text = clip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.coerceToText(context)?.toString().orEmpty()
                if (text.isNotEmpty()) {
                    model.searchText = text
                    model.search()
                }
            }
        }
    }
    if (model.searchProblem.isNotEmpty()) Text(model.searchProblem, fontSize = 13.sp, color = Fm.colors.textSecondary, modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp))
    val counts = model.library.associate { it.id to it.plays }
    LazyColumn(Modifier.fillMaxWidth().heightIn(max = 560.dp)) {
        val results: LazyListScope.() -> Unit = {
            if (model.searchResults.isNotEmpty()) {
                item { DjSection("Search Results") }
                items(model.searchResults, key = { "r" + it.id }) { DjResultRow(model, it, counts[it.id] ?: 0) }
            }
        }
        val recent: LazyListScope.() -> Unit = {
            if (model.history.isNotEmpty()) {
                item { DjSection("Recently Played") { Text("Clear", color = Fm.colors.accent, fontSize = 13.sp, modifier = Modifier.clickable { model.clearHistory() }.padding(8.dp)) } }
                items(model.history, key = { "h" + it.id }) {
                    DjResultRow(model, DjSearchResult(it.id, it.title.ifEmpty { "YouTube video" }, it.author), counts[it.id] ?: 0, forget = true)
                }
            }
        }
        val most: LazyListScope.() -> Unit = {
            val top = DjStudio.mostPlayed(model.library)
            if (top.isNotEmpty()) {
                item { DjSection("Most Played") }
                items(top, key = { "m" + it.id }) { DjResultRow(model, DjSearchResult(it.id, it.title.ifEmpty { "YouTube video" }, it.author), it.plays, forget = true) }
                item {
                    Text(
                        "With the queue empty, AUTOMIX shuffles every song you have played (${model.library.size}).", fontSize = 12.sp,
                        color = Fm.colors.textTertiary, modifier = Modifier.padding(16.dp),
                    )
                }
            }
        }
        if (model.searchText.isEmpty()) {
            recent()
            most()
            results()
        } else {
            results()
            recent()
            most()
        }
    }
}

@Composable
private fun DjLabeled(title: String, content: @Composable () -> Unit) {
    Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(title, color = Fm.colors.textPrimary, fontSize = 15.sp, modifier = Modifier.weight(1f))
        content()
    }
}

@Composable
private fun DjDeckSheet(model: DjStudioModel, deck: DjDeck) {
    Column(Modifier.verticalScroll(rememberScrollState()).padding(bottom = 24.dp)) {
        DjSheetTitle("Deck ${deck.side.label}")
        if (deck.loaded) {
            DjRow(deck.title.ifEmpty { "Loading…" }, listOf(deck.author.ifEmpty { "YouTube" }, DjStudio.playsLabel(model.plays(deck.videoId))).filter { it.isNotEmpty() }.joinToString(" · "), thumbnail = DjStudio.thumbnailUrl(deck.videoId, "mqdefault"))
        } else {
            DjRow("No video loaded")
        }
        Column(Modifier.alpha(if (deck.loaded) 1f else 0.5f)) {
            DjSection("Mix")
            DjLabeled("Volume") { FmIosSlider(deck.level.toFloat(), { model.setLevel(deck.side, it.toDouble()) }, Modifier.width(200.dp), enabled = deck.loaded) }
            DjLabeled("Tempo") {
                Text("%+.1f%%".format((deck.rate - 1) * 100), color = Fm.colors.textSecondary)
                FmToneButton("Reset", { model.setRate(deck.side, 1.0) })
            }
            DjLabeled("BPM") {
                Text(if (deck.bpm > 0) "%.1f".format(deck.shownBpm) else "Not set", color = Fm.colors.textSecondary)
                FmToneButton("Tap", { model.tapTempo(deck.side) })
                FmToneButton("Detect", { model.detectBpm(deck.side) })
                if (deck.bpm > 0) FmToneButton("Clear", { model.clearBpm(deck.side) })
            }
            DjLabeled("8-second loop") { FmSwitch(deck.loop, { model.toggleLoop(deck.side) }, enabled = deck.loaded) }
            DjLabeled("Repeat song") { FmSwitch(deck.repeats, { model.toggleRepeat(deck.side) }, enabled = deck.loaded) }
            DjSection("Cue")
            DjRow("Set cue at ${DjStudio.clock(deck.position())}", icon = "flag") { model.setCue(deck.side) }
            DjRow("Jump to cue at ${DjStudio.clock(deck.cue)}", icon = "replay") { model.jumpToCue(deck.side) }
        }
        DjSection("Song")
        DjRow("Load another video", icon = "library_music") { model.openLoad(deck.side) }
        DjRow("Load the next song", icon = "skip_next") { model.quickNext(deck.side) }
        if (deck.loaded) {
            DjRow("Find similar songs", icon = "travel_explore") { model.openSimilar(deck.side) }
            DjRow("Tags and Playlists…", icon = "sell") { model.editSong(deck.videoId) }
            DjLabeled("Leave out of shuffle") { FmSwitch(!model.crate.shuffles(deck.videoId), { model.toggleShuffle(deck.videoId) }) }
            DjRow("Delete song from library", icon = "delete", tint = Fm.colors.danger) { model.deleteSong(deck.side) }
            DjRow("Eject Deck ${deck.side.label}", icon = "eject", tint = Fm.colors.danger) {
                model.eject(deck.side)
                model.sheet = null
            }
        }
    }
}

@Composable
private fun DjPad(title: String, detail: String, lit: Boolean, enabled: Boolean, modifier: Modifier, onLongPress: (() -> Unit)? = null, action: () -> Unit) {
    Column(
        modifier.heightIn(min = 54.dp).clip(RoundedCornerShape(8.dp))
            .background(if (lit) DjTheme.active.copy(alpha = 0.45f) else Color.White.copy(alpha = 0.08f))
            .border(1.dp, Color.White.copy(alpha = 0.1f), RoundedCornerShape(8.dp))
            .alpha(if (enabled) 1f else 0.45f)
            .djPress(title, onLongPress) { if (enabled) action() },
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text(title, color = Fm.colors.textPrimary, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, maxLines = 1)
        if (detail.isNotEmpty()) Text(detail, color = Fm.colors.textSecondary, fontSize = 11.sp, maxLines = 1)
    }
}

@Composable
private fun DjPadsSheet(model: DjStudioModel) {
    var clearing by remember { mutableStateOf<Int?>(null) }
    Column(Modifier.padding(horizontal = 14.dp).padding(bottom = 24.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        DjSheetTitle("Sampler")
        DjDrumPad.entries.chunked(4).forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                row.forEach { pad -> DjPad(pad.label, "", model.flashedPad == pad, true, Modifier.weight(1f)) { model.playPad(pad) } }
            }
        }
        Text("Samples", fontSize = 13.sp, fontWeight = FontWeight.SemiBold, color = Fm.colors.textSecondary)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            for (index in 0 until DjStudio.SECTION_COUNT) {
                val section = model.sections[index]
                Box(Modifier.weight(1f)) {
                    DjPad(
                        "Sample ${index + 1}", section?.let { "${it.deck.label} · ${DjStudio.clock(it.start)}" } ?: "Empty", false, section != null,
                        Modifier.fillMaxWidth(), onLongPress = if (section != null) ({ clearing = index }) else null,
                    ) { model.playSection(index) }
                    FmActionMenu(clearing == index, { clearing = null }) {
                        FmActionMenuButton("Clear Sample ${index + 1}", { clearing = null; model.clearSection(index) }, destructive = true)
                    }
                }
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            FmIcon("volume_mute", size = 20.dp, tint = Fm.colors.textSecondary)
            FmIosSlider(model.samplerLevel.toFloat(), { model.changeSamplerLevel(it.toDouble()) }, Modifier.weight(1f))
            FmIcon("volume_up", size = 20.dp, tint = Fm.colors.textSecondary)
        }
    }
}

@Composable
private fun DjInfoSheet(model: DjStudioModel) {
    val context = LocalContext.current
    Column(Modifier.verticalScroll(rememberScrollState()).padding(bottom = 24.dp)) {
        DjSheetTitle("Video Mix DJ Studio")
        for (deck in listOf(model.a, model.b)) {
            DjSection("Deck ${deck.side.label}")
            if (deck.loaded) {
                DjRow(deck.title.ifEmpty { "Loading…" }, listOf(deck.author.ifEmpty { "YouTube" }, DjStudio.playsLabel(model.plays(deck.videoId))).filter { it.isNotEmpty() }.joinToString(" · "))
                DjRow("Open on YouTube", icon = "open_in_new", tint = Fm.colors.accent) {
                    runCatching { context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(DjStudio.watchUrl(deck.videoId)))) }
                }
            } else {
                DjRow("Load a video", icon = "library_music", tint = Fm.colors.accent) { model.openLoad(deck.side) }
            }
        }
        DjSection("How it works")
        val tips = listOf(
            "search" to "Search and press return: the top result loads straight onto the deck.",
            "history" to "Songs you play are kept under Recently Played and Most Played in the loader with how often each was played. With the queue empty, AUTOMIX shuffles them.",
            "touch_app" to "Touch a record to slow it a little; drag it to scratch.",
            "play_arrow" to "START plays, PAUSE holds, STOP goes back to the beginning, RESTART starts over.",
            "fiber_manual_record" to "SAMPLE starts recording, tap again to stop and arm it, and once more to play the sample.",
            "view_carousel" to "The middle shows the queue: + adds a video, tap a thumbnail to load it on a deck, or hold it and drag it onto a deck.",
            "volume_up" to "Each deck's volume fader sits beside its tempo strip; double-tap it for full volume.",
            "group" to "Jam (the people button) invites other ForkMesh users to share the decks and the queue. Everyone plays their own synchronised copy, and their icons show who is listening and shake when they dance.",
            "mood" to "The smiley button opens 30 silent reactions that play over the studio for everyone in the jam.",
            "tune" to "Push the tempo fader up to speed a deck up; RESET under it (or a double-tap) puts it back in the middle.",
            "speed" to "Each song's BPM is found from its audio and shown above SYNC; tap the box on the beat to set it yourself.",
            "exposure" to "Hold − or + to nudge a deck's tempo.",
            "compare_arrows" to "AUTOMIX on plays the queue, then songs you have played (or the playlist or tag chosen in Playlists), one after another with an 8-second crossfade; hold it to mix to the other deck now.",
            "vibration" to "Shake the phone to mix into the next song.",
            "queue_music" to "Playlists (the list button) keeps playlists and tagged songs, exports and imports them, and chooses what AUTOMIX plays.",
            "chevron_left" to "Back closes the studio; playing decks keep going, with controls in the notification.",
            "restart_alt" to "Reset All (the circular arrow at the bottom) unloads both decks and puts every setting back to the start.",
        )
        for ((icon, text) in tips) {
            Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                FmIcon(icon, size = 20.dp, tint = Fm.colors.textSecondary, outlined = true)
                Text(text, fontSize = 13.sp, color = Fm.colors.textPrimary)
            }
        }
        DjSection("")
        DjRow("Reset All", icon = "restart_alt", tint = Fm.colors.danger) {
            model.sheet = null
            model.confirmingReset = true
        }
        DjRow("Leave Studio", icon = "logout", tint = Fm.colors.danger) {
            model.sheet = null
            model.leave()
        }
    }
}

@Composable
private fun DjJamSheet(model: DjStudioModel) {
    val jam = model.jam
    Column(Modifier.verticalScroll(rememberScrollState()).padding(bottom = 24.dp)) {
        DjSheetTitle(if (jam?.joined == true) jam.title.ifEmpty { "Jam Session" } else "Jam Session") {
            if (model.jamBusy) FmCircularProgressIndicator(Modifier.size(20.dp))
        }
        if (model.jamProblem.isNotEmpty()) Text(model.jamProblem, color = Fm.colors.danger, fontSize = 13.sp, modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
        if (jam == null || !jam.joined) {
            Text(
                "Start a jam to share both decks and the queue with other ForkMesh users. Everyone plays their own copy in sync; volume, levels and the crossfader stay on each phone.",
                fontSize = 13.sp, color = Fm.colors.textSecondary, modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
            )
            DjRow("Start a Jam Session", icon = "group_add", tint = Fm.colors.accent) { model.startJam() }
            val invites = model.jams.filter { it.invited }
            if (invites.isNotEmpty()) {
                DjSection("Invitations")
                invites.forEach { invite ->
                    DjRow(invite.title.ifEmpty { "Jam" }, "From ${invite.owner} · ${invite.members} people", icon = "mail", trailing = { FmToneButton("Join", { model.joinJam(invite.id) }, tone = FmTone.Accent, filled = true) })
                }
            }
            val mine = model.jams.filter { !it.invited }
            if (mine.isNotEmpty()) {
                DjSection("Your Jams")
                mine.forEach { summary -> DjRow(summary.title.ifEmpty { "Jam" }, "${summary.members} people", icon = "group") { model.openJam(summary.id) } }
            }
            return@Column
        }
        DjSection("Invite")
        Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FmTextField(
                model.inviteText, {
                    model.inviteText = it
                    model.suggestPeople(it)
                }, Modifier.weight(1f), hint = "ForkMesh user names, separated by commas",
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send, autoCorrectEnabled = false),
                keyboardActions = KeyboardActions(onSend = { model.invite(model.inviteText.split(',').map { it.trim() }.filter { it.isNotEmpty() }) }),
            )
            FmToneButton("Invite", { model.invite(model.inviteText.split(',').map { it.trim() }.filter { it.isNotEmpty() }) }, tone = FmTone.Accent, filled = true)
        }
        model.inviteSuggestions.forEach { name -> DjRow("@$name", icon = "person_add") { model.invite(listOf(name)) } }
        DjSection("People")
        val clock = rememberFrameClock()
        jam.members.forEach { member ->
            val status = model.status(member)
            Row(Modifier.fillMaxWidth().heightIn(min = 52.dp).padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                DjPerson(model, member, 32.dp, clock)
                Column(Modifier.weight(1f)) {
                    Text(member.name + if (member.name == jam.me) " (you)" else "", color = Fm.colors.textPrimary, fontSize = 15.sp)
                    val dancing = model.crowd.firstOrNull { it.name == member.name }?.dancing == true
                    Text(status.label + if (dancing && status.live) " · dancing" else "", color = Fm.colors.textSecondary, fontSize = 12.sp)
                }
                if (jam.isOwner && member.name != jam.me) FmToneButton("Remove", { model.removeMember(member.name) }, tone = FmTone.Danger)
            }
        }
        DjLabeled("Share My Phone's Motion") { FmSwitch(model.shareMotion, { model.setShareMotion(it) }) }
        Text("Your phone's movement makes your icon dance for the others. No location is ever sent.", fontSize = 12.sp, color = Fm.colors.textTertiary, modifier = Modifier.padding(horizontal = 16.dp))
        Spacer(Modifier.size(12.dp))
        DjRow(if (jam.isOwner) "End the Jam for Everyone" else "Leave the Jam", icon = "logout", tint = Fm.colors.danger) { model.leaveJam() }
    }
}

@Composable
private fun DjSimilarSheet(model: DjStudioModel, side: DjDeckSide) {
    val targets = DjLoadTarget.entries
    val library = model.similarSongs(side)
    DjSheetTitle("Like ${model.deck(side).title.ifEmpty { "this song" }}")
    FmSegmentTabBar(targets.map { FmSegmentTab(it.label) }, targets.indexOf(model.searchTarget), { model.searchTarget = targets[it] })
    LazyColumn(Modifier.fillMaxWidth().heightIn(max = 560.dp)) {
        if (library.isNotEmpty()) {
            item { DjSection("From Your Library") }
            items(library, key = { "l" + it.id }) { song ->
                var menu by remember { mutableStateOf(false) }
                Box {
                    DjRow(song.track.shownTitle, song.reasons.joinToString(" · "), thumbnail = song.track.thumbnail, onLongPress = { menu = true }) {
                        model.place(song.id, song.track.title, song.track.author)
                        model.sheet = null
                    }
                    DjSongMenu(model, song.id, menu, { menu = false })
                }
            }
        }
        item { DjSection("Suggested on YouTube") }
        if (model.similarLoading) item { Box(Modifier.fillMaxWidth().padding(16.dp), contentAlignment = Alignment.Center) { FmCircularProgressIndicator(Modifier.size(24.dp)) } }
        if (model.similarProblem.isNotEmpty()) item { Text(model.similarProblem, fontSize = 13.sp, color = Fm.colors.textSecondary, modifier = Modifier.padding(16.dp)) }
        val loaded = setOf(model.a.videoId, model.b.videoId)
        items(model.similarResults.filter { it.id !in loaded }, key = { "y" + it.id }) { result -> DjResultRow(model, result, model.plays(result.id)) }
    }
}

