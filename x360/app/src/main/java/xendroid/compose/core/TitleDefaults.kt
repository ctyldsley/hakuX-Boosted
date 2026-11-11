package xendroid.compose.core

import android.content.Context
import android.util.Log
import java.io.File

/**
 * hakuX-Boosted: per-title default configs that ship with the app
 * (assets/title-defaults/<TITLEID>.config.toml, from boosted/x360-titles/config).
 *
 * A title's best-known settings (GAME_COMPAT.md) are otherwise something each
 * user has to find and type in. Each bundled file is written to
 * <storage_root>/config/<TITLEID>.config.toml, the path the core's
 * config::LoadGameConfig reads, once per revision of the file and only while
 * no per-game config exists there: a config the user made (the per-game
 * settings screen writes the same file) is never touched, and one they
 * deleted doesn't come back until the bundled file changes.
 */
object TitleDefaults {
    private const val TAG = "TitleDefaults"
    private const val ASSET_DIR = "title-defaults"
    private const val PREFS = "title_defaults"
    private const val SUFFIX = ".config.toml"

    /** Blocking (asset reads, small files): call before the core boots. */
    fun seed(context: Context, storageRoot: File) {
        val names = runCatching { context.assets.list(ASSET_DIR) }.getOrNull()
            ?.filter { it.endsWith(SUFFIX) } ?: return
        if (names.isEmpty()) return
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val configDir = File(storageRoot, "config")
        for (name in names) {
            runCatching {
                val text = context.assets.open("$ASSET_DIR/$name").use { it.readBytes() }
                val revision = text.contentHashCode()
                if (prefs.getInt(name, 0) == revision) return@runCatching
                val target = File(configDir, name)
                // An empty file is the settings screen's placeholder, not a choice.
                if (!target.exists() || target.length() == 0L) {
                    configDir.mkdirs()
                    target.writeBytes(text)
                    Log.i(TAG, "seeded ${target.name}")
                }
                prefs.edit().putInt(name, revision).apply()
            }.onFailure { Log.w(TAG, "seeding $name failed", it) }
        }
    }
}
