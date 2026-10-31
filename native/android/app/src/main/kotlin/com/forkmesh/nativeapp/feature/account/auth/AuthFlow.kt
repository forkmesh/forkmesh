package com.forkmesh.nativeapp.feature.account.auth

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.LinkAnnotation
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextLinkStyles
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
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
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.HGap
import com.forkmesh.nativeapp.feature.account.VGap
import com.forkmesh.nativeapp.feature.account.isDebugBuild
import com.forkmesh.nativeapp.feature.account.openExternalUrl
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.ui.kit.FmAuroraBackground
import com.forkmesh.nativeapp.ui.kit.FmForcedDark
import com.forkmesh.nativeapp.ui.kit.FmGlassIcon
import com.forkmesh.nativeapp.ui.kit.FmOnboarding
import com.forkmesh.nativeapp.ui.kit.FmOnboardingBackButton
import com.forkmesh.nativeapp.ui.kit.FmPillButton
import com.forkmesh.nativeapp.ui.kit.FmPillField
import com.forkmesh.nativeapp.ui.kit.FmPillKind
import com.forkmesh.nativeapp.ui.kit.FmPillTextButton
import com.forkmesh.nativeapp.ui.kit.FmRevealLine
import com.forkmesh.nativeapp.ui.kit.FmRevealTitle
import com.forkmesh.nativeapp.ui.kit.fmBrighten
import com.forkmesh.nativeapp.ui.kit.fmReveal
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmMotion
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlin.coroutines.cancellation.CancellationException
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
    FmForcedDark {
    Box(modifier.fillMaxSize().background(Color.Black)) {
    FmAuroraBackground()
    AnimatedContent(
        targetState = mode,
        modifier = Modifier.fillMaxSize(),
        transitionSpec = {
            fadeIn(tween(duration, easing = FmMotion.easeOutCubic)) togetherWith
                fadeOut(tween(duration, easing = FmMotion.easeInCubic))
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
    Column(
        Modifier
            .fillMaxSize()
            .windowInsetsPadding(fmSafeAreaInsets())
            .padding(start = 28.dp, top = FmSpace.x3, end = 28.dp, bottom = FmSpace.x3),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Row(Modifier.fillMaxWidth()) {
            Spacer(Modifier.weight(1f))
            com.forkmesh.nativeapp.feature.account.projects.ProjectChip()
        }
        Spacer(Modifier.weight(1f))
        FmGlassIcon(148.dp, Modifier.fmReveal(0, rise = 26.dp, scale = 0.9f))
        Spacer(Modifier.weight(1f))
        Column(Modifier.widthIn(max = 460.dp).fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
            FmRevealTitle(AuthCopy.WELCOME_LINES, style = Fm.text.displayLarge, delayMs = 160)
            VGap(FmSpace.x4)
            FmRevealLine(
                AuthCopy.WELCOME_BODY,
                style = Fm.text.bodyMedium,
                color = FmOnboarding.secondaryText,
                textAlign = TextAlign.Center,
                delayMs = 460,
                durationMs = 600,
            )
            VGap(30.dp)
            Column(Modifier.fillMaxWidth().fmBrighten(300), verticalArrangement = Arrangement.spacedBy(14.dp)) {
                FmPillButton("Log in", onLogin)
                FmPillButton("Sign up", onSignup)
                if (onGuest != null) {
                    FmPillButton("Explore as guest", onGuest, Modifier.testTag("continue-as-guest"), kind = FmPillKind.Dark)
                }
            }
        }
        VGap(FmSpace.x5)
        TermsCopy(Modifier.fmReveal(600, rise = 0.dp, scale = 1f))
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
        VGap(38.dp)
        CenteredLogo(92.dp)
        VGap(26.dp)
        val projects = Services.get<com.forkmesh.nativeapp.feature.account.projects.ProjectsService>()
        val subtitle = remember(projects) {
            projects?.let { com.forkmesh.nativeapp.account.projects.loginSubtitle(it.projects.value, it.active.value) }
                ?: "Continue with your ForkMesh account."
        }
        AuthTitle(title = "Welcome back", subtitle = subtitle)
        VGap(34.dp)
        if (revokedBanner) {
            RevokedBanner()
            VGap(24.dp)
        }
        Column(Modifier.fmReveal(280, rise = 14.dp, scale = 1f), verticalArrangement = Arrangement.spacedBy(24.dp)) {
            FmPillField(
                label = "Email or username",
                value = identifier,
                onValueChange = { identifier = it },
                hint = "you@example.com",
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Email, imeAction = ImeAction.Next),
            )
            FmPillField(
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
                FmPillField(
                    label = "Authenticator code",
                    value = totp,
                    onValueChange = { totp = it },
                    hint = "123456",
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number, imeAction = ImeAction.Done),
                    keyboardActions = KeyboardActions(onDone = { submit() }),
                )
            }
        }
        if (form.hint.isNotEmpty()) {
            VGap(18.dp)
            AuthHint(text = form.hint, bad = form.bad)
        }
        VGap(30.dp)
        PrimaryAuthButton(label = loginButtonLabel(loading), onClick = if (loading) null else { { submit() } })
        VGap(12.dp)
        SecondaryAuthButton(label = AuthCopy.FORGOT_PASSWORD, delayMs = 500, onClick = { auth?.let { openExternalUrl(context, it.forgotPasswordUrl) } })
        SecondaryAuthButton(label = "Create an account", delayMs = 560, onClick = onSignup)
        VGap(20.dp)
        TermsCopy(Modifier.fmReveal(620, rise = 0.dp, scale = 1f))
    }
}

@Composable
private fun SignupPage(onOpened: () -> Unit, onLogin: () -> Unit, onBack: () -> Unit) {
    val context = LocalContext.current
    AuthPageScaffold(pageTitle = "Sign up", onBack = onBack) {
        VGap(58.dp)
        CenteredLogo(110.dp)
        VGap(28.dp)
        AuthTitle(title = AuthCopy.SIGNUP_TITLE, subtitle = AuthCopy.SIGNUP_SUBTITLE)
        VGap(26.dp)
        FmRevealLine(
            AuthCopy.SIGNUP_BODY,
            Modifier.fillMaxWidth(),
            style = Fm.text.bodyMedium,
            color = FmOnboarding.secondaryText,
            textAlign = TextAlign.Center,
            delayMs = 400,
            durationMs = 600,
        )
        VGap(32.dp)
        PrimaryAuthButton(
            label = "Open secure signup",
            onClick = {
                val auth = Services.get<AuthService>()
                if (auth != null && openExternalUrl(context, auth.signupUrl)) onOpened()
            },
        )
        VGap(12.dp)
        SecondaryAuthButton(label = "I already have an account", delayMs = 500, onClick = onLogin)
        VGap(28.dp)
        TermsCopy(Modifier.fmReveal(580, rise = 0.dp, scale = 1f))
    }
}

@Composable
private fun AuthPageScaffold(pageTitle: String, onBack: () -> Unit, content: @Composable ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxSize()) {
        Row(
            Modifier
                .fillMaxWidth()
                .windowInsetsPadding(fmSafeAreaInsets(bottom = false))
                .padding(start = 20.dp, top = FmSpace.x2, end = 20.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmOnboardingBackButton(onClick = onBack)
            HGap(FmSpace.x3)
            Text(
                pageTitle,
                modifier = Modifier.semantics { heading() },
                style = Fm.text.titleMedium.copy(color = Color.White, fontWeight = FontWeight.SemiBold),
            )
            Spacer(Modifier.weight(1f))
            com.forkmesh.nativeapp.feature.account.projects.ProjectChip()
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
                        .padding(start = 24.dp, end = 24.dp, bottom = FmSpace.x5),
                    contentAlignment = Alignment.TopCenter,
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
    Box(Modifier.fillMaxWidth().testTag("forkmesh-auth-logo"), contentAlignment = Alignment.Center) {
        FmGlassIcon(size, Modifier.fmReveal(0, rise = 22.dp, scale = 0.9f))
    }
}

@Composable
private fun AuthTitle(title: String, subtitle: String) {
    Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
        FmRevealTitle(listOf(title), delayMs = 100)
        VGap(10.dp)
        FmRevealLine(
            subtitle,
            style = Fm.text.bodyMedium,
            color = Color.White.copy(alpha = 0.72f),
            textAlign = TextAlign.Center,
            delayMs = 240,
            durationMs = 600,
        )
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
    FmPillButton(label, onClick, Modifier.testTag("primary-auth-button-frame").fmBrighten(380))
}

@Composable
private fun SecondaryAuthButton(label: String, delayMs: Int, onClick: () -> Unit) {
    Box(Modifier.fillMaxWidth().fmReveal(delayMs, rise = 0.dp, scale = 1f), contentAlignment = Alignment.Center) {
        FmPillTextButton(label, onClick)
    }
}

@Composable
private fun TermsCopy(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val linkStyles = TextLinkStyles(SpanStyle(color = Color.White))
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
        modifier = modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp),
        textAlign = TextAlign.Center,
        style = Fm.text.bodySmall.copy(color = FmOnboarding.secondaryText),
    )
}
