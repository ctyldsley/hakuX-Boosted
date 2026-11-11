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
 * - enable/<TITLEID>.txt, the names of patches of the bundled catalog
 *   (xenia-canary's, which all ship disabled) that are on by default for the
 *   title: each catalog file of the title is written to
 *   <storage_root>/patches/ with those patches enabled.
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
    /** The bundled patch catalog (patches/PatchAssets.kt). */
    private const val CATALOG_DIR = "game-patches"

    /** Blocking (asset reads, small files): call before the core boots. */
    fun seed(context: Context, storageRoot: File) {
        seedKind(context, "config", ".config.toml", File(storageRoot, "config"))
        seedKind(context, "patches", ".patch.toml", File(storageRoot, "patches"))
        enableCatalogPatches(context, File(storageRoot, "patches"))
    }

    /** The catalog's files of each title with an enable list, its named patches turned on. */
    private fun enableCatalogPatches(context: Context, patchesDir: File) {
        val lists = runCatching { context.assets.list("$ASSET_DIR/enable") }.getOrNull()
            ?.filter { it.length == 12 && it.endsWith(".txt") } ?: return
        if (lists.isEmpty()) return
        val catalog = runCatching { context.assets.list(CATALOG_DIR) }.getOrNull()
            ?.filter { it.endsWith(".patch.toml") } ?: return
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        for (list in lists) {
            val titleId = list.removeSuffix(".txt")
            val listText = runCatching {
                context.assets.open("$ASSET_DIR/enable/$list").use { it.readBytes().toString(Charsets.UTF_8) }
            }.getOrNull() ?: continue
            val wanted = listText.lineSequence().map { it.trim() }
                .filter { it.isNotEmpty() && !it.startsWith("#") }.toSet()
            if (wanted.isEmpty()) continue
            for (name in catalog) {
                if (!name.startsWith(titleId, ignoreCase = true)) continue
                runCatching {
                    val text = context.assets.open("$CATALOG_DIR/$name").use { it.readBytes().toString(Charsets.UTF_8) }
                    val revision = (listText + text).hashCode()
                    val key = "enable/$name"
                    if (prefs.getInt(key, 0) == revision) return@runCatching
                    val target = File(patchesDir, name)
                    if (!target.exists()) {
                        var edited = text
                        var enabled = 0
                        val parsed = xendroid.compose.patches.PatchTomlParser.parse(name, text)
                        for (entry in parsed?.entries.orEmpty()) {
                            if (entry.name in wanted && !entry.isEnabled) {
                                edited = xendroid.compose.patches.PatchTomlEditor.setEnabled(edited, entry.index, true)
                                enabled++
                            }
                        }
                        if (enabled > 0) {
                            patchesDir.mkdirs()
                            target.writeText(edited)
                            Log.i(TAG, "seeded patches/$name with $enabled patch(es) on")
                        }
                    }
                    prefs.edit().putInt(key, revision).apply()
                }.onFailure { Log.w(TAG, "enabling patches in $name failed", it) }
            }
        }
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
