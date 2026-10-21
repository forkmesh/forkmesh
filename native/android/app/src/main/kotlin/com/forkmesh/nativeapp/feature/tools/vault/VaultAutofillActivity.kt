package com.forkmesh.nativeapp.feature.tools.vault

import android.app.Activity
import android.app.AlertDialog
import android.app.KeyguardManager
import android.content.Context
import android.content.Intent
import android.hardware.biometrics.BiometricManager
import android.hardware.biometrics.BiometricPrompt
import android.os.Build
import android.os.Bundle
import android.os.CancellationSignal
import android.service.autofill.Dataset
import android.text.Editable
import android.text.TextWatcher
import android.util.TypedValue
import android.view.View
import android.view.WindowManager
import android.view.autofill.AutofillId
import android.view.autofill.AutofillManager
import android.view.autofill.AutofillValue
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.RemoteViews
import android.widget.TextView
import androidx.annotation.RequiresApi
import com.forkmesh.nativeapp.R
import com.forkmesh.nativeapp.ui.kit.FmSafeInput

@RequiresApi(Build.VERSION_CODES.O)
class VaultAutofillActivity : Activity() {
    private lateinit var store: VaultAutofillStore
    private val host: String get() = intent.getStringExtra(EXTRA_HOST).orEmpty()
    private val targetPackage: String get() = intent.getStringExtra(EXTRA_PACKAGE).orEmpty()
    private var problemView: TextView? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.setFlags(WindowManager.LayoutParams.FLAG_SECURE, WindowManager.LayoutParams.FLAG_SECURE)
        store = VaultAutofillStore(this)
        if (savedInstanceState == null) authenticate()
    }

    private fun authenticate() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            BiometricPrompt.Builder(this)
                .setTitle(getString(R.string.vault_autofill_prompt_title))
                .setSubtitle(getString(R.string.vault_autofill_prompt_subtitle))
                .setAllowedAuthenticators(
                    BiometricManager.Authenticators.BIOMETRIC_STRONG or BiometricManager.Authenticators.DEVICE_CREDENTIAL,
                )
                .build()
                .authenticate(CancellationSignal(), mainExecutor, object : BiometricPrompt.AuthenticationCallback() {
                    override fun onAuthenticationSucceeded(result: BiometricPrompt.AuthenticationResult?) = authenticated()
                    override fun onAuthenticationError(errorCode: Int, message: CharSequence?) = cancel()
                })
            return
        }
        val keyguard = getSystemService(Context.KEYGUARD_SERVICE) as KeyguardManager
        @Suppress("DEPRECATION")
        val confirm = keyguard.createConfirmDeviceCredentialIntent(
            getString(R.string.vault_autofill_prompt_title), getString(R.string.vault_autofill_prompt_subtitle),
        )
        if (confirm == null) cancel() else startActivityForResult(confirm, CONFIRM_REQUEST)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != CONFIRM_REQUEST) return
        if (resultCode == RESULT_OK) authenticated() else cancel()
    }

    private fun authenticated() {
        val chosen = intent.getStringExtra(EXTRA_LOGIN_ID).orEmpty()
        val login = store.logins().firstOrNull { it.id == chosen }
        if (login != null) fill(login) else showPicker()
    }

    private fun showPicker() {
        val logins = store.logins()
        val shown = logins.toMutableList()
        val labels = { list: List<VaultLogin> ->
            list.map { listOf(it.title, it.username, it.host).filter(String::isNotEmpty).joinToString("  ·  ") }
        }
        val adapter = ArrayAdapter(this, android.R.layout.simple_list_item_1, labels(shown).toMutableList())
        val search = EditText(this).apply {
            hint = getString(R.string.vault_autofill_search_hint)
            isSingleLine = true
            filters = filters + FmSafeInput.Filter
            importantForAutofill = android.view.View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS
        }
        val list = ListView(this).apply { this.adapter = adapter }
        val problem = problemText().apply { visibility = View.GONE }
        problemView = problem
        search.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(text: CharSequence?, start: Int, count: Int, after: Int) = Unit
            override fun onTextChanged(text: CharSequence?, start: Int, before: Int, count: Int) = Unit
            override fun afterTextChanged(text: Editable?) {
                val needle = text?.toString().orEmpty().trim().lowercase()
                shown.clear()
                shown += logins.filter { needle.isEmpty() || listOf(it.title, it.username, it.host).any { field -> needle in field.lowercase() } }
                adapter.clear()
                adapter.addAll(labels(shown))
            }
        })
        list.setOnItemClickListener { _, _, position, _ -> confirmThenFill(shown[position]) }
        setTitle(R.string.vault_autofill_search)
        setContentView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val pad = (16 * resources.displayMetrics.density).toInt()
            setPadding(pad, pad, pad, pad)
            addView(problem)
            addView(search)
            addView(list)
        })
    }

    private fun confirmThenFill(login: VaultLogin) {
        if (host.isEmpty() || login.host == host) {
            fill(login)
            return
        }
        AlertDialog.Builder(this)
            .setTitle(R.string.vault_autofill_mismatch_title)
            .setMessage(getString(R.string.vault_autofill_mismatch_message, login.title, login.host.ifEmpty { "—" }, host))
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(R.string.vault_autofill_mismatch_fill) { _, _ -> fill(login) }
            .show()
    }

    private fun fill(login: VaultLogin) {
        val password = try {
            store.password(login.id)
        } catch (error: Exception) {
            showProblem(R.string.vault_autofill_open_app)
            return
        }
        val usernameId: AutofillId? = parcelable(EXTRA_USERNAME_ID)
        val passwordId: AutofillId? = parcelable(EXTRA_PASSWORD_ID)
        val presentation = RemoteViews(packageName, R.layout.vault_autofill_row).apply {
            setTextViewText(R.id.vault_autofill_title, login.title)
            setTextViewText(R.id.vault_autofill_detail, login.username)
        }
        @Suppress("DEPRECATION")
        val dataset = Dataset.Builder(presentation)
        @Suppress("DEPRECATION")
        usernameId?.takeIf { login.username.isNotEmpty() }?.let { dataset.setValue(it, AutofillValue.forText(login.username)) }
        @Suppress("DEPRECATION")
        passwordId?.let { dataset.setValue(it, AutofillValue.forText(password)) }
        if (host.isEmpty() && targetPackage.isNotEmpty()) store.remember(targetPackage, login.id)
        setResult(RESULT_OK, Intent().putExtra(AutofillManager.EXTRA_AUTHENTICATION_RESULT, dataset.build()))
        finish()
    }

    private fun showProblem(message: Int) {
        val shown = problemView
        if (shown != null) {
            shown.setText(message)
            shown.visibility = View.VISIBLE
            return
        }
        setContentView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val pad = (16 * resources.displayMetrics.density).toInt()
            setPadding(pad, pad, pad, pad)
            addView(problemText().apply { setText(message) })
            addView(Button(this@VaultAutofillActivity).apply {
                setText(android.R.string.ok)
                setOnClickListener { cancel() }
            })
        })
    }

    private fun problemText(): TextView = TextView(this).apply {
        val colour = TypedValue()
        val resolved = theme.resolveAttribute(android.R.attr.colorError, colour, true)
        val isColour = colour.type in TypedValue.TYPE_FIRST_COLOR_INT..TypedValue.TYPE_LAST_COLOR_INT
        setTextColor(if (resolved && isColour) colour.data else 0xFFC93532.toInt())
        val pad = (8 * resources.displayMetrics.density).toInt()
        setPadding(0, 0, 0, pad)
    }

    private fun cancel() {
        setResult(RESULT_CANCELED)
        finish()
    }

    private inline fun <reified T : android.os.Parcelable> parcelable(name: String): T? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(name, T::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra(name)
        }

    companion object {
        const val EXTRA_LOGIN_ID = "forkmesh.vault.loginId"
        const val EXTRA_HOST = "forkmesh.vault.host"
        const val EXTRA_PACKAGE = "forkmesh.vault.package"
        const val EXTRA_USERNAME_ID = "forkmesh.vault.usernameId"
        const val EXTRA_PASSWORD_ID = "forkmesh.vault.passwordId"
        private const val CONFIRM_REQUEST = 4874
    }
}
