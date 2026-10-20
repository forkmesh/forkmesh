package com.forkmesh.nativeapp.feature.health

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.calories.CaloriesApi
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.health.FoodDiet
import com.forkmesh.nativeapp.health.FoodGoal
import com.forkmesh.nativeapp.health.FoodGuideModel
import com.forkmesh.nativeapp.health.FoodMealPlan
import com.forkmesh.nativeapp.health.FoodSuggestion
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class FoodGuideScreen : Screen() {
    override val title: String get() = FoodGuideModel.TITLE

    @Composable
    override fun Content(navigator: Navigator) {
        val model = remember {
            val api = Services.get<ApiService>()
            FoodGuideModel(api?.let { service -> { day: String -> CaloriesApi(service).overview(day) } }, Services.get<PrefStore>())
        }
        val ticker = remember { ModelTicker() }
        DisposableEffect(model) {
            model.onChange = ticker::bump
            onDispose { model.onChange = null }
        }
        val guide = ticker.track(model)
        val scope = rememberCoroutineScope()
        val foreground by AppLifecycle.foreground.collectAsState()

        fun reload() {
            scope.launch(Dispatchers.IO) { guide.load() }
        }

        LaunchedEffect(foreground) {
            if (foreground) withContext(Dispatchers.IO) { guide.load() }
        }

        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(title, navigator, actions = {
                FmIconButton("refresh", "Refresh", onPressed = ::reload)
            })
            if (!guide.loaded) {
                FmSkeletonList()
            } else {
                FmPullToRefreshBox(isRefreshing = guide.loading, onRefresh = ::reload, modifier = Modifier.fillMaxSize()) {
                    Column(
                        Modifier
                            .fillMaxSize()
                            .verticalScroll(rememberScrollState())
                            .windowInsetsPadding(fmSafeAreaInsets(top = false))
                            .padding(FmSpace.x4),
                        verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
                    ) {
                        Text(FoodGuideModel.INTRO, style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
                        BudgetCard(guide)
                        NeedsCard()
                        for (plan in guide.plans) MealSection(guide, plan)
                        DietCard(guide)
                        Text(FoodGuideModel.DISCLAIMER, style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
                    }
                }
            }
        }
    }
}

@Composable
private fun BudgetCard(model: FoodGuideModel) {
    val c = Fm.colors
    var open by remember { mutableStateOf(false) }
    FmCard(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Text("Today's calorie budget", style = Fm.text.titleMedium)
            Text(model.budget.line, style = Fm.text.bodyMedium)
            Text(model.budget.sourceLine, style = Fm.text.bodySmall)
            SfHairline()
            Row(
                Modifier
                    .fillMaxWidth()
                    .heightIn(min = 44.dp)
                    .clickable(role = Role.DropdownList) { open = true },
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("Goal", style = Fm.text.bodyLarge, modifier = Modifier.weight(1f))
                Text(model.goal.label, style = Fm.text.bodyLarge.copy(color = c.accent))
                Spacer(Modifier.width(FmSpace.x1))
                FmIcon("expand_more", size = 20.dp, tint = c.accent)
                FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                    for (goal in FoodGoal.entries) {
                        FmDropdownMenuItem(goal.label, onClick = {
                            open = false
                            model.setGoal(goal)
                        })
                    }
                }
            }
            Text(model.goal.line, style = Fm.text.bodySmall)
        }
    }
}

@Composable
private fun NeedsCard() {
    FmCard(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Text("What your diet needs", style = Fm.text.titleMedium)
            Text(FoodGuideModel.NEEDS_LINE, style = Fm.text.bodySmall)
        }
    }
}

@Composable
private fun DietCard(model: FoodGuideModel) {
    FmCard(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Text("Leave out", style = Fm.text.titleMedium)
            BoxWithConstraints(Modifier.fillMaxWidth()) {
                val columns = maxOf(1, ((maxWidth + FmSpace.x2) / (150.dp + FmSpace.x2)).toInt())
                Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    for (rowDiets in FoodDiet.entries.chunked(columns)) {
                        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            for (diet in rowDiets) {
                                DietChip(diet.label, diet in model.diets, Modifier.weight(1f)) { model.toggleDiet(diet) }
                            }
                            repeat(columns - rowDiets.size) { Spacer(Modifier.weight(1f)) }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun DietChip(label: String, on: Boolean, modifier: Modifier, onTap: () -> Unit) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val ink = if (on) c.onAccent else c.textPrimary
    val shape = RoundedCornerShape(50)
    Row(
        modifier
            .heightIn(min = 44.dp)
            .clip(shape)
            .background(if (on) c.accent else c.bgRaised)
            .then(if (on) Modifier else Modifier.border(BorderStroke(1.dp, c.border), shape))
            .clickable(role = Role.Checkbox) {
                haptics.tap()
                onTap()
            }
            .padding(horizontal = FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(if (on) "check_circle" else "radio_button_unchecked", size = 18.dp, tint = ink)
        Spacer(Modifier.width(FmSpace.x1))
        Text(label, maxLines = 1, style = Fm.text.bodyMedium.copy(fontSize = 14.sp, fontWeight = FontWeight.W500, color = ink))
    }
}

@Composable
private fun MealSection(model: FoodGuideModel, plan: FoodMealPlan) {
    val picks = model.suggestions(plan.meal)
    Column(Modifier.padding(top = FmSpace.x2), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        FmSectionHeader(plan.title, trailing = {
            Text(plan.line, maxLines = 2, textAlign = TextAlign.End, style = Fm.text.bodySmall)
        })
        if (picks.isEmpty()) {
            Text(
                "No ${plan.meal.raw} ideas match what you left out. Turn one off below to see more.",
                style = Fm.text.bodySmall,
            )
        }
        for (suggestion in picks) SuggestionCard(suggestion)
        if (picks.isNotEmpty()) {
            val haptics = FmHaptics.current
            FmTextButton(onClick = {
                haptics.tap()
                model.moreIdeas(plan.meal)
            }) {
                FmIcon("refresh", size = 18.dp)
                Spacer(Modifier.width(FmSpace.x2))
                Text("Show other ${plan.meal.raw} ideas")
            }
        }
    }
}

@Composable
private fun SuggestionCard(suggestion: FoodSuggestion) {
    val c = Fm.colors
    val uri = LocalUriHandler.current
    FmCard(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Text(suggestion.recipe.title, style = Fm.text.titleSmall)
            Text(suggestion.metaLine, style = Fm.text.bodySmall)
            Text(suggestion.nutrientLine, style = Fm.text.bodySmall.copy(color = c.textPrimary))
            for (reason in suggestion.reasons) {
                Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    FmIcon("check_circle", size = 16.dp, tint = c.success, modifier = Modifier.padding(top = 1.dp))
                    Text(reason, style = Fm.text.bodySmall.copy(color = c.textPrimary))
                }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                FmIcon("shopping_cart_outlined", size = 16.dp, tint = c.textSecondary, modifier = Modifier.padding(top = 1.dp))
                Text(suggestion.shoppingLine, style = Fm.text.bodySmall)
            }
            FmTextButton(onClick = { runCatching { uri.openUri(suggestion.recipe.url) } }, contentPadding = androidx.compose.foundation.layout.PaddingValues(0.dp)) {
                FmIcon("open_in_new", size = 18.dp)
                Spacer(Modifier.width(FmSpace.x2))
                Text("Open recipe")
            }
        }
    }
}
