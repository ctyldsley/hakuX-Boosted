package xendroid.compose.settings

import android.content.Context
import android.util.Log
import xendroid.compose.Application
import xendroid.compose.ConfigFiles
import xendroid.compose.core.BundledDriver
import xendroid.compose.core.ContentPaths
import xendroid.compose.core.EmulatorRuntime
import xendroid.compose.core.ProfilePaths
import java.io.File
import java.util.Properties

/**
 * hakuX-Boosted: what happens when the config is missing, empty or does not parse.
 *
 * Before this, [ConfigStore] copied the bundled defaults in, silently. That signed
 * the player's profile out (their saves did not show and the title could not save),
 * dropped every setting they had chosen, and left the title on the phone's own GPU
 * driver, because the app's once-per-install writes (the bundled driver's path, the
 * touch controls' choice) were not made again.
 *
 * The order now, every time the config is opened:
 *
 * 1. the file is there and parses: use it. Refresh the backup from it, add any
 *    setting the bundled defaults have gained (the player's values are not touched),
 *    and sign a profile in if none is and the player did not sign out;
 * 2. else the backup ([ConfigFiles.backupOf]) parses: put it back;
 * 3. else the bundled defaults, with the profile found on disc signed in, the
 *    bundled driver chosen and the touch controls' choice made again.
 *
 * Cases 2 and 3 leave a [Notice] for the player. A file that did not parse is kept
 * beside the config as `<name>.unreadable`, so a hand edit with a mistake is not lost.
 *
 * The guard's own memory (the last profile signed in, whether a config has ever
 * existed, the notice) is a small file in the app's internal data, away from the
 * config, and is read from disc each time: the 360 side runs in two processes.
 */
object ConfigGuard {
    private const val TAG = "ConfigGuard"
    private val lock = Any()

    const val KIND_RESTORED = "restored"
    const val KIND_DEFAULTS = "defaults"
    const val KIND_SIGNED_IN = "signed_in"
    const val KIND_GAME_RESTORED = "game_restored"
    const val KIND_GAME_RESET = "game_reset"

    /** What the player is told. [profile] is the XUID signed in again, or empty. */
    data class Notice(
        val kind: String,
        val atMillis: Long,
        val profile: String,
        val detail: String,
        val keptAside: Boolean,
    ) {
        val title: String
            get() = when (kind) {
                KIND_RESTORED -> "Xbox 360 settings were restored"
                KIND_DEFAULTS -> "Xbox 360 settings were reset"
                KIND_SIGNED_IN -> "Profile signed in again"
                KIND_GAME_RESTORED -> "A game's settings were restored"
                KIND_GAME_RESET -> "A game's settings were reset"
                else -> "Xbox 360 settings"
            }

        val body: String
            get() {
                val aside = if (keptAside) " The unreadable file was kept beside it." else ""
                val signedIn = if (profile.isNotEmpty())
                    " Your profile was signed in again and your saves are untouched."
                else ""
                return when (kind) {
                    KIND_RESTORED ->
                        "The settings file was missing or could not be read, so the last " +
                            "saved copy was put back. Nothing else changed.$aside$signedIn"
                    KIND_DEFAULTS ->
                        "The settings file was missing or could not be read and there was no " +
                            "saved copy, so the settings are back to their defaults.$aside$signedIn" +
                            if (profile.isEmpty())
                                " No profile is signed in: choose one under Profiles to see your saves."
                            else ""
                    KIND_SIGNED_IN ->
                        "No profile was signed in although one exists on this device, so it " +
                            "was signed in. Your saves are untouched. Choose another under Profiles."
                    KIND_GAME_RESTORED ->
                        "The settings of game $detail could not be read, so the last saved " +
                            "copy was put back.$aside"
                    KIND_GAME_RESET ->
                        "The settings of game $detail could not be read and there was no saved " +
                            "copy, so that game uses the shared settings again.$aside"
                    else -> detail
                }
            }
    }

    // ---- The guard's own memory -------------------------------------------------

    private const val K_HAD_CONFIG = "had_config"
    private const val K_LAST_XUID = "last_xuid"
    private const val K_SIGNED_OUT = "signed_out"
    private const val K_MERGED = "merged_revision"
    private const val K_NOTICE_KIND = "notice_kind"
    private const val K_NOTICE_AT = "notice_at"
    private const val K_NOTICE_PROFILE = "notice_profile"
    private const val K_NOTICE_DETAIL = "notice_detail"
    private const val K_NOTICE_ASIDE = "notice_aside"
    private const val K_NOTICE_ANNOUNCED = "notice_announced"

    private fun stateFile(): File = File(Application.get_internal_data_dir(), "config_guard.properties")

    private fun loadState(): Properties {
        val p = Properties()
        runCatching { stateFile().takeIf { it.isFile }?.inputStream()?.use { p.load(it) } }
        return p
    }

    private fun saveState(p: Properties) {
        runCatching {
            val out = java.io.ByteArrayOutputStream()
            p.store(out, null)
            ConfigFiles.writeAtomic(stateFile(), out.toByteArray(), false)
        }.onFailure { Log.w(TAG, "the guard's state was not saved", it) }
    }

    private fun severity(kind: String?): Int = when (kind) {
        KIND_DEFAULTS -> 4
        KIND_RESTORED -> 3
        KIND_GAME_RESET -> 2
        KIND_GAME_RESTORED -> 2
        KIND_SIGNED_IN -> 1
        else -> 0
    }

    private fun postNotice(p: Properties, kind: String, profile: String, detail: String, aside: Boolean) {
        // A lesser event does not replace a graver one the player has not dismissed.
        if (severity(kind) < severity(p.getProperty(K_NOTICE_KIND))) return
        p.setProperty(K_NOTICE_KIND, kind)
        p.setProperty(K_NOTICE_AT, System.currentTimeMillis().toString())
        p.setProperty(K_NOTICE_PROFILE, profile)
        p.setProperty(K_NOTICE_DETAIL, detail)
        p.setProperty(K_NOTICE_ASIDE, aside.toString())
        p.remove(K_NOTICE_ANNOUNCED)
        Log.w(TAG, "notice for the player: $kind profile=$profile detail=$detail keptAside=$aside")
    }

    /** The notice waiting for the player, or null. Reads the disc: cheap, and right across processes. */
    fun pendingNotice(): Notice? = synchronized(lock) {
        val p = loadState()
        val kind = p.getProperty(K_NOTICE_KIND) ?: return@synchronized null
        Notice(
            kind = kind,
            atMillis = p.getProperty(K_NOTICE_AT)?.toLongOrNull() ?: 0L,
            profile = p.getProperty(K_NOTICE_PROFILE).orEmpty(),
            detail = p.getProperty(K_NOTICE_DETAIL).orEmpty(),
            keptAside = p.getProperty(K_NOTICE_ASIDE) == "true",
        )
    }

    /** The notice, once: for a toast as a game starts. The settings screen keeps showing it. */
    fun takeNoticeToAnnounce(): Notice? = synchronized(lock) {
        val p = loadState()
        if (p.getProperty(K_NOTICE_KIND) == null || p.getProperty(K_NOTICE_ANNOUNCED) == "true") {
            return@synchronized null
        }
        p.setProperty(K_NOTICE_ANNOUNCED, "true")
        saveState(p)
        pendingNotice()
    }

    fun dismissNotice() = synchronized(lock) {
        val p = loadState()
        if (p.getProperty(K_NOTICE_KIND) == null) return@synchronized
        listOf(K_NOTICE_KIND, K_NOTICE_AT, K_NOTICE_PROFILE, K_NOTICE_DETAIL, K_NOTICE_ASIDE,
            K_NOTICE_ANNOUNCED).forEach { p.remove(it) }
        saveState(p)
    }

    /**
     * The player's own choice of profile, from the profiles screen. An empty [xuid] is
     * a sign-out the guard must respect: it will not sign anyone in until a profile is
     * chosen again.
     */
    fun notePlayerChoseProfile(xuid: String) = synchronized(lock) {
        val p = loadState()
        if (xuid.isEmpty()) {
            p.setProperty(K_SIGNED_OUT, "true")
        } else {
            p.remove(K_SIGNED_OUT)
            p.setProperty(K_LAST_XUID, xuid.uppercase())
        }
        saveState(p)
    }

    // ---- The global config ------------------------------------------------------

    /** Length and time of the file last found good, so an unchanged file is not parsed again. */
    @Volatile private var goodSignature: String? = null

    private fun signature(file: File): String = "${file.length()}:${file.lastModified()}"

    /** The file's text parses as TOML. Needs the native library. */
    private fun parses(text: String): Boolean = try {
        ConfigHandle.openString(text).closeDiscard()
        true
    } catch (e: Exception) {
        false
    }

    /** Usable as the global config: it holds settings and it parses. */
    private fun usable(text: String?): Boolean =
        text != null && ConfigFiles.countSettings(text) > 0 && parses(text)

    /**
     * Makes the global config usable (see the class comment). Called by [ConfigStore]
     * before every open. Never throws: at worst the config is the bundled defaults.
     */
    fun ensureLive(context: Context, store: ConfigStore) {
        val file = store.globalConfigFile()
        // The common case, without the lock's work: the file is the one already checked.
        if (file.isFile && goodSignature == signature(file)) return
        synchronized(lock) {
            if (file.isFile && goodSignature == signature(file)) return
            runCatching { EmulatorRuntime.ensureLoaded() }
            val state = loadState()
            val ok = runCatching { ensureLiveLocked(context.applicationContext, store, file, state) }
                .onFailure { Log.e(TAG, "the config guard failed", it) }
                .isSuccess
            saveState(state)
            // Only a file that was checked is skipped next time.
            if (ok && file.isFile) goodSignature = signature(file)
        }
    }

    /** [ensureLive] even when the file has not changed since it was last checked. */
    fun recheck(context: Context, store: ConfigStore) {
        goodSignature = null
        ensureLive(context, store)
    }

    private fun ensureLiveLocked(context: Context, store: ConfigStore, file: File, state: Properties) {
        ConfigFiles.sweepTemporaries(file)
        val text = ConfigFiles.readTextOrNull(file)
        val backup = ConfigFiles.backupOf(file)
        var restored = false
        var defaults = false
        var keptAside = false

        if (!usable(text)) {
            keptAside = keepAside(file, text)
            val backupText = ConfigFiles.readTextOrNull(backup)
            if (usable(backupText)) {
                ConfigFiles.writeAtomic(file, backupText!!.toByteArray(Charsets.UTF_8), false)
                restored = true
                Log.w(TAG, "the config was missing or unreadable: restored from ${backup.name}")
            } else {
                ConfigFiles.writeAtomic(
                    file, store.defaultTemplateText().toByteArray(Charsets.UTF_8), false)
                defaults = true
                Log.w(TAG, "the config was missing or unreadable and so was its backup: defaults used")
            }
        } else {
            refreshBackup(file, text!!, backup)
        }

        // One open, one write (only if something changed) for everything put into the file.
        val handle = ConfigHandle.openFile(file.absolutePath)
        var changed = false
        var signedIn = ""
        try {
            if (defaults) changed = reseed(context, handle) || changed
            val revision = store.defaultTemplateText().hashCode().toString()
            if (defaults) {
                state.setProperty(K_MERGED, revision)
            } else if (restored || state.getProperty(K_MERGED) != revision) {
                val added = mergeNewSettings(store, handle)
                if (added > 0) {
                    changed = true
                    Log.i(TAG, "added $added setting(s) the bundled defaults have and the config lacked")
                }
                state.setProperty(K_MERGED, revision)
            }
            val current = handle.getString("Profiles", PROFILE_SLOT_0).orEmpty()
            if (current.isNotEmpty()) {
                state.setProperty(K_LAST_XUID, current.uppercase())
            } else if (state.getProperty(K_SIGNED_OUT) != "true") {
                val found = profileToSignIn(state.getProperty(K_LAST_XUID))
                if (found != null) {
                    handle.putString("Profiles", PROFILE_SLOT_0, found)
                    state.setProperty(K_LAST_XUID, found)
                    signedIn = found
                    changed = true
                    Log.w(TAG, "no profile was signed in: signed in $found, found on disc")
                }
            }
        } finally {
            if (changed) handle.closeFile() else handle.closeDiscard()
        }

        // A first install has no config and nothing to tell the player about.
        val hadConfig = state.getProperty(K_HAD_CONFIG) == "true"
        when {
            defaults && hadConfig -> postNotice(state, KIND_DEFAULTS, signedIn, "", keptAside)
            restored -> postNotice(state, KIND_RESTORED, signedIn, "", keptAside)
            signedIn.isNotEmpty() && hadConfig && !defaults ->
                postNotice(state, KIND_SIGNED_IN, signedIn, "", false)
        }
        state.setProperty(K_HAD_CONFIG, "true")
    }

    private const val PROFILE_SLOT_0 = "logged_profile_slot_0_xuid"

    /** Moves an unreadable (not merely missing or empty) file aside. True when one was kept. */
    private fun keepAside(file: File, text: String?): Boolean {
        if (text == null || text.isBlank()) return false
        val aside = ConfigFiles.unreadableOf(file)
        return runCatching {
            ConfigFiles.writeAtomic(aside, text.toByteArray(Charsets.UTF_8), false)
            true
        }.getOrDefault(false)
    }

    /**
     * The backup follows a good config that changed outside the app (pushed over adb,
     * written by the core), unless that file holds far fewer settings than the backup:
     * a config cut short can still parse, and must not replace the fuller copy.
     */
    private fun refreshBackup(file: File, text: String, backup: File) {
        val backupText = ConfigFiles.readTextOrNull(backup)
        if (backupText == text) return
        val have = ConfigFiles.countSettings(text)
        val had = if (usable(backupText)) ConfigFiles.countSettings(backupText) else 0
        if (have * 5 < had * 4) {
            Log.w(TAG, "the config holds $have settings, its backup $had: the backup is kept as it is")
            return
        }
        runCatching { ConfigFiles.copySynced(file, backup) }
            .onFailure { Log.w(TAG, "the backup was not refreshed", it) }
    }

    /**
     * What the app writes once per install and a config rebuilt from defaults would
     * otherwise lack: the bundled GPU driver and whether the touch controls show.
     */
    private fun reseed(context: Context, handle: ConfigHandle): Boolean {
        var changed = false
        runCatching { BundledDriver.bundledLibrary(context) }.getOrNull()?.let { library ->
            handle.putString("Vulkan", "vulkan_lib_path", library.absolutePath)
            changed = true
            Log.i(TAG, "defaults: the bundled driver chosen again")
        }
        runCatching {
            if (touchOverlayWasSeeded(context)) {
                handle.putBool("HID", "show_touch_overlay", !hasPhysicalController())
                changed = true
            }
        }
        return changed
    }

    /**
     * Adds each setting the bundled defaults list and the config lacks, with the
     * bundled value. A setting the config has is never changed. `defaults_date` is
     * left out: adding it to a config without one would let the core's own updates
     * of old defaults rewrite the player's values at the next load.
     */
    private fun mergeNewSettings(store: ConfigStore, live: ConfigHandle): Int {
        val templateText = store.defaultTemplateText()
        val template = ConfigHandle.openString(templateText)
        var added = 0
        try {
            for ((section, name) in settingNames(templateText)) {
                if (section == "Config") continue
                if (live.getString(section, name) != null) continue
                val value = template.getString(section, name) ?: continue
                live.putString(section, name, value)
                added++
            }
        } finally {
            template.closeDiscard()
        }
        return added
    }

    /** (section, name) of every `name = value` line of a config's text, in file order. */
    internal fun settingNames(text: String): List<Pair<String, String>> {
        val out = ArrayList<Pair<String, String>>()
        var section = ""
        for (raw in text.lineSequence()) {
            val line = raw.trim()
            if (line.isEmpty() || line.startsWith("#")) continue
            if (line.startsWith("[")) {
                val end = line.indexOf(']')
                if (end > 1) section = line.substring(1, end).trim()
                continue
            }
            val equals = line.indexOf('=')
            if (equals <= 0 || section.isEmpty()) continue
            val name = line.substring(0, equals).trim()
            if (name.isNotEmpty() && name.all { it.isLetterOrDigit() || it == '_' }) {
                out.add(section to name)
            }
        }
        return out
    }

    // ---- Profiles on disc -------------------------------------------------------

    /** XUIDs of the profiles under the content root, by their directory layout alone. */
    fun profilesOnDisc(): List<String> {
        val root = ContentPaths.contentRoot()
        val names = root.list() ?: return emptyList()
        return names.map { it.uppercase() }
            .filter { ProfilePaths.XUID_REGEX.matches(it) && it != ContentPaths.MACHINE_XUID }
            .filter { ProfilePaths.profileDir(it).exists() }
            .distinct()
    }

    /**
     * The profile to sign in when none is: the last one known to have been signed in
     * if it is still on disc, else the one whose content was written most recently
     * (the one the player saves with).
     */
    private fun profileToSignIn(lastKnown: String?): String? {
        val found = profilesOnDisc()
        if (found.isEmpty()) return null
        lastKnown?.uppercase()?.let { if (it in found) return it }
        return found.maxByOrNull { newestChange(File(ContentPaths.contentRoot(), it), 3) }
    }

    private fun newestChange(dir: File, depth: Int): Long {
        var newest = dir.lastModified()
        if (depth <= 0) return newest
        val children = dir.listFiles() ?: return newest
        for (child in children) {
            val t = if (child.isDirectory) newestChange(child, depth - 1) else child.lastModified()
            if (t > newest) newest = t
        }
        return newest
    }

    // ---- Per-game configs -------------------------------------------------------

    /**
     * Makes a game's own config openable. A missing one is normal (the game has no
     * settings of its own): an empty file is made, as before. One that does not parse
     * is replaced by its backup if that parses, else by an empty file, and the player
     * is told; the unreadable file is kept aside.
     */
    fun ensureGameConfig(file: File, titleId: String) = synchronized(lock) {
        runCatching { EmulatorRuntime.ensureLoaded() }
        val text = ConfigFiles.readTextOrNull(file)
        if (text == null) {
            runCatching { ConfigFiles.writeAtomic(file, ByteArray(0), false) }
            return@synchronized
        }
        if (parses(text)) return@synchronized
        val state = loadState()
        val keptAside = keepAside(file, text)
        val backupText = ConfigFiles.readTextOrNull(ConfigFiles.backupOf(file))
        val restore = usable(backupText)
        runCatching {
            ConfigFiles.writeAtomic(
                file, (if (restore) backupText!! else "").toByteArray(Charsets.UTF_8), false)
        }
        postNotice(state, if (restore) KIND_GAME_RESTORED else KIND_GAME_RESET, "", titleId, keptAside)
        saveState(state)
    }
}
