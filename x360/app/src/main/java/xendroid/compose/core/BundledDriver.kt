package xendroid.compose.core

import android.content.Context
import android.util.Log
import org.json.JSONObject
import xendroid.compose.Application
import xendroid.compose.settings.ConfigStore
import java.io.File
import java.io.IOException
import java.util.zip.ZipInputStream

/**
 * hakuX-Boosted: the Turnip driver the app ships with (assets/gpu-drivers,
 * pinned by boosted/drivers/turnip.lock.json), in use on Snapdragon phones
 * without the user installing anything.
 *
 * The 360 core is written against Turnip: on Qualcomm's own driver its
 * Turnip-only settings do nothing and games draw wrongly (the first device
 * run of Fable II, on an Adreno 840's stock driver, flickered throughout).
 * XenDroid leaves finding and installing a Turnip package to the user.
 *
 * What install() does, each time the 360 side starts:
 *
 * - unpacks the package for the phone's Adreno generation (7xx, 8xx) into
 *   the custom driver directory, once per bundled release;
 * - the first time, points the global config's vulkan_lib_path at it;
 * - after an app update with a newer package, moves a path that pointed at
 *   the bundled driver to the new one.
 *
 * A choice the user made is kept: their own driver package, and the system
 * driver once they picked it (the settings' "Default"). The package is also
 * put among hakuX's stored drivers, where its driver screen lists it; hakuX's
 * own choice of driver is not changed.
 */
object BundledDriver {
    private const val TAG = "BundledDriver"
    private const val ASSET_DIR = "gpu-drivers"
    private const val LOCK = "turnip.lock.json"
    private const val DIR = "bundled-turnip"
    private const val REVISION_FILE = ".revision"
    private const val PREFS = "bundled_driver"
    /** The bundled driver has been made the default once: an empty path after that is the user's. */
    private const val KEY_OFFERED = "offered"

    /** Blocking (a 3 MB asset, once per release): call before the core boots. Never throws. */
    @Synchronized
    fun install(context: Context) {
        runCatching { installOrThrow(context.applicationContext) }
            .onFailure { Log.w(TAG, "the bundled driver was not installed", it) }
    }

    /** The bundled driver's library when it is the one unpacked, for the settings' label. */
    fun bundledDir(): File = File(Application.get_custom_driver_dir(), DIR)

    /**
     * The bundled driver's library, unpacked, for the settings' way back to it after
     * another driver was chosen. Null: the app has no package for this phone's GPU, or
     * install() has not run yet. Never throws.
     */
    fun bundledLibrary(context: Context): File? {
        return runCatching {
            val generation = adrenoGeneration()
            val lock = JSONObject(
                context.assets.open("$ASSET_DIR/$LOCK").use { it.readBytes().toString(Charsets.UTF_8) })
            val packages = lock.getJSONArray("packages")
            val pkg = (0 until packages.length()).map { packages.getJSONObject(it) }
                .firstOrNull { it.getString("adreno") == generation }
            pkg?.let { File(bundledDir(), it.getString("library")) }?.takeIf { it.exists() }
        }.getOrNull()
    }

    private fun installOrThrow(context: Context) {
        // adrenotools loads a driver over KGSL only
        if (!File("/dev/kgsl-3d0").exists()) return
        val generation = adrenoGeneration() ?: return
        val lock = runCatching {
            JSONObject(context.assets.open("$ASSET_DIR/$LOCK").use { it.readBytes().toString(Charsets.UTF_8) })
        }.getOrNull() ?: return  // a build without the packages
        val packages = lock.getJSONArray("packages")
        val pkg = (0 until packages.length()).map { packages.getJSONObject(it) }
            .firstOrNull { it.getString("adreno") == generation } ?: return
        val file = pkg.getString("file")
        val library = pkg.getString("library")
        val revision = lock.getString("tag") + "/" + pkg.getString("sha256")

        val dir = bundledDir()
        val libFile = File(dir, library)
        val revisionFile = File(dir, REVISION_FILE)
        if (!libFile.exists() || runCatching { revisionFile.readText() }.getOrNull() != revision) {
            unpack(context, "$ASSET_DIR/$file", dir, revision)
            Log.i(TAG, "unpacked ${pkg.optString("name", file)} (${lock.getString("tag")})")
            storeForHakuX(context, file)
        }
        if (!libFile.exists()) throw IOException("$file has no $library")

        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val handle = ConfigStore(context).openLive()
        var changed = false
        try {
            val current = handle.getString("Vulkan", "vulkan_lib_path").orEmpty()
            val bundledPrefix = dir.absolutePath + File.separator
            val target = libFile.absolutePath
            val use = when {
                // nothing chosen yet (also a config from before the driver was bundled)
                current.isEmpty() -> !prefs.getBoolean(KEY_OFFERED, false)
                // the bundled driver under an older release's library name, or gone
                current.startsWith(bundledPrefix) -> current != target
                // the user's own package
                else -> false
            }
            if (use) {
                handle.putString("Vulkan", "vulkan_lib_path", target)
                changed = true
                Log.i(TAG, "vulkan_lib_path = $target")
            }
        } finally {
            if (changed) handle.closeFile() else handle.closeDiscard()
        }
        if (!prefs.getBoolean(KEY_OFFERED, false)) prefs.edit().putBoolean(KEY_OFFERED, true).apply()
    }

    /** "7", "8": the first digit of the Adreno's model number, from the Vulkan device name. */
    private fun adrenoGeneration(): String? {
        val name = Application.gpu_device_name_vk ?: return null
        return Regex("""Adreno \(TM\) (\d)\d\d""").find(name)?.groupValues?.get(1)
    }

    /**
     * The package's libraries and meta.json into [dir], which is replaced only
     * once the new one is whole. Internal storage: a library can't be loaded
     * from the shared one.
     */
    private fun unpack(context: Context, asset: String, dir: File, revision: String) {
        val fresh = File(dir.parentFile, dir.name + ".new")
        fresh.deleteRecursively()
        if (!fresh.mkdirs()) throw IOException("couldn't create $fresh")
        try {
            ZipInputStream(context.assets.open(asset)).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    val name = entry.name
                    // the package's files are at its root: no directories, no paths
                    val wanted = !entry.isDirectory && !name.contains('/') && !name.contains('\\') &&
                        (name.endsWith(".so") || name == "meta.json")
                    if (wanted) File(fresh, name).outputStream().use { zip.copyTo(it) }
                    zip.closeEntry()
                }
            }
            File(fresh, REVISION_FILE).writeText(revision)
        } catch (e: Exception) {
            fresh.deleteRecursively()
            throw e
        }
        dir.deleteRecursively()
        if (!fresh.renameTo(dir)) {
            fresh.deleteRecursively()
            throw IOException("couldn't move the driver into place")
        }
    }

    /**
     * The package among hakuX's stored drivers (GpuDriverHelper.driverStorageDir),
     * so its driver screen lists it. Only inside hakuX's app, where that
     * directory exists; an older bundled package there is replaced.
     */
    private fun storeForHakuX(context: Context, file: String) {
        runCatching {
            val storage = File(context.getExternalFilesDir(null) ?: return, "gpu_drivers")
            if (!storage.isDirectory) return
            val assets = context.assets.list(ASSET_DIR).orEmpty().filter { it.endsWith(".zip") }
            // earlier releases' packages: the names this app bundled, recorded as it stores them
            val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            prefs.getString("stored", null)?.takeIf { it != file && it !in assets }
                ?.let { File(storage, it).delete() }
            context.assets.open("$ASSET_DIR/$file").use { input ->
                File(storage, file).outputStream().use { input.copyTo(it) }
            }
            prefs.edit().putString("stored", file).apply()
        }.onFailure { Log.w(TAG, "not stored for hakuX's driver screen", it) }
    }
}
