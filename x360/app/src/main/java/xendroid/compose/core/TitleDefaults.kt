package xendroid.compose.core

import android.content.Context
import android.util.Log
import java.io.File

/**
 * hakuX-Boosted: per-title defaults that ship with the app (assets/title-defaults,
 * from boosted/x360-titles).
 *
 * A title's best-known settings and required patches (GAME_COMPAT.md) are
 * otherwise something each user has to find and put in place. Two kinds:
 *
 * - config/<TITLEID>.config.toml, written to <storage_root>/config/, the path
 *   the core's config::LoadGameConfig reads;
 * - patches/<TITLEID> ... .patch.toml, written to <storage_root>/patches/,
 *   the directory the core's patcher scans. The same files are in the bundled
 *   patch catalog (assets/game-patches), so the title's patches screen shows
 *   and toggles them.
 *
 * Each bundled file is written once per revision of the file and only while
 * nothing of that name is there: a file the user made or changed (the
 * per-game settings and patches screens write the same files) is never
 * touched, and one they deleted doesn't come back until the bundled file
 * changes.
 */
object TitleDefaults {
    private const val TAG = "TitleDefaults"
    private const val ASSET_DIR = "title-defaults"
    private const val PREFS = "title_defaults"

    /** Blocking (asset reads, small files): call before the core boots. */
    fun seed(context: Context, storageRoot: File) {
        seedKind(context, "config", ".config.toml", File(storageRoot, "config"))
        seedKind(context, "patches", ".patch.toml", File(storageRoot, "patches"))
    }

    private fun seedKind(context: Context, kind: String, suffix: String, targetDir: File) {
        val assetDir = "$ASSET_DIR/$kind"
        val names = runCatching { context.assets.list(assetDir) }.getOrNull()
            ?.filter { it.endsWith(suffix) } ?: return
        if (names.isEmpty()) return
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        for (name in names) {
            runCatching {
                val text = context.assets.open("$assetDir/$name").use { it.readBytes() }
                val revision = text.contentHashCode()
                val key = "$kind/$name"
                if (prefs.getInt(key, 0) == revision) return@runCatching
                val target = File(targetDir, name)
                // An empty file is the settings screen's placeholder, not a choice.
                if (!target.exists() || target.length() == 0L) {
                    targetDir.mkdirs()
                    target.writeBytes(text)
                    Log.i(TAG, "seeded $kind/${target.name}")
                }
                prefs.edit().putInt(key, revision).apply()
            }.onFailure { Log.w(TAG, "seeding $kind/$name failed", it) }
        }
    }
}
