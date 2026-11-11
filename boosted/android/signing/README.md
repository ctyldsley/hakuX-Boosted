# The fork's debug signing key

`debug.keystore` signs every debug APK that GitHub Actions builds (the build
copies it to `~/.android/debug.keystore`, where the Android Gradle plugin
looks). It is a debug key with Android's standard debug passwords (store and
key `android`, alias `androiddebugkey`): not a secret, and never to be used
for a release.

It is here so that each build installs over the previous one. A runner's own
debug key is new on every run, and Android refuses an update signed with a
different key: the app then has to be uninstalled first, with its saves and
settings.

To build by hand with the same key, copy the file to `~/.android/` too.
