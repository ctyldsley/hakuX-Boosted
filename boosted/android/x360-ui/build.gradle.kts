// hakuX-Boosted: the Xbox 360 side's screens (XenDroid's :app: library,
// settings, the emulator's host activity) as a library module of hakuX's
// Gradle build.
//
// The sources are x360/app's; XenDroid builds them as an application
// (x360/app/build.gradle), here they are a library the hakuX app depends on.
// What differs is in this module's own files: the manifest (src/main: no
// launcher entry, no Application class) and BuildConfig.VERSION_NAME.
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
  id("com.android.library")
  id("org.jetbrains.kotlin.android")
  id("org.jetbrains.kotlin.plugin.compose")
  id("org.jetbrains.kotlin.plugin.serialization")
}

val x360App = rootProject.file("../x360/app")

// xenia-canary's game patches (the x360/patches/xenia-canary submodule) as
// assets/game-patches, as x360/app/build.gradle packs them.
val gamePatchesGenDir = layout.buildDirectory.dir("generated/gamePatches")
val syncGamePatches = tasks.register<Sync>("syncGamePatches") {
  from(rootProject.file("../x360/patches/xenia-canary/patches")) { include("*.patch.toml") }
  // hakuX-Boosted's own title patches and per-title defaults (boosted/x360-titles)
  from(rootProject.file("../boosted/x360-titles/patches")) { include("*.patch.toml") }
  into(gamePatchesGenDir.map { it.dir("game-patches") })
}
val titleDefaultsGenDir = layout.buildDirectory.dir("generated/titleDefaults")
val syncTitleDefaults = tasks.register<Sync>("syncTitleDefaults") {
  from(rootProject.file("../boosted/x360-titles/config")) { include("*.config.toml") }
  into(titleDefaultsGenDir.map { it.dir("title-defaults") })
}
tasks.named("preBuild") { dependsOn(syncGamePatches, syncTitleDefaults) }

android {
  // XenDroid's application namespace: its sources use xendroid.compose.BuildConfig.
  namespace = "xendroid.compose"
  compileSdk = 36

  defaultConfig {
    minSdk = 29
    // R8 runs in the hakuX app (debug builds too): XenDroid's app rules come
    // along as this library's.
    consumerProguardFiles(File(x360App, "proguard-rules.pro"))
    // An application's BuildConfig has it, a library's doesn't; the 360
    // side's About screen and its updater read it. Its updater is off inside
    // hakuX (core/Embedded.kt).
    buildConfigField("String", "VERSION_NAME", "\"hakuX-Boosted\"")
  }

  sourceSets.getByName("main") {
    java.setSrcDirs(listOf(File(x360App, "src/main/java")))
    // XenDroid's resources, and this module's own (src/main/res)
    res.setSrcDirs(listOf(File(x360App, "src/main/res"), file("src/main/res")))
    assets.setSrcDirs(
      listOf(
        File(x360App, "src/main/assets"),
        gamePatchesGenDir.get().asFile,
        titleDefaultsGenDir.get().asFile,
      )
    )
  }

  buildFeatures {
    compose = true
    buildConfig = true
  }

  compileOptions {
    sourceCompatibility = JavaVersion.VERSION_11
    targetCompatibility = JavaVersion.VERSION_11
  }
}

kotlin {
  compilerOptions {
    jvmTarget.set(JvmTarget.JVM_11)
  }
}

dependencies {
  api(project(":x360-core"))

  // Compose and the androidx libraries at the hakuX app's versions (one
  // version of each ends up in the APK).
  implementation(platform("androidx.compose:compose-bom:2025.06.00"))
  implementation("androidx.compose.ui:ui")
  implementation("androidx.compose.ui:ui-graphics")
  implementation("androidx.compose.ui:ui-tooling-preview")
  implementation("androidx.compose.material3:material3")
  implementation("androidx.activity:activity-compose:1.10.1")
  implementation("androidx.navigation:navigation-compose:2.9.0")
  implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.9.1")
  implementation("androidx.lifecycle:lifecycle-runtime-compose:2.9.1")
  implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.9.1")

  // XenDroid's own
  implementation("com.squareup.retrofit2:retrofit:2.11.0")
  implementation("com.squareup.retrofit2:converter-gson:2.11.0")
  implementation("com.squareup.okhttp3:logging-interceptor:4.12.0")
  implementation("io.coil-kt:coil-compose:2.7.0")
  implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.9.0")
  implementation("androidx.datastore:datastore-preferences:1.1.1")
}
