package com.forkmesh.nativeapp.feature.account

import android.util.Log
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.OwnerCrypto
import com.forkmesh.nativeapp.security.OwnerKeys
import com.forkmesh.nativeapp.security.SecretStore

object SecurityBootstrap {
    private const val TAG = "ForkMeshSecurity"

    fun install(app: App, store: SecretStore) {
        runCatching { Identity.loadOrCreate(store) }
            .onSuccess { Services.put(DeviceIdentity::class.java, it) }
            .onFailure { Log.e(TAG, "device identity unavailable", it) }
        runCatching { OwnerCrypto.loadOrCreate(store) }
            .onSuccess { Services.put(OwnerKeys::class.java, it) }
            .onFailure { Log.e(TAG, "owner keys unavailable", it) }
    }
}
