package com.forkmesh.nativeapp.feature.tools

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.util.Base64
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.DatePicker
import androidx.compose.material3.DatePickerDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.material3.TimePicker
import androidx.compose.material3.rememberDatePickerState
import androidx.compose.material3.rememberTimePickerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.calories.Calories
import com.forkmesh.nativeapp.calories.CaloriesApi
import com.forkmesh.nativeapp.calories.CaloriesPrompt
import com.forkmesh.nativeapp.calories.FoodSearch
import com.forkmesh.nativeapp.calories.FoodSearchStep
import com.forkmesh.nativeapp.composer.PhotoPrep
import com.forkmesh.nativeapp.composer.ShellPromptError
import com.forkmesh.nativeapp.composer.ShellPromptTarget
import com.forkmesh.nativeapp.composer.ShellPromptTargets
import com.forkmesh.nativeapp.calories.GoalsDraft
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFilledButton
import com.forkmesh.nativeapp.feature.account.AccountOutlinedButton
import com.forkmesh.nativeapp.feature.composer.ComposerFeature
import com.forkmesh.nativeapp.feature.composer.PhotoIntake
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.notes.JsonObject
import com.forkmesh.nativeapp.notes.jsonObject
import com.forkmesh.nativeapp.notes.jsonObjects
import com.forkmesh.nativeapp.notes.jsonString
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCheckbox
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmSoundOnShow
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.kit.fmTapSounds
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZoneId
import java.time.ZoneOffset
import java.util.TimeZone

private fun tzOffsetMinutes(): Int = -TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60_000

private fun imageDataUrl(context: Context, uri: Uri): String {
    val bytes = context.contentResolver.openInputStream(uri)?.use { it.readBytes() } ?: error("unreadable")
    val type = context.contentResolver.getType(uri).orEmpty()
    val kind = when (type) {
        "image/jpeg", "image/jpg" -> "jpeg"
        "image/png" -> "png"
        "image/webp" -> "webp"
        else -> ""
    }
    if (kind.isNotEmpty() && bytes.size <= 1_100_000) return "data:image/$kind;base64," + Base64.encodeToString(bytes, Base64.NO_WRAP)
    val original = BitmapFactory.decodeByteArray(bytes, 0, bytes.size) ?: error("unreadable")
    for (width in listOf(800, 560, 400)) {
        val height = (original.height.toLong() * width / original.width).toInt().coerceAtLeast(1)
        val scaled = Bitmap.createScaledBitmap(original, width, height, true)
        val out = ByteArrayOutputStream()
        scaled.compress(Bitmap.CompressFormat.PNG, 100, out)
        if (out.size() <= 1_100_000) return "data:image/png;base64," + Base64.encodeToString(out.toByteArray(), Base64.NO_WRAP)
    }
    error("photo_too_large")
}

private fun caloriesJpegDataUrl(jpeg: ByteArray, maxSide: Int): String {
    val original = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size)
        ?: throw ShellPromptError("That file is not a photo this app can read. Try a JPEG or PNG.")
    try {
        for ((side, quality) in CaloriesPrompt.jpegLadder(maxSide)) {
            val longest = maxOf(original.width, original.height)
            val scaled = if (longest > side) {
                Bitmap.createScaledBitmap(
                    original,
                    maxOf(1, Math.round(original.width.toDouble() * side / longest).toInt()),
                    maxOf(1, Math.round(original.height.toDouble() * side / longest).toInt()),
                    true,
                )
            } else {
                original
            }
            val out = ByteArrayOutputStream()
            scaled.compress(Bitmap.CompressFormat.JPEG, quality, out)
            if (scaled !== original) scaled.recycle()
            val encoded = Base64.encodeToString(PhotoPrep.stripJpegMetadata(out.toByteArray()), Base64.NO_WRAP)
            CaloriesPrompt.jpegDataUrl(encoded)?.let { return it }
        }
    } finally {
        original.recycle()
    }
    throw ShellPromptError("That photo is too large to send.")
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun DayPickerDialog(day: String, maxDaysAhead: Long, onDismiss: () -> Unit, onPicked: (String) -> Unit) {
    val initial = Calories.parseDay(day).atStartOfDay(ZoneOffset.UTC).toInstant().toEpochMilli()
    val state = rememberDatePickerState(initialSelectedDateMillis = initial)
    FmSoundOnShow()
    DatePickerDialog(
        onDismissRequest = onDismiss,
        modifier = Modifier.fmTapSounds(),
        confirmButton = {
            FmTextButton(onClick = {
                val millis = state.selectedDateMillis
                if (millis != null) {
                    val picked = Instant.ofEpochMilli(millis).atZone(ZoneOffset.UTC).toLocalDate()
                    val latest = LocalDate.now().plusDays(maxDaysAhead)
                    onPicked(Calories.isoDay(if (picked.isAfter(latest)) latest else picked))
                } else {
                    onDismiss()
                }
            }) { Text("OK") }
        },
        dismissButton = { FmTextButton(onClick = onDismiss) { Text("Cancel") } },
    ) { DatePicker(state) }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TimePickerDialog(hour: Int, minute: Int, onDismiss: () -> Unit, onPicked: (Int, Int) -> Unit) {
    val state = rememberTimePickerState(initialHour = hour, initialMinute = minute)
    FmAlertDialog(
        onDismissRequest = onDismiss,
        actions = {
            FmTextButton(onClick = onDismiss) { Text("Cancel") }
            FmTextButton(onClick = { onPicked(state.hour, state.minute) }) { Text("OK") }
        },
    ) { TimePicker(state) }
}

@Composable
private fun PromptDialog(title: String, hint: String, lines: Int = 1, number: Boolean = false, allowEmpty: Boolean = false, suffix: String? = null, initial: String = "", extra: (@Composable () -> Unit)? = null, onDismiss: () -> Unit, onDone: (String) -> Unit) {
    var text by remember { mutableStateOf(initial) }
    FmAlertDialog(
        onDismissRequest = onDismiss,
        title = title,
        actions = {
            extra?.invoke()
            FmTextButton(onClick = onDismiss) { Text("Cancel") }
            FmTextButton(onClick = {
                val trimmed = text.trim()
                if (trimmed.isEmpty() && !allowEmpty) onDismiss() else onDone(trimmed)
            }) { Text(if (suffix != null) "Save" else "OK") }
        },
    ) {
        FmTextField(
            value = text,
            onValueChange = { text = it },
            hint = hint,
            minLines = lines,
            maxLines = lines.coerceAtLeast(1),
            autofocus = true,
            keyboardOptions = KeyboardOptions(keyboardType = if (number) KeyboardType.Decimal else KeyboardType.Text),
            trailing = suffix?.let { { Text(it, modifier = Modifier.padding(end = 12.dp)) } },
        )
    }
}

class CaloriesScreen : Screen() {
    override val title: String get() = "Calorie Counter"

    private var overview by mutableStateOf<JsonObject?>(null)
    private var installed by mutableStateOf(false)
    private var loading by mutableStateOf(true)
    private var error by mutableStateOf("")
    private var day by mutableStateOf(Calories.isoDay(LocalDate.now()))
    private var tab by mutableIntStateOf(0)
    private var generation by mutableIntStateOf(0)
    private var sheet by mutableStateOf<String?>(null)
    private var editingEntry by mutableStateOf<JsonObject?>(null)
    private var suggestions by mutableStateOf<JsonObject?>(null)
    private var product by mutableStateOf<Pair<JsonObject, String>?>(null)
    private var board by mutableStateOf<JsonObject?>(null)
    private var photoUri by mutableStateOf<Uri?>(null)

    private val profile: JsonObject get() = jsonObject(overview?.get("profile"))
    private val imperial: Boolean get() = profile["units"] == "imperial"
    private val weightUnit: String get() = if (imperial) "lb" else "kg"
    private fun displayWeight(kg: Double) = if (imperial) kg / Calories.KG_PER_LB else kg
    private fun toKg(shown: Double) = if (imperial) shown * Calories.KG_PER_LB else shown
    private fun weightText(kg: Any?) = if (Calories.num(kg) > 0) "${Calories.grams(displayWeight(Calories.num(kg)))} $weightUnit" else "—"

    private val myFoods: List<JsonObject> get() = jsonObjects(overview?.get("quickFoods"))
    private val commonFoods: List<JsonObject> get() = jsonObjects(overview?.get("commonFoods"))
    private val frequentFoods: List<JsonObject> get() = CaloriesPrompt.frequent(jsonObjects(overview?.get("entries")), myFoods)
    private val todayDay: String get() = Calories.isoDay(LocalDate.now())

    private suspend fun refresh(api: CaloriesApi) {
        val data = withContext(Dispatchers.IO) { api.overview(todayDay) }
        installed = data["installed"] == true
        overview = if (installed) data else null
    }

    private fun setMyFoods(foods: Any?) {
        val current = overview ?: return
        overview = current + ("quickFoods" to jsonObjects(foods))
    }

    private suspend fun postEntries(api: CaloriesApi, items: List<JsonObject>, meal: String, source: String, logDay: String) {
        withContext(Dispatchers.IO) {
            for (item in items) api.call("POST", "/entries", item + mapOf("day" to logDay, "meal" to meal, "source" to source))
        }
    }

    private suspend fun logItems(api: CaloriesApi, items: List<JsonObject>, meal: String, source: String, logDay: String): String {
        postEntries(api, items, meal, source, logDay)
        runCatching { refresh(api) }
        return CaloriesPrompt.loggedMessage(
            items,
            meal,
            logDay,
            todayDay,
            Calories.int(profile["dailyCalories"]),
            Calories.num(jsonObject(overview?.get("todayTotals"))["calories"]),
        )
    }

    private suspend fun jpeg(bytes: ByteArray, side: Int): String = withContext(Dispatchers.Default) { caloriesJpegDataUrl(bytes, side) }

    private suspend fun estimate(api: CaloriesApi, body: JsonObject): JsonObject = try {
        withContext(Dispatchers.IO) { api.call("POST", "/analyze", body) }
    } catch (thrown: CancellationException) {
        throw thrown
    } catch (thrown: Exception) {
        throw ShellPromptError(CaloriesPrompt.estimateError(thrown))
    }

    private suspend fun readReceipt(api: CaloriesApi, bytes: ByteArray): String {
        val image = jpeg(bytes, CaloriesPrompt.RECEIPT_SIDE)
        return try {
            val payload = withContext(Dispatchers.IO) { api.call("POST", "/receipt", mapOf("image" to image)) }
            setMyFoods(payload["quickFoods"])
            CaloriesPrompt.receiptMessage(payload)
        } catch (thrown: CancellationException) {
            throw thrown
        } catch (thrown: Exception) {
            throw ShellPromptError(CaloriesPrompt.receiptError(thrown))
        }
    }

    private suspend fun saveMyFoods(api: CaloriesApi, foods: List<JsonObject>) {
        try {
            val payload = withContext(Dispatchers.IO) { api.call("PUT", "/quick-foods", mapOf("foods" to foods)) }
            setMyFoods(payload["quickFoods"])
        } catch (thrown: CancellationException) {
            throw thrown
        } catch (thrown: Exception) {
            throw ShellPromptError("Could not save My foods: ${Calories.errorText(thrown)}")
        }
    }

    private suspend fun quickLog(api: CaloriesApi, food: JsonObject, meal: String): String = try {
        postEntries(api, listOf(CaloriesPrompt.quickFoodFields(food)), meal, "quick", day)
        runCatching { refresh(api) }
        CaloriesPrompt.quickLogged(food, meal)
    } catch (thrown: CancellationException) {
        throw thrown
    } catch (thrown: Exception) {
        "Could not log ${jsonString(food["name"])}: ${Calories.errorText(thrown)}"
    }

    private suspend fun submitPrompt(api: CaloriesApi, raw: String, photos: List<PromptAttachment>): String {
        if (!installed) throw ShellPromptError("Install Calorie Counter first.")
        val text = raw.trim()
        val photo = photos.firstOrNull()
        if (text.isEmpty() && photo == null) throw ShellPromptError(CaloriesPrompt.EMPTY)
        if (photo != null && CaloriesPrompt.isReceipt(text, photo.name)) return readReceipt(api, photo.bytes)
        val today = LocalDate.now()
        val meal = CaloriesPrompt.meal(text).ifEmpty { Calories.guessMeal(LocalTime.now().hour) }
        val logDay = CaloriesPrompt.day(text, day, today)
        try {
            if (photo != null) {
                val payload = estimate(api, mapOf("image" to jpeg(photo.bytes, CaloriesPrompt.MEAL_SIDE), "hint" to CaloriesPrompt.clip(text, 300)))
                val items = jsonObjects(payload["items"])
                if (items.isEmpty()) throw ShellPromptError(CaloriesPrompt.NOTHING_IDENTIFIED)
                return logItems(api, items, meal, "photo", logDay)
            }
            val weight = CaloriesPrompt.weight(text, imperial, day, today)
            if (weight != null) {
                withContext(Dispatchers.IO) { api.call("PUT", "/weights/${weight.day}", mapOf("weightKg" to weight.kg)) }
                runCatching { refresh(api) }
                return CaloriesPrompt.weightSaved(weightText(weight.kg), weight.day, today.toString())
            }
            val local = CaloriesPrompt.match(text, listOf(myFoods, frequentFoods, commonFoods))
            if (local != null) return logItems(api, local, meal, "quick", logDay)
            val payload = estimate(api, mapOf("description" to CaloriesPrompt.clip(text, 1000)))
            val items = jsonObjects(payload["items"])
            if (items.isEmpty()) throw ShellPromptError(CaloriesPrompt.NOTHING_IDENTIFIED)
            return logItems(api, items, meal, "text", logDay)
        } catch (thrown: ShellPromptError) {
            throw thrown
        } catch (thrown: CancellationException) {
            throw thrown
        } catch (thrown: Exception) {
            throw ShellPromptError(Calories.errorText(thrown))
        }
    }

    @OptIn(ExperimentalLayoutApi::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val api = Services.get<ApiService>()?.let(::CaloriesApi) ?: return
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val status = rememberFmStatus()
        val promptTarget = remember(api) {
            ShellPromptTarget("calories", "Calorie Counter", "restaurant_outlined", CaloriesPrompt.HINT, CaloriesPrompt.TOOLTIP) { text, photos ->
                submitPrompt(api, text, photos)
            }
        }
        DisposableEffect(installed, promptTarget) {
            val targets = ComposerFeature.promptTargets
            val owner = this@CaloriesScreen
            if (installed) targets.register(owner, promptTarget) else targets.unregister(owner)
            onDispose { targets.unregister(owner) }
        }
        LaunchedEffect(generation) {
            loading = true
            error = ""
            try {
                val data = withContext(Dispatchers.IO) { api.overview(Calories.isoDay(LocalDate.now())) }
                installed = data["installed"] == true
                overview = if (installed) data else null
            } catch (thrown: OrgApiException) {
                if (thrown.code == "plugin_not_installed") {
                    installed = false
                    overview = null
                } else {
                    error = thrown.message
                }
            } catch (thrown: Exception) {
                error = Calories.errorText(thrown)
            }
            loading = false
        }
        fun run(success: String = "", action: suspend () -> Unit) {
            scope.launch {
                try {
                    action()
                    if (success.isNotEmpty()) status.show(success)
                    generation++
                } catch (thrown: Exception) {
                    status.error(Calories.errorText(thrown))
                }
            }
        }
        fun call(method: String, suffix: String, body: JsonObject? = null, success: String = "") =
            run(success) { withContext(Dispatchers.IO) { api.call(method, suffix, body) } }
        fun addEntries(items: List<JsonObject>, meal: String, source: String) = run("${items.size} item(s) added to ${Calories.dayLabel(day)}.") {
            withContext(Dispatchers.IO) {
                for (item in items) api.call("POST", "/entries", item + mapOf("day" to day, "meal" to meal, "source" to source))
            }
        }
        fun analyze(body: JsonObject) {
            scope.launch {
                try {
                    suggestions = withContext(Dispatchers.IO) { api.call("POST", "/analyze", body) }
                    status.clear()
                } catch (thrown: Exception) {
                    status.error(Calories.errorText(thrown))
                }
            }
        }
        val picker = rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri ->
            if (uri != null) {
                photoUri = uri
                sheet = "photoHint"
            }
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar("Calorie Counter", navigator) {
                FmIconButton("refresh", "Refresh", onPressed = { generation++ }, color = Fm.colors.textPrimary)
            }
            val current = overview
            when {
                loading && current == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                error.isNotEmpty() && current == null -> FmEmptyState("restaurant_outlined", "Calorie Counter is unavailable", message = error)
                !installed -> Column(
                    Modifier.fillMaxSize().padding(24.dp),
                    verticalArrangement = Arrangement.Center,
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Text("Install Calorie Counter for yourself", textAlign = TextAlign.Center, style = Fm.text.titleMedium)
                    Spacer(Modifier.height(12.dp))
                    Text(
                        "Your food diary, goals, reminders, charts, and weigh-ins are private to your ForkMesh account and sync across web, desktop, and mobile. Every row is encrypted at rest with a key minted for your account alone. Invite friends to challenges by username.",
                        textAlign = TextAlign.Center,
                        style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
                    )
                    Spacer(Modifier.height(16.dp))
                    AccountFilledButton("Install Calorie Counter", "download", onClick = {
                        run("Calorie Counter installed.") { withContext(Dispatchers.IO) { api.install(tzOffsetMinutes()) } }
                    })
                    FmInlineStatus(status, Modifier.padding(top = 12.dp))
                }
                current != null -> {
                    FmSegmentTabBar(
                        tabs = listOf(FmSegmentTab("Diary"), FmSegmentTab("Trends"), FmSegmentTab("Goals"), FmSegmentTab("Challenges")),
                        index = tab,
                        onChanged = { tab = it },
                    )
                    FmInlineStatus(status, Modifier.padding(horizontal = 16.dp))
                    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).imePadding().padding(16.dp)) {
                        when (tab) {
                            0 -> DiaryTab(current, onPickDay = { sheet = "day" }, onAction = { action ->
                                when (action) {
                                    "photo" -> {
                                        val sources = current["sources"] as? Map<*, *>
                                        if (sources != null && sources["ai"] == false) {
                                            status.error("AI estimates are unavailable on this host. Describe the meal or add it by hand.")
                                        } else {
                                            picker.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))
                                        }
                                    }
                                    "add" -> {
                                        editingEntry = null
                                        sheet = "entry"
                                    }
                                    "quick" -> sheet = "quick"
                                    else -> sheet = action
                                }
                            }, onEntry = {
                                editingEntry = it
                                sheet = "entry"
                            })
                            1 -> TrendsTab(current)
                            2 -> GoalsTab(
                                current,
                                onSave = { body -> call("PUT", "/profile", body, "Goals and reminders saved.") },
                                onSaveKey = { key -> call("PUT", "/settings", mapOf("usdaApiKey" to key), "Food database settings saved.") },
                            )
                            else -> ChallengesTab(current, onNew = { sheet = "challenge" }, onOpen = { id ->
                                scope.launch {
                                    try {
                                        board = jsonObject(withContext(Dispatchers.IO) { api.call("GET", "/challenges/$id") }["challenge"])
                                    } catch (thrown: Exception) {
                                        status.error(Calories.errorText(thrown))
                                    }
                                }
                            })
                        }
                    }
                }
            }
        }
        when (sheet) {
            "day" -> DayPickerDialog(day, 1, onDismiss = { sheet = null }) {
                day = it
                sheet = null
            }
            "entry" -> EntrySheet(editingEntry, day, onDismiss = { sheet = null }) { result ->
                sheet = null
                val entry = editingEntry
                if (result == null && entry != null) {
                    call("DELETE", "/entries/${jsonString(entry["id"])}", emptyMap(), "Entry deleted.")
                } else if (result != null) {
                    if (entry != null) call("PATCH", "/entries/${jsonString(entry["id"])}", result, "Saved to your diary.") else call("POST", "/entries", result, "Saved to your diary.")
                }
            }
            "photoHint" -> PromptDialog("Anything the model should know?", "half portion, brown rice… (optional)", allowEmpty = true, onDismiss = { sheet = null }) { hint ->
                sheet = null
                val uri = photoUri ?: return@PromptDialog
                scope.launch {
                    val dataUrl = try {
                        withContext(Dispatchers.IO) { imageDataUrl(context, uri) }
                    } catch (_: Exception) {
                        status.error("That photo could not be read.")
                        return@launch
                    }
                    status.show("Asking the model about the photo…")
                    analyze(mapOf("image" to dataUrl, "hint" to hint))
                }
            }
            "describe" -> PromptDialog("Describe the meal", "2 scrambled eggs, 1 slice of toast with butter", lines = 3, onDismiss = { sheet = null }) { text ->
                sheet = null
                status.show("Asking the model about the meal…")
                analyze(mapOf("description" to text))
            }
            "barcode" -> PromptDialog("Type the barcode digits", "Printed under the barcode", number = true, onDismiss = { sheet = null }) { text ->
                sheet = null
                val digits = Calories.barcodeDigits(text) ?: return@PromptDialog
                scope.launch {
                    try {
                        product = jsonObject(withContext(Dispatchers.IO) { api.call("GET", "/barcode/$digits") }["product"]) to "barcode"
                    } catch (thrown: OrgApiException) {
                        status.error(if (thrown.code == "product_not_found") "No product found for $digits. Try a search or add it by hand." else thrown.message)
                    } catch (thrown: Exception) {
                        status.error(Calories.errorText(thrown))
                    }
                }
            }
            "quick" -> QuickAddSheet(
                dayLabel = if (day == todayDay) "today" else Calories.dayLabel(day),
                mine = myFoods,
                frequent = frequentFoods,
                common = commonFoods,
                receiptUnavailable = CaloriesPrompt.receiptUnavailable(overview),
                onDismiss = { sheet = null },
                onLog = { food, meal -> quickLog(api, food, meal) },
                onSaveMine = { foods -> saveMyFoods(api, foods) },
                onReceipt = { uri -> readReceipt(api, PhotoIntake.fromUri(context, uri).bytes) },
            )
            "search" -> SearchSheet(api, onDismiss = { sheet = null }) { food ->
                sheet = null
                product = food to "search"
            }
            "weigh" -> {
                val latest = Calories.num(jsonObject(overview?.get("latestWeight"))["weightKg"])
                PromptDialog(
                    "Weigh in · ${Calories.dayLabel(day)}",
                    "",
                    number = true,
                    suffix = weightUnit,
                    initial = if (latest > 0) Calories.grams(displayWeight(latest)) else "",
                    extra = { FmTextButton(onClick = { sheet = null; call("DELETE", "/weights/$day", emptyMap(), "Weigh-in removed.") }) { Text("Remove") } },
                    onDismiss = { sheet = null },
                ) { value ->
                    sheet = null
                    val shown = value.toDoubleOrNull()
                    if (shown != null && shown > 0) call("PUT", "/weights/$day", mapOf("weightKg" to toKg(shown)), "Weigh-in saved.")
                }
            }
            "challenge" -> ChallengeSheet(day, onDismiss = { sheet = null }) { body ->
                sheet = null
                run {
                    val result = withContext(Dispatchers.IO) { api.call("POST", "/challenges", body) }
                    status.show(Calories.inviteNote("Challenge created.", result))
                }
            }
            else -> {}
        }
        suggestions?.let { payload ->
            SuggestionsSheet(payload, onDismiss = { suggestions = null }) { items, meal ->
                suggestions = null
                addEntries(items, meal, jsonString(payload["source"], "photo"))
            }
        }
        product?.let { (food, source) ->
            if (food.isEmpty()) {
                product = null
            } else {
                PortionSheet(food, onDismiss = { product = null }) { values ->
                    product = null
                    val meal = values.remove("meal").toString()
                    addEntries(listOf(values), meal, source)
                }
            }
        }
        board?.let { challenge ->
            val id = jsonString(challenge["id"])
            var inviting by remember { mutableStateOf(false) }
            if (inviting) {
                PromptDialog("Invite friends", "maya, @sam", onDismiss = { inviting = false; board = null }) { names ->
                    inviting = false
                    board = null
                    val members = Calories.usernames(names)
                    if (members.isNotEmpty()) {
                        run {
                            val result = withContext(Dispatchers.IO) { api.call("POST", "/challenges/$id/invite", mapOf("members" to members)) }
                            status.show(Calories.inviteNote("Invitations sent.", result))
                        }
                    }
                }
            } else {
                BoardSheet(challenge, jsonString(overview?.get("actor")), onDismiss = { board = null }) { action ->
                    when (action) {
                        "accept", "decline" -> {
                            board = null
                            call("POST", "/challenges/$id/respond", mapOf("accept" to (action == "accept")), if (action == "accept") "You are in. Good luck!" else "Challenge left.")
                        }
                        "invite" -> inviting = true
                        "close" -> {
                            board = null
                            val starts = jsonString(challenge["startsOn"])
                            val end = if (day < starts) starts else day
                            call("PATCH", "/challenges/$id", mapOf("title" to jsonString(challenge["title"]), "status" to "closed", "endsOn" to end), "Challenge ended.")
                        }
                        "delete" -> {
                            board = null
                            call("DELETE", "/challenges/$id", emptyMap(), "Challenge deleted.")
                        }
                    }
                }
            }
        }
    }

    @OptIn(ExperimentalLayoutApi::class)
    @Composable
    private fun DiaryTab(overview: JsonObject, onPickDay: () -> Unit, onAction: (String) -> Unit, onEntry: (JsonObject) -> Unit) {
        val c = Fm.colors
        val totals = jsonObject(overview["todayTotals"])
        val goal = Calories.int(profile["dailyCalories"])
        val calories = Calories.num(totals["calories"])
        val today = jsonString(overview["today"])
        val entries = Calories.sortedEntries(jsonObjects(overview["entries"]))
        val dayTotals = Calories.dayTotals(entries)
        val streaks = jsonObject(overview["streaks"])
        val latest = jsonObject(overview["latestWeight"])
        Row(verticalAlignment = Alignment.CenterVertically) {
            AccountOutlinedButton("Log to ${Calories.dayLabel(day)}", onPickDay, icon = "calendar_today_outlined")
            Spacer(Modifier.weight(1f))
            FmTextButton(onClick = { day = Calories.isoDay(LocalDate.now()) }) { Text("Today") }
        }
        Spacer(Modifier.height(12.dp))
        FmCard {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.size(120.dp), contentAlignment = Alignment.Center) {
                    val ratio = Calories.ringRatio(goal, calories).toFloat()
                    val over = goal > 0 && calories > goal
                    val track = c.border
                    val arc = if (over) c.warning else c.accent
                    Canvas(Modifier.fillMaxSize()) {
                        val stroke = 10.dp.toPx()
                        val inset = 8.dp.toPx()
                        val radius = size.minDimension / 2 - inset
                        val topLeft = Offset(center.x - radius, center.y - radius)
                        val arcSize = Size(radius * 2, radius * 2)
                        drawCircle(track, radius, center, style = Stroke(stroke))
                        if (ratio > 0) drawArc(arc, -90f, 360f * ratio, false, topLeft, arcSize, style = Stroke(stroke, cap = StrokeCap.Round))
                    }
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Text(Calories.kcal(calories), style = Fm.text.titleLarge)
                        Text("of $goal kcal", style = Fm.text.labelSmall)
                    }
                }
                Spacer(Modifier.width(16.dp))
                Column(Modifier.weight(1f)) {
                    MacroRow("Protein", totals["proteinG"], profile["proteinG"])
                    MacroRow("Carbs", totals["carbsG"], profile["carbsG"])
                    MacroRow("Fat", totals["fatG"], profile["fatG"])
                    Text(Calories.remainingText(goal, calories), style = Fm.text.bodySmall)
                }
            }
        }
        Spacer(Modifier.height(12.dp))
        FmCard {
            Row {
                Stat(Modifier.weight(1f), "Streaks", "${Calories.int(streaks["logged"])} days logged · ${Calories.int(streaks["onGoal"])} within goal")
                Stat(
                    Modifier.weight(1f),
                    "Weight",
                    if (Calories.num(latest["weightKg"]) > 0) "${weightText(latest["weightKg"])} on ${Calories.dayLabel(jsonString(latest["day"]))}" else "No weigh-in yet",
                )
            }
        }
        Spacer(Modifier.height(12.dp))
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            FmToneButton("Photo", onPressed = { onAction("photo") }, icon = "photo_camera_outlined", tone = FmTone.Accent, filled = true)
            FmToneButton("Quick add", onPressed = { onAction("quick") }, icon = "bolt_outlined")
            FmToneButton("Describe", onPressed = { onAction("describe") }, icon = "auto_awesome_outlined")
            FmToneButton("Barcode", onPressed = { onAction("barcode") }, icon = "qr_code_scanner_outlined")
            FmToneButton("Search", onPressed = { onAction("search") }, icon = "search")
            FmToneButton("Add", onPressed = { onAction("add") }, icon = "add")
            FmToneButton("Weigh in", onPressed = { onAction("weigh") }, icon = "monitor_weight_outlined")
        }
        Spacer(Modifier.height(16.dp))
        FmSectionHeader(Calories.diaryHeader(entries.size, dayTotals.size))
        if (entries.isEmpty()) {
            Text(
                "Nothing logged yet. Snap a photo, type a barcode, or add a meal.",
                modifier = Modifier.fillMaxWidth().padding(24.dp),
                textAlign = TextAlign.Center,
                style = Fm.text.bodyMedium.copy(color = c.textSecondary),
            )
        }
        entries.forEachIndexed { index, entry ->
            val entryDay = jsonString(entry["day"])
            if (index == 0 || entryDay != jsonString(entries[index - 1]["day"])) {
                Row(Modifier.fillMaxWidth().padding(top = 12.dp, bottom = 4.dp)) {
                    Text(if (entryDay == today) "Today" else Calories.dayLabel(entryDay), style = Fm.text.labelLarge)
                    Spacer(Modifier.weight(1f))
                    Text("${Calories.kcal(dayTotals[entryDay] ?: 0.0)} kcal", style = Fm.text.labelLarge.copy(color = c.textSecondary))
                }
            }
            FmListRow(
                title = jsonString(entry["name"]),
                subtitle = Calories.entrySubtitle(entry),
                trailing = { Text("${Calories.kcal(entry["calories"])} kcal", style = Fm.text.labelMedium) },
                showChevron = false,
                onTap = { onEntry(entry) },
            )
        }
        if (overview["entriesTruncated"] == true) {
            Text(
                "Showing the ${entries.size} most recent entries. Older ones stay in your export.",
                modifier = Modifier.fillMaxWidth().padding(12.dp),
                textAlign = TextAlign.Center,
                style = Fm.text.bodySmall,
            )
        }
    }

    @Composable
    private fun TrendsTab(overview: JsonObject) {
        val c = Fm.colors
        val days = jsonObjects(overview["days"])
        val weights = jsonObjects(overview["weights"])
        val goal = Calories.int(profile["dailyCalories"])
        val target = Calories.num(profile["targetWeightKg"])
        FmSectionHeader("Calories · last 30 days")
        FmCard {
            val chart = Calories.dayChart(days, goal)
            val accent = c.accent
            val over = c.warning
            val grid = c.border
            Column {
                Canvas(Modifier.fillMaxWidth().height(180.dp)) {
                    val left = 8.dp.toPx()
                    val plotW = size.width - left
                    for (step in 0..4) {
                        val y = size.height - size.height * step / 4
                        drawLine(grid, Offset(left, y), Offset(size.width, y), 1f)
                    }
                    if (days.isNotEmpty()) {
                        val slot = plotW / days.size
                        val barWidth = slot.coerceIn(2f, 14.dp.toPx() + 3f) - 3f
                        for (bar in chart.bars) {
                            val h = (size.height * bar.heightRatio).toFloat().coerceAtLeast(2f)
                            val x = left + bar.index * slot + (slot - barWidth) / 2
                            drawRoundRect(if (bar.over) over else accent, Offset(x, size.height - h), Size(barWidth.coerceAtLeast(2f), h), CornerRadius(2f, 2f))
                        }
                    }
                    chart.goalRatio?.let { ratio ->
                        val y = (size.height - size.height * ratio).toFloat()
                        var x = left
                        while (x < size.width) {
                            drawLine(over, Offset(x, y), Offset((x + 4.dp.toPx()).coerceAtMost(size.width), y), 2.dp.toPx())
                            x += 8.dp.toPx()
                        }
                    }
                }
                if (days.isNotEmpty()) {
                    Row(Modifier.fillMaxWidth()) {
                        Text(jsonString(days.first()["day"]).drop(5), style = Fm.text.labelSmall)
                        Spacer(Modifier.weight(1f))
                        Text("peak ${Calories.kcal(chart.peak)} kcal", style = Fm.text.labelSmall)
                        Spacer(Modifier.weight(1f))
                        Text(jsonString(days.last()["day"]).drop(5), style = Fm.text.labelSmall)
                    }
                }
            }
        }
        Text(
            "Seven-day average ${Calories.kcal(overview["weekAverage"])} kcal · goal $goal kcal · blue within goal, orange over",
            modifier = Modifier.padding(vertical = 8.dp),
            style = Fm.text.bodySmall,
        )
        FmSectionHeader("Weight · last 180 days")
        FmCard {
            val chart = Calories.weightChart(weights, target, imperial)
            if (chart == null) {
                Box(Modifier.fillMaxWidth().height(160.dp), contentAlignment = Alignment.Center) {
                    Text("No weigh-ins yet. Use Weigh in on the Diary tab.", style = Fm.text.bodySmall)
                }
            } else {
                val line = c.success
                val goalColor = c.warning
                val grid = c.border
                val surface = c.bgRaised
                Column {
                    Canvas(Modifier.fillMaxWidth().height(160.dp)) {
                        for (step in 0..3) {
                            val y = size.height - size.height * step / 3
                            drawLine(grid, Offset(0f, y), Offset(size.width, y), 1f)
                        }
                        chart.targetRatio?.let { ratio ->
                            val y = (size.height - size.height * ratio).toFloat()
                            var x = 0f
                            while (x < size.width) {
                                drawLine(goalColor, Offset(x, y), Offset((x + 4.dp.toPx()).coerceAtMost(size.width), y), 2.dp.toPx())
                                x += 8.dp.toPx()
                            }
                        }
                        val path = Path()
                        val points = chart.points.map { (px, py) -> Offset((px * size.width).toFloat(), (size.height - py * size.height).toFloat()) }
                        points.forEachIndexed { i, p -> if (i == 0) path.moveTo(p.x, p.y) else path.lineTo(p.x, p.y) }
                        drawPath(path, line, style = Stroke(2.dp.toPx()))
                        for (p in points) {
                            drawCircle(line, 4.dp.toPx(), p)
                            drawCircle(surface, 4.dp.toPx(), p, style = Stroke(1.5f.dp.toPx()))
                        }
                    }
                    Row(Modifier.fillMaxWidth()) {
                        Text(Calories.grams(chart.low) + " $weightUnit", style = Fm.text.labelSmall)
                        Spacer(Modifier.weight(1f))
                        Text(Calories.grams(chart.high) + " $weightUnit", style = Fm.text.labelSmall)
                    }
                }
            }
        }
        Spacer(Modifier.height(12.dp))
        FmSectionHeader("Day by day")
        for (entry in days.asReversed()) {
            FmListRow(
                title = Calories.dayLabel(jsonString(entry["day"])),
                subtitle = "P ${Calories.grams(entry["proteinG"])} · C ${Calories.grams(entry["carbsG"])} · F ${Calories.grams(entry["fatG"])}",
                trailing = { Text(if (Calories.int(entry["entries"]) > 0) "${Calories.kcal(entry["calories"])} kcal" else "—", style = Fm.text.labelMedium) },
                showChevron = false,
                minHeight = 48.dp,
            )
        }
    }

    @Composable
    private fun GoalsTab(overview: JsonObject, onSave: (JsonObject) -> Unit, onSaveKey: (String) -> Unit) {
        val draft = remember(overview) { GoalsDraft(profile) }
        var tick by remember { mutableIntStateOf(0) }
        var usda by remember { mutableStateOf("") }
        var timeFor by remember { mutableStateOf<Int?>(null) }
        val reminders = remember(overview) { mutableStateListOf<MutableMap<String, Any?>>().apply { addAll(draft.reminders) } }
        fun changed() {
            tick++
        }
        tick.let { }
        val latestKg = Calories.num(jsonObject(overview["latestWeight"])["weightKg"])
        val unit = if (draft.imperial) "lb" else "kg"
        FmSectionHeader("Daily goals")
        NumberField("Calories", draft.calories, "kcal") { draft.calories = it; changed() }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            NumberField("Protein", draft.protein, "g", Modifier.weight(1f)) { draft.protein = it; changed() }
            NumberField("Carbs", draft.carbs, "g", Modifier.weight(1f)) { draft.carbs = it; changed() }
            NumberField("Fat", draft.fat, "g", Modifier.weight(1f)) { draft.fat = it; changed() }
        }
        Text("Units", style = Fm.text.labelMedium, modifier = Modifier.padding(top = 8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            for ((value, label) in listOf("auto" to draft.autoLabel, "metric" to "Metric (kg, cm)", "imperial" to "Imperial (lb, in)")) {
                FmToneButton(label, onPressed = { draft.setUnits(value); changed() }, tone = if (draft.unitsPreference == value) FmTone.Accent else FmTone.Neutral, filled = draft.unitsPreference == value)
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            NumberField("Target weight", draft.target, unit, Modifier.weight(1f)) { draft.target = it; changed() }
            NumberField("Starting weight", draft.start, unit, Modifier.weight(1f)) { draft.start = it; changed() }
        }
        NumberField("Height", draft.height, if (draft.imperial) "in" else "cm") { draft.height = it; changed() }
        Calories.bmi(latestKg, draft.heightCm)?.let {
            Text("BMI $it from your latest weigh-in.", style = Fm.text.bodySmall, modifier = Modifier.padding(top = 4.dp))
        }
        Spacer(Modifier.height(12.dp))
        FmSectionHeader("Reminders", trailing = {
            FmTextButton(onClick = { if (reminders.size < 8) timeFor = -1 }) { Text("Add") }
        })
        Text("Delivered as pings in this phone's time zone. Up to eight per day.", style = Fm.text.bodySmall)
        reminders.forEachIndexed { i, reminder ->
            Column(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmTextButton(onClick = { timeFor = i }) { Text(jsonString(reminder["time"])) }
                    FmTextField(
                        value = jsonString(reminder["label"]),
                        onValueChange = { reminders[i] = LinkedHashMap(reminder).apply { put("label", it) } },
                        hint = "Log lunch",
                        modifier = Modifier.weight(1f),
                    )
                    FmIconButton("close", "Remove reminder", onPressed = { reminders.removeAt(i) })
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmCheckbox(checked = reminder["onlyIfUnlogged"] == true, onCheckedChange = { on -> reminders[i] = LinkedHashMap(reminder).apply { put("onlyIfUnlogged", on) } })
                    Text("Only if nothing logged", style = Fm.text.bodySmall, modifier = Modifier.weight(1f))
                    FmSwitch(checked = reminder["enabled"] != false, onCheckedChange = { on -> reminders[i] = LinkedHashMap(reminder).apply { put("enabled", on) } })
                }
            }
        }
        AccountFilledButton("Save goals and reminders", "check", modifier = Modifier.fillMaxWidth().padding(vertical = 12.dp), onClick = {
            draft.reminders.clear()
            draft.reminders.addAll(reminders)
            onSave(draft.body(tzOffsetMinutes()))
        })
        FmSectionHeader("Food databases")
        val usdaSaved = (overview["sources"] as? Map<*, *>)?.get("usda") == true
        Text(
            "Open Food Facts is always used for barcodes and search. Add your own free USDA FoodData Central API key to also search the USDA database. It is saved on your account only. ${if (usdaSaved) "A USDA key is saved." else "No USDA key saved."}",
            style = Fm.text.bodySmall,
        )
        FmTextField(value = usda, onValueChange = { usda = it }, hint = "USDA FoodData Central API key", visualTransformation = PasswordVisualTransformation(), modifier = Modifier.padding(vertical = 8.dp))
        AccountOutlinedButton("Save key", {
            onSaveKey(usda.trim())
            usda = ""
        })
        timeFor?.let { index ->
            val parts = if (index >= 0) jsonString(reminders[index]["time"]).split(":") else listOf("12", "30")
            TimePickerDialog(parts.first().toIntOrNull() ?: 12, parts.last().toIntOrNull() ?: 0, onDismiss = { timeFor = null }) { h, m ->
                timeFor = null
                if (index >= 0) {
                    reminders[index] = LinkedHashMap(reminders[index]).apply { put("time", Calories.reminderTime(h, m)) }
                } else if (reminders.size < 8) {
                    reminders.add(linkedMapOf("time" to Calories.reminderTime(h, m), "label" to "Time to log your meal", "onlyIfUnlogged" to false, "enabled" to true))
                }
            }
        }
    }

    @Composable
    private fun ChallengesTab(overview: JsonObject, onNew: () -> Unit, onOpen: (String) -> Unit) {
        val challenges = jsonObjects(overview["challenges"])
        Text(
            "Challenge friends by their ForkMesh username to lose weight or keep a logging streak. Standings show progress only, never anyone's diary.",
            style = Fm.text.bodySmall,
        )
        Spacer(Modifier.height(12.dp))
        AccountFilledButton("New challenge", "emoji_events_outlined", onClick = onNew)
        if (challenges.isEmpty()) {
            Text(
                "No challenges yet. Start one and invite your friends.",
                modifier = Modifier.fillMaxWidth().padding(24.dp),
                textAlign = TextAlign.Center,
                style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
            )
        }
        for (item in challenges) {
            FmCard(margin = androidx.compose.foundation.layout.PaddingValues(top = 12.dp), onTap = { onOpen(jsonString(item["id"])) }) {
                Column {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(jsonString(item["title"]), style = Fm.text.titleSmall, modifier = Modifier.weight(1f))
                        FmStatusBadge(Calories.cap(jsonString(item["status"])), if (item["status"] == "active") Fm.colors.success else Fm.colors.textSecondary)
                    }
                    Text(Calories.KIND_LABELS[jsonString(item["kind"])] ?: jsonString(item["kind"]), style = Fm.text.bodySmall)
                    Text(Calories.challengeLine(item), style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
                    if (item["myStatus"] == "invited") Text("You are invited", style = Fm.text.bodySmall.copy(color = Fm.colors.accent))
                }
            }
        }
    }
}

@Composable
private fun MacroRow(label: String, value: Any?, goal: Any?) {
    val ratio = if (Calories.num(goal) > 0) (Calories.num(value) / Calories.num(goal)).coerceIn(0.0, 1.0) else 0.0
    Column(Modifier.padding(bottom = 8.dp)) {
        Row {
            Text(label, style = Fm.text.labelSmall)
            Spacer(Modifier.weight(1f))
            Text("${Calories.grams(value)} g / ${Calories.grams(goal)} g", style = Fm.text.labelSmall)
        }
        Spacer(Modifier.height(4.dp))
        FmLinearProgressIndicator(progress = { ratio.toFloat() }, modifier = Modifier.fillMaxWidth())
    }
}

@Composable
private fun Stat(modifier: Modifier, label: String, value: String) {
    Column(modifier) {
        Text(label.uppercase(), style = Fm.text.labelSmall)
        Spacer(Modifier.height(4.dp))
        Text(value, style = Fm.text.bodyMedium)
    }
}

@Composable
private fun NumberField(label: String, value: String, suffix: String, modifier: Modifier = Modifier, onChange: (String) -> Unit) {
    var text by remember { mutableStateOf(value) }
    Column(modifier.padding(top = 8.dp)) {
        Text(label, style = Fm.text.labelMedium)
        FmTextField(
            value = text,
            onValueChange = {
                text = it
                onChange(it)
            },
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
            trailing = { Text(suffix, modifier = Modifier.padding(end = 12.dp), style = Fm.text.bodySmall) },
        )
    }
}

@Composable
private fun MealPicker(meal: String, onChange: (String) -> Unit) {
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        for (option in Calories.MEALS) {
            FmToneButton(Calories.cap(option), onPressed = { onChange(option) }, tone = if (meal == option) FmTone.Accent else FmTone.Neutral, filled = meal == option)
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun EntrySheet(entry: JsonObject?, day: String, onDismiss: () -> Unit, onResult: (JsonObject?) -> Unit) {
    val editing = entry != null
    var name by remember { mutableStateOf(jsonString(entry?.get("name"))) }
    var portion by remember { mutableStateOf(jsonString(entry?.get("portion"))) }
    var calories by remember { mutableStateOf(if (entry == null) "" else Calories.kcal(entry["calories"])) }
    var protein by remember { mutableStateOf(if (entry == null) "" else Calories.grams(entry["proteinG"])) }
    var carbs by remember { mutableStateOf(if (entry == null) "" else Calories.grams(entry["carbsG"])) }
    var fat by remember { mutableStateOf(if (entry == null) "" else Calories.grams(entry["fatG"])) }
    var notes by remember { mutableStateOf(jsonString(entry?.get("notes"))) }
    var meal by remember { mutableStateOf(entry?.get("meal")?.let { jsonString(it) } ?: Calories.guessMeal(LocalTime.now().hour)) }
    var entryDay by remember { mutableStateOf(entry?.get("day")?.let { jsonString(it) } ?: day) }
    var picking by remember { mutableStateOf(false) }
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.imePadding().verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(if (editing) "Edit food" else "Add food", style = Fm.text.titleLarge)
            FmTextField(value = name, onValueChange = { name = it }, hint = "Food", autofocus = !editing)
            FmTextField(value = portion, onValueChange = { portion = it }, hint = "Portion (1 bowl, 150 g…)")
            AccountOutlinedButton(Calories.dayLabel(entryDay), { picking = true }, icon = "calendar_today_outlined")
            MealPicker(if (meal in Calories.MEALS) meal else "snack") { meal = it }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                NumberField("kcal", calories, "", Modifier.weight(1f)) { calories = it }
                NumberField("Protein g", protein, "", Modifier.weight(1f)) { protein = it }
                NumberField("Carbs g", carbs, "", Modifier.weight(1f)) { carbs = it }
                NumberField("Fat g", fat, "", Modifier.weight(1f)) { fat = it }
            }
            FmTextField(value = notes, onValueChange = { notes = it }, hint = "Notes", minLines = 2, maxLines = 2)
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (editing) FmTextButton(onClick = { onResult(null) }, contentColor = Fm.colors.danger) { Text("Delete") }
                Spacer(Modifier.weight(1f))
                FmTextButton(onClick = onDismiss) { Text("Cancel") }
                AccountFilledButton("Save", "check", onClick = {
                    if (name.trim().isEmpty()) return@AccountFilledButton
                    val body = linkedMapOf<String, Any?>(
                        "day" to entryDay,
                        "name" to name.trim(),
                        "portion" to portion.trim(),
                        "meal" to meal,
                        "calories" to Calories.int(calories),
                        "proteinG" to Calories.num(protein),
                        "carbsG" to Calories.num(carbs),
                        "fatG" to Calories.num(fat),
                        "notes" to notes,
                    )
                    if (!editing) body["source"] = "manual"
                    onResult(body)
                })
            }
        }
    }
    if (picking) DayPickerDialog(entryDay, 1, onDismiss = { picking = false }) {
        entryDay = it
        picking = false
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun SuggestionsSheet(payload: JsonObject, onDismiss: () -> Unit, onAdd: (List<JsonObject>, String) -> Unit) {
    val items = jsonObjects(payload["items"])
    val chosen = remember { mutableStateListOf<Boolean>().apply { repeat(items.size) { add(true) } } }
    val kcal = remember { mutableStateListOf<String>().apply { items.forEach { add(Calories.int(it["calories"]).toString()) } } }
    var meal by remember { mutableStateOf(Calories.guessMeal(LocalTime.now().hour)) }
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.imePadding().verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Suggested foods · about ${Calories.int(payload["totalCalories"])} kcal", style = Fm.text.titleLarge)
            Text("Untick anything wrong and correct the calories before adding. ${jsonString(payload["summary"])}", style = Fm.text.bodySmall)
            items.forEachIndexed { i, item ->
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmCheckbox(checked = chosen[i], onCheckedChange = { chosen[i] = it })
                    Column(Modifier.weight(1f)) {
                        Text(jsonString(item["name"]), style = Fm.text.bodyMedium)
                        Text(
                            "${jsonString(item["portion"])} · confidence ${Calories.int(Calories.num(item["confidence"]) * 100)}% · P ${Calories.grams(item["proteinG"])} · C ${Calories.grams(item["carbsG"])} · F ${Calories.grams(item["fatG"])}",
                            style = Fm.text.labelSmall,
                        )
                    }
                    FmTextField(
                        value = kcal[i],
                        onValueChange = { kcal[i] = it },
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        modifier = Modifier.width(96.dp),
                        trailing = { Text("kcal", style = Fm.text.labelSmall, modifier = Modifier.padding(end = 8.dp)) },
                    )
                }
            }
            MealPicker(meal) { meal = it }
            AccountFilledButton("Add selected to diary", "add", modifier = Modifier.fillMaxWidth(), onClick = {
                val picked = items.indices.filter { chosen[it] }.map { i ->
                    val item = items[i]
                    linkedMapOf(
                        "name" to item["name"],
                        "portion" to item["portion"],
                        "calories" to Calories.int(kcal[i]),
                        "proteinG" to item["proteinG"],
                        "carbsG" to item["carbsG"],
                        "fatG" to item["fatG"],
                        "confidence" to item["confidence"],
                    )
                }
                onAdd(picked, meal)
            })
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun QuickAddSheet(
    dayLabel: String,
    mine: List<JsonObject>,
    frequent: List<JsonObject>,
    common: List<JsonObject>,
    receiptUnavailable: String?,
    onDismiss: () -> Unit,
    onLog: suspend (JsonObject, String) -> String,
    onSaveMine: suspend (List<JsonObject>) -> Unit,
    onReceipt: suspend (Uri) -> String,
) {
    var meal by remember { mutableStateOf(Calories.guessMeal(LocalTime.now().hour)) }
    var filter by remember { mutableStateOf("") }
    var status by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    fun perform(action: suspend () -> String) {
        if (busy) return
        busy = true
        scope.launch {
            try {
                status = action()
            } catch (thrown: CancellationException) {
                throw thrown
            } catch (thrown: Exception) {
                status = ShellPromptTargets.message(thrown)
            } finally {
                busy = false
            }
        }
    }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri ->
        if (uri != null) {
            status = "Reading the receipt…"
            perform { onReceipt(uri) }
        }
    }
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.imePadding().verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Quick add · $dayLabel", style = Fm.text.titleLarge)
            Text(
                "Tap a food to log one serving. Star it to keep it in My foods, or read a grocery receipt to fill them.",
                style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary),
            )
            MealPicker(meal) { meal = it }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FmTextField(value = filter, onValueChange = { filter = it }, hint = "Filter foods", modifier = Modifier.weight(1f))
                AccountOutlinedButton("Read receipt", {
                    if (receiptUnavailable != null) {
                        status = receiptUnavailable
                    } else if (!busy) {
                        picker.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))
                    }
                }, icon = "receipt_long_outlined")
            }
            if (status.isNotEmpty()) Text(status, style = Fm.text.bodyMedium)
            for (section in CaloriesPrompt.sections(mine, frequent, common, filter)) {
                FmSectionHeader(section.title)
                if (section.foods.isEmpty()) {
                    Text(section.empty, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary), modifier = Modifier.padding(vertical = 4.dp))
                }
                for (food in section.foods) {
                    val name = jsonString(food["name"])
                    val saved = CaloriesPrompt.isMine(food, mine)
                    FmListRow(
                        title = name,
                        subtitle = CaloriesPrompt.meta(food),
                        trailing = {
                            if (section.kind == "mine") {
                                FmIconButton("close", "Remove $name from My foods", onPressed = {
                                    val next = CaloriesPrompt.withoutFood(mine, food)
                                    if (next != null) {
                                        perform {
                                            onSaveMine(next)
                                            "Removed $name from My foods."
                                        }
                                    }
                                })
                            } else {
                                FmIconButton(if (saved) "star" else "star_border", "Save $name to My foods", onPressed = {
                                    val next = CaloriesPrompt.withFood(mine, food)
                                    if (next == null) {
                                        status = "$name is already in My foods."
                                    } else {
                                        perform {
                                            onSaveMine(next)
                                            "Saved $name to My foods."
                                        }
                                    }
                                })
                            }
                        },
                        showChevron = false,
                        onTap = { perform { onLog(food, meal) } },
                    )
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun SearchSheet(api: CaloriesApi, onDismiss: () -> Unit, onChosen: (JsonObject) -> Unit) {
    var query by remember { mutableStateOf("") }
    val search = remember { FoodSearch() }
    var shown by remember { mutableIntStateOf(0) }
    val scope = rememberCoroutineScope()
    fun run(q: String) {
        val cached = search.begin(q)
        shown++
        if (cached != null) return
        scope.launch {
            try {
                val found = withContext(Dispatchers.IO) { api.searchFoods(q) }
                if (search.finished(q, found)) shown++
            } catch (thrown: CancellationException) {
                throw thrown
            } catch (_: Exception) {
                if (search.failed(q)) shown++
            }
        }
    }
    fun submit() {
        val first = search.submitted(query)
        if (first != null) {
            onChosen(first)
            return
        }
        val q = search.typed(query)
        if (q.length >= 2) run(q)
    }
    LaunchedEffect(query) {
        when (val step = search.changed(query)) {
            FoodSearchStep.Clear -> shown++
            FoodSearchStep.Keep -> {}
            is FoodSearchStep.Run -> {
                if (step.delayMs > 0) delay(step.delayMs)
                run(step.query)
            }
        }
    }
    val results = shown.let { search.results }
    val status = shown.let { search.status }
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.imePadding().verticalScroll(rememberScrollState()).padding(16.dp)) {
            Text(FoodSearch.TITLE, style = Fm.text.titleLarge)
            FmTextField(
                value = query,
                onValueChange = { query = it },
                hint = FoodSearch.HINT,
                autofocus = true,
                trailing = { FmIconButton("search", "Search", onPressed = { submit() }) },
                keyboardActions = androidx.compose.foundation.text.KeyboardActions(onSearch = { submit() }),
                keyboardOptions = KeyboardOptions(autoCorrectEnabled = false, imeAction = androidx.compose.ui.text.input.ImeAction.Search),
            )
            Text(status, style = Fm.text.labelSmall, modifier = Modifier.padding(vertical = 8.dp))
            for (food in results) {
                FmListRow(
                    title = jsonString(food["name"]),
                    subtitle = FoodSearch.subtitle(food),
                    showChevron = false,
                    onTap = { onChosen(food) },
                )
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun PortionSheet(product: JsonObject, onDismiss: () -> Unit, onAdd: (LinkedHashMap<String, Any?>) -> Unit) {
    val hasServing = product["perServing"] is Map<*, *>
    var basis by remember { mutableStateOf(if (hasServing) "serving" else "100g") }
    var amount by remember { mutableStateOf("1") }
    var meal by remember { mutableStateOf(Calories.guessMeal(LocalTime.now().hour)) }
    val values = Calories.portionValues(product, basis, amount, meal)
    val serving = jsonString(product["servingSize"])
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.imePadding().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(jsonString(product["name"]), style = Fm.text.titleLarge)
            Text(Calories.productSource(product), style = Fm.text.labelSmall)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                val options = buildList {
                    if (hasServing) add("serving" to "1 serving${if (serving.isEmpty()) "" else " ($serving)"}")
                    add("100g" to "100 g")
                    add("1g" to "1 g")
                }
                for ((value, label) in options) {
                    FmToneButton(label, onPressed = { basis = value }, tone = if (basis == value) FmTone.Accent else FmTone.Neutral, filled = basis == value)
                }
            }
            NumberField("Amount", amount, "") { amount = it }
            MealPicker(meal) { meal = it }
            Text(
                "${values["calories"]} kcal · P ${Calories.grams(values["proteinG"])} g · C ${Calories.grams(values["carbsG"])} g · F ${Calories.grams(values["fatG"])} g",
                style = Fm.text.bodyMedium,
            )
            AccountFilledButton("Add to diary", "add", modifier = Modifier.fillMaxWidth(), onClick = { onAdd(Calories.portionValues(product, basis, amount, meal)) })
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ChallengeSheet(day: String, onDismiss: () -> Unit, onSave: (JsonObject) -> Unit) {
    var title by remember { mutableStateOf("") }
    var description by remember { mutableStateOf("") }
    var kind by remember { mutableStateOf("weight_loss_percent") }
    var starts by remember { mutableStateOf(day) }
    var ends by remember { mutableStateOf(Calories.isoDay(Calories.parseDay(day).plusDays(28))) }
    var invite by remember { mutableStateOf("") }
    var picking by remember { mutableStateOf<Boolean?>(null) }
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.imePadding().verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("New challenge", style = Fm.text.titleLarge)
            FmTextField(value = title, onValueChange = { title = it }, hint = "Title", autofocus = true)
            for ((value, label) in Calories.KIND_LABELS) {
                Row(Modifier.fillMaxWidth().clickable { kind = value }.padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                    androidx.compose.material3.RadioButton(selected = kind == value, onClick = { kind = value })
                    Text(label, style = Fm.text.bodyMedium)
                }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                AccountOutlinedButton("Starts ${Calories.dayLabel(starts)}", { picking = true }, modifier = Modifier.weight(1f))
                AccountOutlinedButton("Ends ${Calories.dayLabel(ends)}", { picking = false }, modifier = Modifier.weight(1f))
            }
            FmTextField(value = description, onValueChange = { description = it }, hint = "Description", minLines = 2, maxLines = 2)
            FmTextField(value = invite, onValueChange = { invite = it }, hint = "Invite friends: ForkMesh usernames, e.g. maya, @sam")
            AccountFilledButton("Save challenge", "check", modifier = Modifier.fillMaxWidth(), onClick = {
                if (title.trim().isEmpty()) return@AccountFilledButton
                onSave(
                    linkedMapOf(
                        "title" to title.trim(),
                        "kind" to kind,
                        "startsOn" to starts,
                        "endsOn" to ends,
                        "description" to description,
                        "invite" to Calories.usernames(invite),
                    ),
                )
            })
        }
    }
    picking?.let { start ->
        DayPickerDialog(if (start) starts else ends, 400, onDismiss = { picking = null }) {
            if (start) starts = it else ends = it
            picking = null
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
private fun BoardSheet(challenge: JsonObject, me: String, onDismiss: () -> Unit, onAction: (String) -> Unit) {
    val board = jsonObjects(challenge["leaderboard"])
    val pending = jsonObjects(challenge["pending"])
    val status = jsonString(challenge["status"])
    val mine = jsonString(challenge["myStatus"])
    val owner = challenge["isCreator"] == true
    val max = Calories.boardMax(board)
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(jsonString(challenge["title"]), style = Fm.text.titleLarge)
            Text(Calories.boardSummary(challenge), style = Fm.text.labelSmall)
            if (jsonString(challenge["description"]).isNotEmpty()) Text(jsonString(challenge["description"]), style = Fm.text.bodyMedium)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                if (status != "closed" && mine == "invited") {
                    FmToneButton("Accept challenge", onPressed = { onAction("accept") }, tone = FmTone.Accent, filled = true)
                    FmToneButton("Decline", onPressed = { onAction("decline") })
                }
                if (status != "closed" && mine == "accepted" && !owner) FmToneButton("Leave challenge", onPressed = { onAction("decline") })
                if (status != "closed" && owner) {
                    FmToneButton("Invite", onPressed = { onAction("invite") })
                    FmToneButton("End now", onPressed = { onAction("close") })
                }
                if (owner) FmToneButton("Delete", onPressed = { onAction("delete") }, tone = FmTone.Danger)
            }
            if (board.isEmpty()) Text("Nobody has accepted yet.", style = Fm.text.bodyMedium)
            for (member in board) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(if (Calories.int(member["rank"]) > 0) jsonString(member["rank"]) else "–", modifier = Modifier.width(24.dp))
                    Column(Modifier.weight(1f)) {
                        val name = jsonString(member["name"])
                        Text("$name${if (name == me) " (you)" else ""}", style = Fm.text.bodyMedium)
                        val ratio = if (member["measured"] == true) (Calories.num(member["score"]) / max).coerceIn(0.0, 1.0) else 0.0
                        FmLinearProgressIndicator(progress = { ratio.toFloat() }, modifier = Modifier.fillMaxWidth())
                    }
                    Spacer(Modifier.width(8.dp))
                    Text(jsonString(member["detail"]), style = Fm.text.labelSmall)
                }
            }
            if (pending.isNotEmpty()) {
                Text("Invited or left: ${pending.joinToString(", ") { "${jsonString(it["name"])} (${jsonString(it["status"])})" }}", style = Fm.text.labelSmall)
            }
        }
    }
}
