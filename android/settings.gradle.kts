pluginManagement {
  repositories {
    google()
    mavenCentral()
    gradlePluginPortal()
  }
}
dependencyResolutionManagement {
  repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
  repositories {
    google()
    mavenCentral()
  }
}
rootProject.name = "hakuX"
include(":app")

// hakuX-Boosted: the Xbox 360 side (x360/, XenDroid) as two library modules,
// defined in boosted/android (boosted/docs/ARCHITECTURE.md).
include(":x360-core", ":x360-ui")
project(":x360-core").projectDir = file("../boosted/android/x360-core")
project(":x360-ui").projectDir = file("../boosted/android/x360-ui")
