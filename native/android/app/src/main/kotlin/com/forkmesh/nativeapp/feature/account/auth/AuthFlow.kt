package com.forkmesh.nativeapp.feature.account.auth

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.LinkAnnotation
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextLinkStyles
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.R
import com.forkmesh.nativeapp.account.AuthCopy
import com.forkmesh.nativeapp.account.AuthFlowStart
import com.forkmesh.nativeapp.account.LoginFormState
import com.forkmesh.nativeapp.account.LoginSubmit
import com.forkmesh.nativeapp.account.authBackDismisses
import com.forkmesh.nativeapp.account.loginButtonLabel
import com.forkmesh.nativeapp.account.loginSubmit
import com.forkmesh.nativeapp.account.passwordSubmits
import com.forkmesh.nativeapp.account.signInOverAppCreatesPreviewSession
import com.forkmesh.nativeapp.api.AuthController
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFilledField
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.HGap
import com.forkmesh.nativeapp.feature.account.VGap
import com.forkmesh.nativeapp.feature.account.isDebugBuild
import com.forkmesh.nativeapp.feature.account.openExternalUrl
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmPanelHeader
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.LocalFmFieldLabel
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmColors
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmMotion
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlin.coroutines.cancellation.CancellationException
import kotlin.math.min
import kotlin.math.roundToInt
import kotlinx.coroutines.launch

@Composable
fun AuthFlow(
    onAuthenticated: () -> Unit,
    modifier: Modifier = Modifier,
    onContinueAsGuest: (() -> Unit)? = null,
    start: AuthFlowStart = AuthFlowStart.Welcome,
    onDismiss: (() -> Unit)? = null,
) {
    var mode by rememberSaveable { mutableStateOf(start) }
    var signupOpened by rememberSaveable { mutableStateOf(false) }
    val back: () -> Unit = {
        if (authBackDismisses(pushedOverApp = onDismiss != null)) onDismiss?.invoke() else mode = AuthFlowStart.Welcome
    }
    val duration = FmMotion.effective(220)
    AnimatedContent(
        targetState = mode,
        modifier = modifier.fillMaxSize(),
        transitionSpec = {
            val slideIn = tween<IntOffset>(duration, easing = FmMotion.easeOutCubic)
            val slideOut = tween<IntOffset>(duration, easing = FmMotion.easeInCubic)
            (
                fadeIn(tween(duration, easing = FmMotion.easeOutCubic)) +
                    slideInHorizontally(slideIn) { (it * 0.02f).roundToInt() }
                ) togetherWith (
                fadeOut(tween(duration, easing = FmMotion.easeInCubic)) +
                    slideOutHorizontally(slideOut) { (it * 0.02f).roundToInt() }
                )
        },
        label = "AuthFlow",
    ) { page ->
        when (page) {
            AuthFlowStart.Welcome -> WelcomePage(
                onLogin = { mode = AuthFlowStart.Login },
                onSignup = { mode = AuthFlowStart.Signup },
                onGuest = onContinueAsGuest,
            )
            AuthFlowStart.Login -> LoginPage(
                onAuthenticated = onAuthenticated,
                onSignup = { mode = AuthFlowStart.Signup },
                onBack = back,
                afterSignup = signupOpened,
            )
            AuthFlowStart.Signup -> SignupPage(
                onOpened = {
                    signupOpened = true
                    mode = AuthFlowStart.Login
                },
                onLogin = { mode = AuthFlowStart.Login },
                onBack = back,
            )
        }
    }
}

internal class AuthFlowScreen(
    private val start: AuthFlowStart,
    private val onClosed: () -> Unit,
) : Screen() {
    override val title: String get() = if (start == AuthFlowStart.Signup) "Sign up" else "Login"

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val close: () -> Unit = {
            if (navigator.top === this@AuthFlowScreen) navigator.pop()
        }
        AuthFlow(
            start = start,
            onDismiss = close,
            onAuthenticated = {
                val auth = Services.get<AuthService>()
                if (auth != null && signInOverAppCreatesPreviewSession(auth.isAuthenticated, isDebugBuild(context))) {
                    AppLifecycle.scope.launch { auth.authenticatePreview() }
                }
                close()
            },
        )
    }

    override fun onDisposed() {
        onClosed()
    }
}

@Composable
private fun WelcomePage(onLogin: () -> Unit, onSignup: () -> Unit, onGuest: (() -> Unit)?) {
    Box(
        Modifier
            .fillMaxSize()
            .drawBehind {
                val w = size.width
                val h = size.height
                drawRect(
                    Brush.radialGradient(
                        0f to Color(0xFFEFEAFF),
                        0.18f to Color(0xFF6F5AD9),
                        0.48f to Color(0xFF050505),
                        center = Offset(w * 0.175f, h * 0.09f),
                        radius = 1.2f * min(w, h),
                    ),
                )
                drawRect(
                    Brush.linearGradient(
                        0f to Color(0x00000000),
                        0.44f to Color(0xFF050505),
                        0.68f to Color(0xFF160F35),
                        1f to Color(0xFF050505),
                        start = Offset(w, 0f),
                        end = Offset(0f, h),
                    ),
                )
            },
    ) {
        Column(
            Modifier
                .fillMaxSize()
                .windowInsetsPadding(fmSafeAreaInsets())
                .padding(start = 28.dp, top = 28.dp, end = 28.dp, bottom = 24.dp),
        ) {
            androidx.compose.foundation.layout.Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                LogoImage(54.dp)
                androidx.compose.foundation.layout.Spacer(Modifier.weight(1f))
                com.forkmesh.nativeapp.feature.account.projects.ProjectChip()
            }
            androidx.compose.foundation.layout.Spacer(Modifier.weight(1f))
            Column(Modifier.widthIn(max = 460.dp)) {
                Text("Build open, sync easy", style = Fm.text.displayLarge.copy(color = Color.White))
                VGap(FmSpace.x5)
                Text(
                    "Private relay chat, repository mirrors, and signed collaboration from your phone.",
                    style = Fm.text.bodyLarge.copy(color = Color(0xFFD8D5DF)),
                )
            }
            VGap(34.dp)
            Row(Modifier.fillMaxWidth()) {
                OutlinedButton(
                    onClick = onLogin,
                    modifier = Modifier.weight(1f),
                    shape = RoundedCornerShape(percent = 50),
                    border = BorderStroke(1.4.dp, Color.White),
                    colors = ButtonDefaults.outlinedButtonColors(contentColor = Color.White),
                    contentPadding = PaddingValues(vertical = 18.dp),
                ) {
                    Text("Log in")
                }
                HGap(12.dp)
                Button(
                    onClick = onSignup,
                    modifier = Modifier.weight(1f),
                    shape = RoundedCornerShape(percent = 50),
                    colors = ButtonDefaults.buttonColors(containerColor = Color.White, contentColor = Color.Black),
                    contentPadding = PaddingValues(vertical = 18.dp),
                ) {
                    Text("Sign up")
                }
            }
            if (onGuest != null) {
                VGap(14.dp)
                Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                    TextButton(
                        onClick = onGuest,
                        modifier = Modifier
                            .defaultMinSize(minHeight = 48.dp)
                            .testTag("continue-as-guest"),
                        colors = ButtonDefaults.textButtonColors(contentColor = Color.White),
                        contentPadding = PaddingValues(horizontal = 12.dp),
                    ) {
                        Text(
                            "Explore as guest",
                            modifier = Modifier.weight(1f, fill = false),
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                        HGap(8.dp)
                        FmIcon("arrow_forward", size = 18.dp, tint = Color.White)
                    }
                }
            }
        }
    }
}

@Composable
private fun LoginPage(onAuthenticated: () -> Unit, onSignup: () -> Unit, onBack: () -> Unit, afterSignup: Boolean) {
    val auth = Services.get<AuthService>()
    val context = LocalContext.current
    val debug = remember(context) { isDebugBuild(context) }
    val scope = rememberCoroutineScope()
    var identifier by rememberSaveable { mutableStateOf("") }
    var password by rememberSaveable { mutableStateOf("") }
    var totp by rememberSaveable { mutableStateOf("") }
    var loading by remember { mutableStateOf(false) }
    var form by remember { mutableStateOf(if (afterSignup) LoginFormState().afterSignup() else LoginFormState()) }
    val revokedBanner by rememberSaveable { mutableStateOf(auth?.revokedBannerPending?.value == true) }
    LaunchedEffect(Unit) {
        if (revokedBanner) auth?.clearRevokedBanner()
    }

    fun submit() {
        when (val action = loginSubmit(identifier, password, totp, loading, debug)) {
            LoginSubmit.Busy -> Unit
            LoginSubmit.DesignPreview -> onAuthenticated()
            is LoginSubmit.Invalid -> form = form.invalid(action.hint)
            is LoginSubmit.Send -> {
                val service = auth ?: return
                loading = true
                form = form.sending()
                scope.launch {
                    try {
                        service.login(action.identifier, action.password, action.totp)
                        onAuthenticated()
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (error: Exception) {
                        form = form.failed(error)
                    } finally {
                        loading = false
                    }
                }
            }
        }
    }

    AuthPageScaffold(pageTitle = "Login", onBack = onBack) {
        VGap(74.dp)
        CenteredLogo(72.dp)
        VGap(28.dp)
        val projects = Services.get<com.forkmesh.nativeapp.feature.account.projects.ProjectsService>()
        val subtitle = remember(projects) {
            projects?.let { com.forkmesh.nativeapp.account.projects.loginSubtitle(it.projects.value, it.active.value) }
                ?: "Continue with your ForkMesh account."
        }
        AuthTitle(title = "Welcome back", subtitle = subtitle)
        VGap(48.dp)
        if (revokedBanner) {
            RevokedBanner()
            VGap(24.dp)
        }
        AuthField(
            label = "Email or username",
            value = identifier,
            onValueChange = { identifier = it },
            hint = "you@example.com",
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Email, imeAction = ImeAction.Next),
        )
        VGap(24.dp)
        AuthField(
            label = "Password",
            value = password,
            onValueChange = { password = it },
            hint = "Your password",
            obscure = true,
            keyboardOptions = KeyboardOptions(
                keyboardType = KeyboardType.Password,
                imeAction = if (form.showTotp) ImeAction.Next else ImeAction.Done,
            ),
            keyboardActions = if (passwordSubmits(form.showTotp)) KeyboardActions(onDone = { submit() }) else KeyboardActions.Default,
        )
        if (form.showTotp) {
            VGap(24.dp)
            AuthField(
                label = "Authenticator code",
                value = totp,
                onValueChange = { totp = it },
                hint = "123456",
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number, imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = { submit() }),
            )
        }
        if (form.hint.isNotEmpty()) {
            VGap(18.dp)
            AuthHint(text = form.hint, bad = form.bad)
        }
        VGap(42.dp)
        PrimaryAuthButton(label = loginButtonLabel(loading), onClick = if (loading) null else { { submit() } })
        VGap(18.dp)
        SecondaryAuthButton(label = AuthCopy.FORGOT_PASSWORD, onClick = { auth?.let { openExternalUrl(context, it.forgotPasswordUrl) } })
        VGap(8.dp)
        SecondaryAuthButton(label = "Create an account", onClick = onSignup)
        VGap(72.dp)
        TermsCopy()
    }
}

@Composable
private fun SignupPage(onOpened: () -> Unit, onLogin: () -> Unit, onBack: () -> Unit) {
    val context = LocalContext.current
    AuthPageScaffold(pageTitle = "Sign up", onBack = onBack) {
        VGap(58.dp)
        CenteredLogo(78.dp)
        VGap(28.dp)
        AuthTitle(
            title = "Create your account",
            subtitle = "For spam protection, new accounts are created only on the secure ForkMesh website.",
        )
        VGap(42.dp)
        Text(
            "The signup page issues a short-lived, one-use browser check. " +
                "After you confirm your email, return here and log in.",
            style = Fm.text.bodyLarge.copy(color = Fm.colors.textSecondary),
        )
        VGap(32.dp)
        PrimaryAuthButton(
            label = "Open secure signup",
            onClick = {
                val auth = Services.get<AuthService>()
                if (auth != null && openExternalUrl(context, auth.signupUrl)) onOpened()
            },
        )
        VGap(18.dp)
        SecondaryAuthButton(label = "I already have an account", onClick = onLogin)
        VGap(72.dp)
        TermsCopy()
    }
}

@Composable
private fun AuthPageScaffold(pageTitle: String, onBack: () -> Unit, content: @Composable ColumnScope.() -> Unit) {
    val c = Fm.colors
    Column(
        Modifier
            .fillMaxSize()
            .background(c.bgBase),
    ) {
        Box(
            Modifier
                .fillMaxWidth()
                .background(c.bgRaised)
                .windowInsetsPadding(fmSafeAreaInsets(bottom = false)),
        ) {
            FmPanelHeader(
                title = pageTitle,
                leading = {
                    FmIconButton(
                        icon = "chevron_left",
                        label = "Back",
                        onPressed = onBack,
                        color = c.textPrimary,
                        iconSize = 30.dp,
                    )
                },
                trailing = { com.forkmesh.nativeapp.feature.account.projects.ProjectChip(Modifier.padding(end = FmSpace.x3)) },
            )
        }
        BoxWithConstraints(
            Modifier
                .weight(1f)
                .fillMaxWidth()
                .windowInsetsPadding(fmSafeAreaInsets(top = false))
                .imePadding(),
        ) {
            val minHeight = maxHeight
            Column(
                Modifier
                    .fillMaxSize()
                    .verticalScroll(rememberScrollState()),
            ) {
                Box(
                    Modifier
                        .fillMaxWidth()
                        .heightIn(min = minHeight)
                        .padding(start = FmSpace.x4, top = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x5),
                    contentAlignment = Alignment.Center,
                ) {
                    Column(
                        Modifier
                            .widthIn(max = 520.dp)
                            .fillMaxWidth(),
                        content = content,
                    )
                }
            }
        }
    }
}

@Composable
private fun CenteredLogo(size: Dp) {
    Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
        LogoImage(size, shadow = true)
    }
}

@Composable
private fun LogoImage(size: Dp, shadow: Boolean = false) {
    val shape = RoundedCornerShape(size * 0.24f)
    val shadowColor = Color(0x1A000000)
    Image(
        painter = painterResource(R.drawable.fm_logo),
        contentDescription = null,
        contentScale = ContentScale.Fit,
        modifier = Modifier
            .testTag("forkmesh-auth-logo")
            .size(size)
            .then(if (shadow) Modifier.shadow(8.dp, shape, ambientColor = shadowColor, spotColor = shadowColor) else Modifier)
            .clip(shape)
            .background(Color.Black),
    )
}

@Composable
private fun AuthTitle(title: String, subtitle: String) {
    Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
        Text(title, modifier = Modifier.fillMaxWidth(), textAlign = TextAlign.Center, style = Fm.text.displaySmall)
        VGap(10.dp)
        Text(
            subtitle,
            modifier = Modifier.fillMaxWidth(),
            textAlign = TextAlign.Center,
            style = Fm.text.bodyLarge.copy(color = Fm.colors.textSecondary),
        )
    }
}

@Composable
private fun AuthField(
    label: String,
    value: String,
    onValueChange: (String) -> Unit,
    hint: String,
    obscure: Boolean = false,
    keyboardOptions: KeyboardOptions = KeyboardOptions.Default,
    keyboardActions: KeyboardActions = KeyboardActions.Default,
) {
    Column(Modifier.fillMaxWidth()) {
        Text(label, modifier = Modifier.clearAndSetSemantics {}, style = Fm.text.titleSmall)
        VGap(FmSpace.x2)
        CompositionLocalProvider(LocalFmFieldLabel provides label) {
            AccountFilledField(
                value = value,
                onValueChange = onValueChange,
                hint = hint,
                fillColor = Fm.colors.bgRaised,
                verticalPadding = FmSpace.x4,
                obscure = obscure,
                keyboardOptions = keyboardOptions,
                keyboardActions = keyboardActions,
            )
        }
    }
}

@Composable
private fun RevokedBanner() {
    val color = Fm.colors.warning
    Row(
        Modifier
            .fillMaxWidth()
            .semantics { liveRegion = LiveRegionMode.Polite }
            .background(color.copy(alpha = 28f / 255f), RoundedCornerShape(FmRadius.md))
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
        verticalAlignment = Alignment.Top,
    ) {
        FmIcon("phonelink_erase_outlined", size = 18.dp, tint = color)
        HGap(FmSpace.x3)
        Column(Modifier.weight(1f)) {
            Text(AuthController.REVOKED_BANNER_MESSAGE, style = Fm.text.bodyMedium.copy(fontWeight = FmType.emphasis))
            VGap(2.dp)
            Text(AuthCopy.REVOKED_DETAIL, style = Fm.text.bodySmall)
        }
    }
}

@Composable
private fun AuthHint(text: String, bad: Boolean) {
    val c = Fm.colors
    Text(
        text,
        style = Fm.text.bodyMedium.copy(color = if (bad) c.danger else c.success, fontWeight = FmType.medium),
    )
}

@Composable
private fun PrimaryAuthButton(label: String, onClick: (() -> Unit)?) {
    val c = Fm.colors
    Button(
        onClick = { onClick?.invoke() },
        enabled = onClick != null,
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 48.dp)
            .testTag("primary-auth-button-frame"),
        shape = RoundedCornerShape(FmRadius.md),
        colors = ButtonDefaults.buttonColors(
            containerColor = c.textPrimary,
            contentColor = if (c.dark) FmColors.darkBgBase else FmColors.bgRaised,
        ),
        elevation = null,
        contentPadding = PaddingValues(horizontal = FmSpace.x4, vertical = FmSpace.x3),
    ) {
        Text(label, textAlign = TextAlign.Center, style = MaterialTheme.typography.titleMedium)
    }
}

@Composable
private fun SecondaryAuthButton(label: String, onClick: () -> Unit) {
    FmTextButton(
        onClick = onClick,
        modifier = Modifier.fillMaxWidth(),
        contentColor = Fm.colors.textPrimary,
        textStyle = MaterialTheme.typography.labelLarge,
    ) {
        Text(label)
    }
}

@Composable
private fun TermsCopy() {
    val context = LocalContext.current
    val c = Fm.colors
    val linkStyles = TextLinkStyles(SpanStyle(color = c.accent, textDecoration = TextDecoration.Underline))
    val text = buildAnnotatedString {
        append("By continuing, you agree to our ")
        pushLink(LinkAnnotation.Clickable("terms", linkStyles) { openExternalUrl(context, com.forkmesh.nativeapp.feature.account.projects.ProjectActions.termsUrl()) })
        append("Terms of Service")
        pop()
        append(" and have read our ")
        pushLink(LinkAnnotation.Clickable("privacy", linkStyles) { openExternalUrl(context, com.forkmesh.nativeapp.feature.account.projects.ProjectActions.privacyUrl()) })
        append("Privacy Policy")
        pop()
        append(".")
    }
    Text(
        text,
        modifier = Modifier
            .fillMaxWidth()
            .padding(start = 10.dp, top = 24.dp, end = 10.dp),
        textAlign = TextAlign.Start,
        style = Fm.text.bodyMedium.copy(color = c.textSecondary),
    )
}
