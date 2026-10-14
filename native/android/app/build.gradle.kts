import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

val storeApplicationId = "com.forkmesh.forkmesh"
val appIdOverride = (findProperty("forkmesh.appId") as String?)?.trim().orEmpty()
val resolvedApplicationId = appIdOverride.ifEmpty { "com.forkmesh.nativeapp" }
val versionCodeOverride = (findProperty("forkmesh.versionCode") as String?)?.trim()?.toIntOrNull()
val versionNameOverride = (findProperty("forkmesh.versionName") as String?)?.trim().orEmpty()
val changelogOverride = (findProperty("forkmesh.changelog") as String?)?.trim().orEmpty()
val releaseSigningEnvironment = mapOf(
    "storeFile" to System.getenv("FORKMESH_ANDROID_KEYSTORE"),
    "storePassword" to System.getenv("FORKMESH_ANDROID_KEYSTORE_PASSWORD"),
    "keyAlias" to System.getenv("FORKMESH_ANDROID_KEY_ALIAS"),
    "keyPassword" to System.getenv("FORKMESH_ANDROID_KEY_PASSWORD"),
)
val providedSigningValues = releaseSigningEnvironment.values.count { !it.isNullOrBlank() }
check(providedSigningValues == 0 || providedSigningValues == releaseSigningEnvironment.size) {
    "Android release signing environment is incomplete"
}
val releaseSigningConfigured = providedSigningValues == releaseSigningEnvironment.size

android {
    namespace = "com.forkmesh.nativeapp"
    compileSdk = 37

    defaultConfig {
        applicationId = resolvedApplicationId
        minSdk = 26
        targetSdk = 37
        versionCode = versionCodeOverride ?: 10400
        versionName = versionNameOverride.ifEmpty { "1.4.0" }
        manifestPlaceholders["appLabel"] = if (resolvedApplicationId == storeApplicationId) "ForkMesh" else "ForkMesh Native"
    }

    if (releaseSigningConfigured) {
        signingConfigs {
            create("release") {
                storeFile = file(releaseSigningEnvironment.getValue("storeFile")!!)
                storePassword = releaseSigningEnvironment.getValue("storePassword")
                keyAlias = releaseSigningEnvironment.getValue("keyAlias")
                keyPassword = releaseSigningEnvironment.getValue("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = when {
                releaseSigningConfigured -> signingConfigs.getByName("release")
                resolvedApplicationId == storeApplicationId -> null
                else -> signingConfigs.getByName("debug")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
    }
}

abstract class BundleChangelog : DefaultTask() {
    @get:InputFile
    abstract val source: RegularFileProperty

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @TaskAction
    fun copy() {
        val target = outputDir.get().asFile
        target.mkdirs()
        source.get().asFile.copyTo(target.resolve("changelog.json"), overwrite = true)
    }
}

val bundleChangelog = tasks.register<BundleChangelog>("bundleChangelog") {
    source.set(if (changelogOverride.isEmpty()) file("src/changelog/changelog.json") else file(changelogOverride))
}

abstract class BundleDocsLibrary : DefaultTask() {
    @get:InputFile
    abstract val source: RegularFileProperty

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @TaskAction
    fun copy() {
        val target = outputDir.get().asFile.resolve("docs")
        target.mkdirs()
        source.get().asFile.copyTo(target.resolve("library.json"), overwrite = true)
    }
}

val bundleDocsLibrary = tasks.register<BundleDocsLibrary>("bundleDocsLibrary") {
    source.set(rootProject.layout.projectDirectory.file("../ios/ForkMesh/Resources/DocsLibrary.json"))
}

androidComponents {
    onVariants { variant ->
        variant.sources.assets?.addGeneratedSourceDirectory(bundleChangelog, BundleChangelog::outputDir)
        variant.sources.assets?.addGeneratedSourceDirectory(bundleDocsLibrary, BundleDocsLibrary::outputDir)
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(project(":core"))

    implementation(platform("androidx.compose:compose-bom:2026.09.00"))
    implementation("androidx.activity:activity-compose:1.13.0")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui-tooling-preview")
    debugImplementation("androidx.compose.ui:ui-tooling")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.11.0")
}

tasks.configureEach {
    if (name == "packageRelease" && resolvedApplicationId == storeApplicationId && !releaseSigningConfigured) {
        doFirst {
            throw GradleException("$storeApplicationId release builds need the release signing environment")
        }
    }
}
