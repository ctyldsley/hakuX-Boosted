package xendroid.compose.core

import android.content.Context
import android.util.Log
import xendroid.compose.Utils
import xendroid.compose.data.GameMetadataCache
import xendroid.compose.settings.ConfigStore
import java.io.File

/**
 * hakuX-Boosted: what the core keeps on disc of each title's shaders, for the settings'
 * "Shader caches" rows: how much it is, and a way to clear one title's.
 *
 * The core's layout (gpu/shader_storage.h, vulkan_pipeline_cache.cc), under the cache
 * root's `shaders` directory:
 *
 * - `shareable/<TITLEID>.xsh`: the title's translated shaders' list;
 * - `shareable/<TITLEID>.<fsi|fbo>.vk.xpso`: its pipeline states;
 * - `local/<TITLEID>.vk.bin`: the driver's compiled pipelines, by far the largest.
 *
 * Every file under `shaders` whose name starts with the title's eight hex digits is
 * counted as the title's, so a file a later core adds is covered too. Clearing costs
 * nothing but time: the title compiles its shaders again as it plays, with the stutter
 * of a first run.
 */
object ShaderCaches {
    private const val TAG = "ShaderCaches"
    private val TITLE_ID = Regex("^[0-9A-Fa-f]{8}")

    class Entry(val titleId: String, val name: String?, val bytes: Long, val files: Int)

    /** `<cache root>/shaders`. The cache root is `<storage root>/cache` unless Storage|cache_root says otherwise. */
    fun shadersDir(context: Context): File {
        val storage = File(Utils.get_storage_root_path())
        val configured = runCatching {
            val handle = ConfigStore(context.applicationContext).openLiveSnapshot()
            try {
                handle.getString("Storage", "cache_root").orEmpty()
            } finally {
                handle.closeDiscard()
            }
        }.getOrDefault("")
        val root = when {
            configured.isEmpty() -> File(storage, "cache")
            File(configured).isAbsolute -> File(configured)
            else -> File(storage, configured)
        }
        return File(root, "shaders")
    }

    private fun filesOf(dir: File, into: MutableList<File>, depth: Int = 4) {
        val children = dir.listFiles() ?: return
        for (child in children) {
            if (child.isDirectory) {
                if (depth > 0) filesOf(child, into, depth - 1)
            } else {
                into.add(child)
            }
        }
    }

    /** Each title with shaders on disc, largest first. Blocking. */
    fun list(context: Context): List<Entry> {
        val all = ArrayList<File>()
        filesOf(shadersDir(context), all)
        val names = runCatching {
            GameMetadataCache(context.applicationContext.cacheDir).also { it.load() }.namesByTitleId()
        }.getOrDefault(emptyMap())
        return all.mapNotNull { f -> TITLE_ID.find(f.name)?.value?.uppercase()?.let { it to f } }
            .groupBy({ it.first }, { it.second })
            .map { (id, files) -> Entry(id, names[id], files.sumOf { it.length() }, files.size) }
            .sortedByDescending { it.bytes }
    }

    /** Deletes one title's shader files. Returns the bytes freed. Blocking. */
    fun clear(context: Context, titleId: String): Long {
        val id = titleId.uppercase()
        if (!Regex("^[0-9A-F]{8}$").matches(id)) return 0L
        val all = ArrayList<File>()
        filesOf(shadersDir(context), all)
        var freed = 0L
        for (f in all) {
            if (!f.name.uppercase().startsWith(id)) continue
            val size = f.length()
            if (f.delete()) freed += size else Log.w(TAG, "could not delete ${f.name}")
        }
        Log.i(TAG, "cleared the shader cache of $id: $freed bytes")
        return freed
    }
}
