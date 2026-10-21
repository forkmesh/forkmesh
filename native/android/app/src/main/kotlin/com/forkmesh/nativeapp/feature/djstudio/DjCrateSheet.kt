package com.forkmesh.nativeapp.feature.djstudio

import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.FileProvider
import com.forkmesh.nativeapp.djstudio.DjCrate
import com.forkmesh.nativeapp.djstudio.DjDeckSide
import com.forkmesh.nativeapp.djstudio.DjFocus
import com.forkmesh.nativeapp.djstudio.DjSmartKind
import com.forkmesh.nativeapp.djstudio.DjStudio
import com.forkmesh.nativeapp.djstudio.DjTrack
import com.forkmesh.nativeapp.ui.kit.FmActionMenu
import com.forkmesh.nativeapp.ui.kit.FmActionMenuButton
import com.forkmesh.nativeapp.ui.kit.FmCheckbox
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import java.io.File

private sealed interface CratePage {
    data object Root : CratePage
    data class Playlist(val id: String) : CratePage
    data class Tag(val name: String) : CratePage
    data class Smart(val id: String) : CratePage
    data class Kind(val kind: DjSmartKind) : CratePage
    data object All : CratePage
    data class Picker(val id: String) : CratePage
}

@Composable
internal fun DjCrateSheet(model: DjStudioModel) {
    var stack by remember { mutableStateOf(listOf<CratePage>(CratePage.Root)) }
    val page = stack.last()
    val push = { next: CratePage -> stack = stack + next }
    val pop = { stack = stack.dropLast(1).ifEmpty { listOf(CratePage.Root) } }
    if (page != CratePage.Root) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp).djPress("Back", onTap = pop), verticalAlignment = Alignment.CenterVertically) {
            FmIcon("chevron_left", size = 28.dp, tint = Fm.colors.accent)
            Text("Back", color = Fm.colors.accent, fontSize = 16.sp)
        }
    }
    when (page) {
        CratePage.Root -> CrateRoot(model, push)
        is CratePage.Playlist -> PlaylistPage(model, page.id, push, pop)
        is CratePage.Tag -> TagPage(model, page.name, pop)
        is CratePage.Smart -> SmartPage(model, page.id)
        is CratePage.Kind -> KindPage(model, page.kind, push)
        CratePage.All -> AllSongsPage(model)
        is CratePage.Picker -> PickerPage(model, page.id)
    }
}

private fun share(context: Context, name: String, text: String, extension: String, mime: String) {
    val folder = File(context.cacheDir, "dj-share").apply { mkdirs() }
    val file = File(folder, "$name.$extension")
    file.writeText(text)
    val uri = FileProvider.getUriForFile(context, "${context.packageName}.composer.files", file)
    val intent = Intent(Intent.ACTION_SEND).setType(mime).putExtra(Intent.EXTRA_STREAM, uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    runCatching { context.startActivity(Intent.createChooser(intent, name).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
}

private fun exportJson(context: Context, model: DjStudioModel, scope: DjFocus) = share(context, model.crate.fileName(scope), model.crate.exported(scope), "json", "application/json")

private fun exportM3u(context: Context, model: DjStudioModel, scope: DjFocus) = share(context, model.crate.fileName(scope), model.crate.m3u(scope), "m3u", "audio/x-mpegurl")

private fun displayName(context: Context, uri: Uri): String = runCatching {
    context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
        if (cursor.moveToFirst()) cursor.getString(0) else null
    }
}.getOrNull()?.substringBeforeLast('.').orEmpty()

@Composable
private fun FocusActions(model: DjStudioModel, focus: DjFocus) {
    val context = LocalContext.current
    Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        FmToneButton("Play with AUTOMIX", { model.focusAutomix(focus, true) }, icon = "shuffle", tone = FmTone.Accent, filled = true)
        var more by remember { mutableStateOf(false) }
        Box {
            FmToneButton("More", { more = true }, icon = "more_horiz")
            FmActionMenu(more, { more = false }) {
                FmActionMenuButton("Add All to the Queue", { more = false; model.queueAll(focus, false) })
                FmActionMenuButton("Shuffle into the Queue", { more = false; model.queueAll(focus, true) })
                FmActionMenuButton("Export", { more = false; exportJson(context, model, focus) })
                FmActionMenuButton("Export as M3U", { more = false; exportM3u(context, model, focus) })
            }
        }
    }
}

@Composable
private fun CrateRoot(model: DjStudioModel, push: (CratePage) -> Unit) {
    val context = LocalContext.current
    val crate = model.crate
    val importer = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        val bytes = runCatching { context.contentResolver.openInputStream(uri)?.use { it.readBytes() } }.getOrNull()
        if (bytes == null || bytes.size > DjCrate.IMPORT_LIMIT) model.show("That file could not be read (4 MB at most).") else model.importData(bytes, displayName(context, uri))
    }
    var focusMenu by remember { mutableStateOf(false) }
    Column(Modifier.verticalScroll(rememberScrollState()).padding(bottom = 24.dp)) {
        DjSheetTitle("Playlists") { FmToneButton("New", { model.prompt = DjPrompt.NewPlaylist(emptyList()) }, icon = "add") }
        DjSection("AUTOMIX")
        Box {
            DjRow("Plays", crate.label(crate.focus), icon = "gps_fixed", trailing = { FmIcon("unfold_more", size = 20.dp, tint = Fm.colors.textTertiary) }) { focusMenu = true }
            FmActionMenu(focusMenu, { focusMenu = false }) {
                val pick = { focus: DjFocus -> focusMenu = false; model.focusAutomix(focus, false) }
                FmActionMenuButton("Everything", { pick(DjFocus.Everything) })
                crate.playlists.forEach { FmActionMenuButton(it.name, { pick(DjFocus.Playlist(it.id)) }) }
                crate.tagCounts.forEach { FmActionMenuButton("#${it.name}", { pick(DjFocus.Tag(it.name)) }) }
                crate.smart.forEach { FmActionMenuButton(it.name, { pick(DjFocus.Smart(it.id)) }) }
            }
        }
        if (crate.focus != DjFocus.Everything) {
            Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("Shuffle", fontSize = 15.sp, color = Fm.colors.textPrimary, modifier = Modifier.weight(1f))
                FmSwitch(crate.shuffle, { model.setShuffle(it) })
            }
        }
        DjSection("Playlists")
        if (crate.playlists.isEmpty()) DjRow("No playlists yet", "Create one with New, or file a song from the turntable.")
        crate.playlists.forEach { list ->
            DjRow(list.name, "${list.songs.size} songs", icon = "queue_music") { push(CratePage.Playlist(list.id)) }
        }
        if (crate.smart.isNotEmpty()) {
            DjSection("Smart Playlists")
            DjSmartKind.entries.forEach { kind ->
                val lists = crate.smart.filter { it.kind == kind }
                if (lists.isNotEmpty()) DjRow(kind.title, "${lists.size}", icon = lists.first().icon) { push(CratePage.Kind(kind)) }
            }
        }
        DjSection("Tags")
        crate.tagCounts.forEach { tag -> DjRow("#${tag.name}", "${tag.count} songs", icon = "sell") { push(CratePage.Tag(tag.name)) } }
        DjRow("All Songs", "${model.librarySongs.size}", icon = "library_music") { push(CratePage.All) }
        Text(
            "Tag a song from All Songs, a playlist, or by holding it in the loader. Tags group songs across playlists, such as #warm up or #chill.",
            fontSize = 12.sp, color = Fm.colors.textTertiary, modifier = Modifier.padding(horizontal = 16.dp),
        )
        DjSection("Import and Export")
        DjRow("Export Everything", icon = "ios_share") { exportJson(context, model, DjFocus.Everything) }
        DjRow("Export as M3U", icon = "playlist_play") { exportM3u(context, model, DjFocus.Everything) }
        DjRow("Import from Files", icon = "file_open") { importer.launch(arrayOf("*/*")) }
        DjRow("Paste", icon = "content_paste") {
            val clip = context.getSystemService(ClipboardManager::class.java)?.primaryClip
            val text = clip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.coerceToText(context)?.toString().orEmpty()
            if (text.isEmpty()) model.show("The clipboard is empty.") else model.importData(text.toByteArray(), "Pasted Songs")
        }
        Text(
            "Export saves your songs, tags and playlists as a file to keep or send to another phone. Import reads that file, M3U playlists and lists of YouTube links; songs already here keep their tags and playlists with the same name are combined.",
            fontSize = 12.sp, color = Fm.colors.textTertiary, modifier = Modifier.padding(horizontal = 16.dp),
        )
    }
}

@Composable
private fun TrackRow(model: DjStudioModel, track: DjTrack, detail: String = "", extra: (@Composable (() -> Unit) -> Unit)? = null) {
    var menu by remember { mutableStateOf(false) }
    Box {
        val plays = DjStudio.playsLabel(model.plays(track.id))
        val line = listOf(detail.ifEmpty { track.author }, plays, if (!model.crate.shuffles(track.id)) "Not shuffled" else "").filter { it.isNotEmpty() }.joinToString(" · ")
        DjRow(track.shownTitle, line, thumbnail = track.thumbnail, onLongPress = { menu = true }) { menu = true }
        FmActionMenu(menu, { menu = false }) {
            extra?.invoke { menu = false }
            FmActionMenuButton("Load on Deck A", { menu = false; model.loadTrack(track, DjDeckSide.A) })
            FmActionMenuButton("Load on Deck B", { menu = false; model.loadTrack(track, DjDeckSide.B) })
            FmActionMenuButton("Add to the Queue", { menu = false; model.enqueue(track.id, track.title, track.author) })
            FmActionMenuButton("Tags and Playlists…", { menu = false; model.editSong(track.id) })
            FmActionMenuButton(if (model.crate.shuffles(track.id)) "Don't Shuffle" else "Shuffle Again", { menu = false; model.toggleShuffle(track.id) })
            FmActionMenuButton("Delete from Library", { menu = false; model.removeSong(track.id) }, destructive = true)
        }
    }
}

@Composable
private fun PlaylistPage(model: DjStudioModel, id: String, push: (CratePage) -> Unit, pop: () -> Unit) {
    val list = model.crate.playlist(id) ?: run {
        pop()
        return
    }
    var menu by remember { mutableStateOf(false) }
    val context = LocalContext.current
    DjSheetTitle(list.name) {
        Box {
            FmToneButton("Edit", { menu = true }, icon = "edit")
            FmActionMenu(menu, { menu = false }) {
                FmActionMenuButton("Rename", { menu = false; model.prompt = DjPrompt.RenamePlaylist(id) })
                FmActionMenuButton("Export", { menu = false; exportJson(context, model, DjFocus.Playlist(id)) })
                FmActionMenuButton("Delete Playlist", { menu = false; model.deletePlaylist(id); pop() }, destructive = true)
            }
        }
    }
    FocusActions(model, DjFocus.Playlist(id))
    DjRow("Add Songs", icon = "playlist_add", tint = Fm.colors.accent) { push(CratePage.Picker(id)) }
    val songs = model.crate.songs(DjFocus.Playlist(id))
    LazyColumn(Modifier.fillMaxWidth().heightIn(max = 520.dp)) {
        itemsIndexed(songs, key = { _, it -> it.id }) { index, track ->
            TrackRow(model, track) { close ->
                if (index > 0) FmActionMenuButton("Move Up", { close(); model.moveInPlaylist(id, index, index - 1) })
                if (index < songs.size - 1) FmActionMenuButton("Move Down", { close(); model.moveInPlaylist(id, index, index + 2) })
                FmActionMenuButton("Remove from ${list.name}", { close(); model.removeFromPlaylist(index, id) }, destructive = true)
            }
        }
    }
}

@Composable
private fun TagPage(model: DjStudioModel, name: String, pop: () -> Unit) {
    var menu by remember { mutableStateOf(false) }
    val context = LocalContext.current
    val songs = model.crate.songs(DjFocus.Tag(name))
    if (songs.isEmpty()) {
        pop()
        return
    }
    DjSheetTitle("#$name") {
        Box {
            FmToneButton("Edit", { menu = true }, icon = "edit")
            FmActionMenu(menu, { menu = false }) {
                FmActionMenuButton("Rename", { menu = false; model.prompt = DjPrompt.RenameTag(name) })
                FmActionMenuButton("Export", { menu = false; exportJson(context, model, DjFocus.Tag(name)) })
                FmActionMenuButton("Delete Tag", { menu = false; model.deleteTag(name); pop() }, destructive = true)
            }
        }
    }
    FocusActions(model, DjFocus.Tag(name))
    LazyColumn(Modifier.fillMaxWidth().heightIn(max = 520.dp)) {
        items(songs, key = { it.id }) { track ->
            TrackRow(model, track) { close -> FmActionMenuButton("Untag", { close(); model.toggleTag(name, track.id) }, destructive = true) }
        }
    }
}

@Composable
private fun KindPage(model: DjStudioModel, kind: DjSmartKind, push: (CratePage) -> Unit) {
    DjSheetTitle(kind.title)
    Column(Modifier.verticalScroll(rememberScrollState()).padding(bottom = 24.dp)) {
        model.crate.smart.filter { it.kind == kind }.forEach { list -> DjRow(list.name, "${list.songs.size} songs", icon = list.icon) { push(CratePage.Smart(list.id)) } }
    }
}

@Composable
private fun SmartPage(model: DjStudioModel, id: String) {
    val list = model.crate.smartList(id) ?: return
    DjSheetTitle(list.name) { FmToneButton("Save as Playlist", { model.saveSmart(id) }, icon = "playlist_add") }
    FocusActions(model, DjFocus.Smart(id))
    LazyColumn(Modifier.fillMaxWidth().heightIn(max = 520.dp)) {
        items(model.crate.songs(DjFocus.Smart(id)), key = { it.id }) { TrackRow(model, it) }
    }
}

@Composable
private fun AllSongsPage(model: DjStudioModel) {
    var filter by remember { mutableStateOf("") }
    DjSheetTitle("All Songs")
    FmTextField(filter, { filter = it }, Modifier.fillMaxWidth().padding(horizontal = 16.dp), hint = "Filter by title, channel or #tag", leading = { FmIcon("search", size = 20.dp, tint = Fm.colors.textTertiary) })
    val needle = filter.trim().lowercase()
    val songs = model.librarySongs.filter { track ->
        needle.isEmpty() || if (needle.startsWith("#")) track.tags.any { it.startsWith(needle.drop(1)) } else track.title.lowercase().contains(needle) || track.author.lowercase().contains(needle)
    }
    LazyColumn(Modifier.fillMaxWidth().heightIn(max = 520.dp)) {
        items(songs, key = { it.id }) { track -> TrackRow(model, track, track.tags.joinToString(" ") { "#$it" }) }
        item {
            Text(
                "Songs you have played or saved. Removing one also takes it out of every playlist, its tags and its play count.",
                fontSize = 12.sp, color = Fm.colors.textTertiary, modifier = Modifier.padding(16.dp),
            )
        }
    }
}

@Composable
private fun PickerPage(model: DjStudioModel, id: String) {
    var filter by remember { mutableStateOf("") }
    DjSheetTitle("Add Songs")
    FmTextField(
        filter, { filter = it }, Modifier.fillMaxWidth().padding(horizontal = 16.dp), hint = "Filter, or paste a YouTube link",
        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
        keyboardActions = KeyboardActions(onDone = { if (model.addLink(filter, id)) filter = "" }),
    )
    if (DjStudio.videoId(filter).isNotEmpty()) DjRow("Add this link", icon = "add_link", tint = Fm.colors.accent) { if (model.addLink(filter, id)) filter = "" }
    val needle = filter.trim().lowercase()
    LazyColumn(Modifier.fillMaxWidth().heightIn(max = 520.dp)) {
        items(model.librarySongs.filter { needle.isEmpty() || it.title.lowercase().contains(needle) || it.author.lowercase().contains(needle) }, key = { it.id }) { track ->
            DjRow(track.shownTitle, track.author, thumbnail = track.thumbnail, trailing = { FmCheckbox(model.inPlaylist(track.id, id), { model.toggleInPlaylist(track.id, id) }) }) {
                model.toggleInPlaylist(track.id, id)
            }
        }
    }
}

@Composable
internal fun DjSongEditor(model: DjStudioModel, id: String) {
    val track = model.song(id)
    var text by remember { mutableStateOf("") }
    Column(Modifier.verticalScroll(rememberScrollState()).padding(bottom = 24.dp)) {
        DjSheetTitle("Tags and Playlists")
        DjRow(track.shownTitle, track.author, thumbnail = track.thumbnail)
        DjSection("Tags")
        Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FmTextField(
                text, { text = it }, Modifier.weight(1f), hint = "New tags, separated by commas",
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = {
                    model.addTags(text, id)
                    text = ""
                }),
            )
            FmToneButton("Add", {
                model.addTags(text, id)
                text = ""
            })
        }
        val mine = model.crate.track(id)?.tags.orEmpty()
        val all = (mine + model.crate.tagCounts.map { it.name }).distinct()
        FlowRow(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            all.forEach { name ->
                val on = name in mine
                Text(
                    "#$name", fontSize = 14.sp, color = if (on) Fm.colors.onAccent else Fm.colors.textPrimary,
                    modifier = Modifier.clip(CircleShape).background(if (on) Fm.colors.accent else Fm.colors.textPrimary.copy(alpha = 0.08f))
                        .border(0.5.dp, Fm.colors.textPrimary.copy(alpha = 0.12f), CircleShape)
                        .djPress("Tag $name ${if (on) "on" else "off"}") { model.toggleTag(name, id) }.padding(horizontal = 12.dp, vertical = 6.dp),
                )
            }
        }
        DjSection("Playlists") { FmToneButton("New", { model.prompt = DjPrompt.NewPlaylist(listOf(id)) }, icon = "add") }
        model.crate.playlists.forEach { list ->
            DjRow(list.name, "${list.songs.size} songs", icon = "queue_music", trailing = { FmCheckbox(id in list.songs, { model.toggleInPlaylist(id, list.id) }) }) { model.toggleInPlaylist(id, list.id) }
        }
        Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("Leave Out of Shuffle", fontSize = 15.sp, color = Fm.colors.textPrimary, modifier = Modifier.weight(1f))
            FmSwitch(!model.crate.shuffles(id), { model.toggleShuffle(id) })
        }
        Text("AUTOMIX and Next never pick a song left out of shuffle; it still plays in order in a playlist and from the queue.", fontSize = 12.sp, color = Fm.colors.textTertiary, modifier = Modifier.padding(horizontal = 16.dp))
        DjSection("")
        DjRow("Load on Deck A", icon = "looks_one") { model.loadTrack(track, DjDeckSide.A) }
        DjRow("Load on Deck B", icon = "looks_two") { model.loadTrack(track, DjDeckSide.B) }
        DjRow("Add to the Queue", icon = "add_to_queue") { model.enqueue(track.id, track.title, track.author) }
    }
}
