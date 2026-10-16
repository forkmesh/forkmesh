package com.forkmesh.nativeapp.facechanger

import java.util.concurrent.CopyOnWriteArrayList

data class FaceChangerOption(val id: String, val label: String)

enum class FaceChangerPart { FACE, BACKGROUND, VOICE }

class FaceChangerLook {
    companion object {
        const val FACE_PREF = "chat.callFace"
        const val VOICE_PREF = "chat.callVoice"
        const val BACKGROUND_PREF = "chat.callBackground"

        val FACES = listOf(
            FaceChangerOption("maya", "Maya"),
            FaceChangerOption("theo", "Theo"),
            FaceChangerOption("kofi", "Kofi"),
            FaceChangerOption("ingrid", "Ingrid"),
            FaceChangerOption("ren", "Ren"),
        )
        const val CUSTOM_FACE = "custom"
        val FACE_CHOICES: List<FaceChangerOption> by lazy { FACES + FaceChangerOption(CUSTOM_FACE, "Your photo") }

        fun faceLabel(id: String): String = FACE_CHOICES.firstOrNull { it.id == id }?.label ?: "Your own face"

        const val CUSTOM_BACKGROUND = "custom"
        val BACKGROUNDS = listOf(
            FaceChangerOption("", "No background"),
            FaceChangerOption("blur", "Blur"),
            FaceChangerOption("blur-strong", "Strong blur"),
            FaceChangerOption("world", "ForkMesh World"),
            FaceChangerOption("office", "ForkMesh office"),
            FaceChangerOption("meeting", "Meeting room"),
            FaceChangerOption("library", "Home library"),
            FaceChangerOption("skyline", "City window"),
            FaceChangerOption(CUSTOM_BACKGROUND, "Your photo"),
        )

        fun backgroundLabel(id: String): String = BACKGROUNDS.firstOrNull { it.id == id }?.label ?: "No background"

        const val VOICE_MATCH_FACE = "face"
        const val VOICE_OFF = "off"
        val VOICES = listOf(
            FaceChangerOption("maya", "Maya"),
            FaceChangerOption("theo", "Theo"),
            FaceChangerOption("kofi", "Kofi"),
            FaceChangerOption("ingrid", "Ingrid"),
            FaceChangerOption("ren", "Ren"),
            FaceChangerOption("deep", "Deep man"),
            FaceChangerOption("older-man", "Older man"),
            FaceChangerOption("young-man", "Young man"),
            FaceChangerOption("warm", "Warm woman"),
            FaceChangerOption("low", "Low woman"),
            FaceChangerOption("older-woman", "Older woman"),
            FaceChangerOption("teen", "Teen"),
            FaceChangerOption("neutral", "Neutral"),
            FaceChangerOption("robot", "Robot"),
        )

        fun voiceLabel(id: String): String = VOICES.firstOrNull { it.id == id }?.label ?: "Your own"
    }

    @Volatile
    var onFaceChanged: ((String) -> Unit)? = null

    @Volatile
    var onFaceImageChanged: ((String) -> Unit)? = null

    @Volatile
    var onBackgroundChanged: ((String, String) -> Unit)? = null

    @Volatile
    var onVoiceChanged: ((String) -> Unit)? = null

    private val lock = Any()
    private val listeners = CopyOnWriteArrayList<(FaceChangerPart) -> Unit>()

    @Volatile
    var face: String = ""
        private set

    @Volatile
    var faceImage: String = ""
        private set

    @Volatile
    var background: String = ""
        private set

    @Volatile
    var backgroundImage: String = ""
        private set

    @Volatile
    var voiceChoice: String = VOICE_MATCH_FACE
        private set

    val effectiveVoice: String
        get() = when (val choice = voiceChoice) {
            VOICE_OFF -> ""
            VOICE_MATCH_FACE -> if (face == CUSTOM_FACE) "" else face
            else -> choice
        }

    val customFaceImage: String get() = if (face == CUSTOM_FACE) faceImage else ""

    val customBackgroundImage: String get() = if (background == CUSTOM_BACKGROUND) backgroundImage else ""

    fun addListener(listener: (FaceChangerPart) -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: (FaceChangerPart) -> Unit) {
        listeners.remove(listener)
    }

    private fun changed(part: FaceChangerPart) {
        for (listener in listeners) runCatching { listener(part) }
    }

    fun setFace(id: String, image: String = ""): Boolean {
        val custom = id == CUSTOM_FACE
        if (id.isNotEmpty() && !custom && FACES.none { it.id == id }) return false
        val picture: String
        val pictureChanged: Boolean
        synchronized(lock) {
            picture = if (custom && image.isNotEmpty()) image else faceImage
            if (custom && picture.isEmpty()) return false
            pictureChanged = picture != faceImage
            face = id
            faceImage = picture
        }
        if (pictureChanged) onFaceImageChanged?.invoke(picture)
        onFaceChanged?.invoke(id)
        changed(FaceChangerPart.FACE)
        return true
    }

    fun setBackground(id: String, image: String = ""): Boolean {
        if (BACKGROUNDS.none { it.id == id }) return false
        val custom = id == CUSTOM_BACKGROUND
        val picture: String
        synchronized(lock) {
            picture = if (custom && image.isNotEmpty()) image else backgroundImage
            if (custom && picture.isEmpty()) return false
            if (id == background && picture == backgroundImage) return true
            background = id
            backgroundImage = picture
        }
        onBackgroundChanged?.invoke(id, picture)
        changed(FaceChangerPart.BACKGROUND)
        return true
    }

    fun setVoiceChoice(choice: String): Boolean {
        val valid = choice == VOICE_OFF || choice == VOICE_MATCH_FACE || VOICES.any { it.id == choice }
        if (!valid) return false
        synchronized(lock) {
            if (voiceChoice == choice) return true
            voiceChoice = choice
        }
        onVoiceChanged?.invoke(choice)
        changed(FaceChangerPart.VOICE)
        return true
    }
}
