package com.forkmesh.nativeapp.feature.tools.vault

import android.app.PendingIntent
import android.content.Intent
import android.os.Build
import android.os.CancellationSignal
import android.service.autofill.AutofillService
import android.service.autofill.Dataset
import android.service.autofill.FillCallback
import android.service.autofill.FillRequest
import android.service.autofill.FillResponse
import android.service.autofill.SaveCallback
import android.service.autofill.SaveInfo
import android.service.autofill.SaveRequest
import android.view.autofill.AutofillId
import android.widget.RemoteViews
import androidx.annotation.RequiresApi
import com.forkmesh.nativeapp.R

@RequiresApi(Build.VERSION_CODES.O)
open class VaultAutofillService : AutofillService() {
    override fun onFillRequest(request: FillRequest, cancel: CancellationSignal, callback: FillCallback) {
        val structure = request.fillContexts.lastOrNull()?.structure
        val form = structure?.let(VaultLoginForm::from)
        if (form == null || !form.fillable || form.packageName == packageName) {
            callback.onSuccess(null)
            return
        }
        val store = VaultAutofillStore(this)
        val logins = store.logins()
        if (logins.isEmpty()) {
            callback.onSuccess(null)
            return
        }
        val host = form.trustedHost
        val matches = when {
            host.isNotEmpty() -> logins.filter { it.host == host }
            form.inBrowser -> emptyList()
            else -> store.links()[form.packageName].orEmpty().mapNotNull { id -> logins.firstOrNull { it.id == id } }
        }.take(MAX_SUGGESTIONS)

        val response = FillResponse.Builder()
        var code = 0
        for (login in matches) {
            response.addDataset(locked(form, row(login.title, login.username.ifEmpty { login.host }), login.id, host, code++))
        }
        response.addDataset(locked(
            form,
            row(getString(R.string.vault_autofill_search), if (host.isEmpty()) "" else host),
            "", host, code,
        ))
        form.passwordId?.let { passwordId ->
            val save = SaveInfo.Builder(
                SaveInfo.SAVE_DATA_TYPE_PASSWORD or SaveInfo.SAVE_DATA_TYPE_USERNAME, arrayOf(passwordId),
            )
            form.usernameId?.let { save.setOptionalIds(arrayOf(it)) }
            response.setSaveInfo(save.build())
        }
        callback.onSuccess(response.build())
    }

    override fun onSaveRequest(request: SaveRequest, callback: SaveCallback) {
        val form = request.fillContexts.lastOrNull()?.structure?.let(VaultLoginForm::from)
        val store = VaultAutofillStore(this)
        if (form == null || form.password.isEmpty() || !store.enabled) {
            callback.onFailure(getString(R.string.vault_autofill_open_app))
            return
        }
        val label = runCatching {
            packageManager.getApplicationLabel(packageManager.getApplicationInfo(form.packageName, 0)).toString()
        }.getOrDefault(form.packageName)
        store.addPending(mapOf(
            "title" to if (form.trustedHost.isEmpty()) label else form.trustedHost,
            "host" to form.trustedHost,
            "username" to form.username,
            "password" to form.password,
            "app" to if (form.inBrowser) "" else label,
        ))
        callback.onSuccess()
    }

    private fun locked(form: VaultLoginForm, presentation: RemoteViews, loginId: String, host: String, code: Int): Dataset {
        val intent = Intent(this, VaultAutofillActivity::class.java)
            .putExtra(VaultAutofillActivity.EXTRA_LOGIN_ID, loginId)
            .putExtra(VaultAutofillActivity.EXTRA_HOST, host)
            .putExtra(VaultAutofillActivity.EXTRA_PACKAGE, form.packageName)
            .putExtra(VaultAutofillActivity.EXTRA_USERNAME_ID, form.usernameId)
            .putExtra(VaultAutofillActivity.EXTRA_PASSWORD_ID, form.passwordId)
        val flags = PendingIntent.FLAG_CANCEL_CURRENT or
            (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0)
        val sender = PendingIntent.getActivity(this, REQUEST_BASE + code, intent, flags).intentSender
        @Suppress("DEPRECATION")
        val dataset = Dataset.Builder(presentation)
        for (id in listOfNotNull<AutofillId>(form.usernameId, form.passwordId)) {
            @Suppress("DEPRECATION")
            dataset.setValue(id, null, presentation)
        }
        return dataset.setAuthentication(sender).build()
    }

    private fun row(title: String, detail: String): RemoteViews =
        RemoteViews(packageName, R.layout.vault_autofill_row).apply {
            setTextViewText(R.id.vault_autofill_title, title)
            setTextViewText(R.id.vault_autofill_detail, detail)
        }

    private companion object {
        const val MAX_SUGGESTIONS = 4
        const val REQUEST_BASE = 48730
    }
}
