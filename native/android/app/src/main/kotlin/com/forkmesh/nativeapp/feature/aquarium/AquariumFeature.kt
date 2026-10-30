package com.forkmesh.nativeapp.feature.aquarium

import com.forkmesh.nativeapp.aquarium.AquariumSwimmer
import com.forkmesh.nativeapp.aquarium.AquariumTank
import com.forkmesh.nativeapp.aquarium.GenieLandmark
import com.forkmesh.nativeapp.feature.market.PackageGateFeature
import com.forkmesh.nativeapp.market.PhonePackages
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

object AquariumFeature {
    private val onState = MutableStateFlow(false)
    private val swimmersState = MutableStateFlow<List<AquariumSwimmer>>(emptyList())
    private val landmarksState = MutableStateFlow<List<GenieLandmark>>(emptyList())

    val on: StateFlow<Boolean> = onState.asStateFlow()

    val swimmers: StateFlow<List<AquariumSwimmer>> = swimmersState.asStateFlow()

    val landmarks: StateFlow<List<GenieLandmark>> = landmarksState.asStateFlow()

    fun refresh() {
        onState.value = PackageGateFeature.deviceSwitch(PhonePackages.AQUARIUM) == true
    }

    fun setSwimmers(next: List<AquariumSwimmer>) {
        swimmersState.value = next.take(AquariumTank.MOST)
    }

    fun setLandmarks(next: List<GenieLandmark>) {
        landmarksState.value = next
    }

    fun clear() {
        swimmersState.value = emptyList()
        landmarksState.value = emptyList()
    }
}
