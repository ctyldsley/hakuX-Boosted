// hakuX-Boosted: the Xbox 360 core (XenDroid's :emulator-core) as a module of
// hakuX's Gradle build.
//
// This file only points at the sources in x360/emulator-core: the native
// build (CMake -> libe.so, libhardware_ProcessorInfo.so, adrenotools' hook
// libraries) and the JNI-bound Java classes. x360/emulator-core/build.gradle
// is XenDroid's own build file for the same sources (its standalone build);
// keep the two in step when XenDroid's changes.
plugins {
  id("com.android.library")
}

val x360Core = rootProject.file("../x360/emulator-core")

android {
  // XenDroid's: its Java sources import xendroid.compose.core.R.
  namespace = "xendroid.compose.core"
  compileSdk = 36
  ndkVersion = "29.0.14206865"

  defaultConfig {
    // 29+ gives native ELF TLS (thread_local is a register read, not an
    // emutls call). hakuX's own minSdk stays 28: the app's manifest overrides
    // this library's, and the 360 side is offered on Android 10 and later
    // only (boosted/X360.kt).
    minSdk = 29

    consumerProguardFiles(File(x360Core, "consumer-rules.pro"))

    ndk {
      abiFilters += listOf("arm64-v8a")
    }
    externalNativeBuild {
      cmake {
        // As x360/emulator-core/build.gradle: host code optimized in every
        // variant, -g kept for simpleperf, -fno-strict-aliasing for the
        // raw-memory-reinterpretation code.
        cFlags("-O2", "-DNDEBUG", "-g", "-fno-strict-aliasing")
        cppFlags("-O2", "-DNDEBUG", "-g", "-fno-strict-aliasing")
        if (System.getenv("USE_CCACHE") == "true") {
          arguments(
            "-DCMAKE_C_COMPILER_LAUNCHER=ccache",
            "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache",
          )
        }
      }
    }
  }

  sourceSets.getByName("main") {
    manifest.srcFile(File(x360Core, "src/main/AndroidManifest.xml"))
    java.setSrcDirs(listOf(File(x360Core, "src/main/java")))
    res.setSrcDirs(listOf(File(x360Core, "src/main/res")))
    assets.setSrcDirs(listOf(File(x360Core, "src/main/assets")))
  }

  externalNativeBuild {
    cmake {
      path = File(x360Core, "src/main/cpp/CMakeLists.txt")
      version = "3.30.3"
    }
  }

  compileOptions {
    sourceCompatibility = JavaVersion.VERSION_11
    targetCompatibility = JavaVersion.VERSION_11
  }
}

dependencies {
  // api: the JNI bridge uses DocumentFile, the UI module WindowCompat.
  api("androidx.appcompat:appcompat:1.6.1")
  api("androidx.documentfile:documentfile:1.0.1")
}
