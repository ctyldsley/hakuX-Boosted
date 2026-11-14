package xendroid.compose.settings

import android.content.Context
import xendroid.compose.Application
import java.io.File

/** Sources the live global config file and the bundled default template baseline. */
class ConfigStore(private val appContext: Context) {

    /** The live, editable config the emulator reads at boot. */
    fun globalConfigFile(): File = Application.get_global_config_file()

    /** <app_data_dir>/config/<TITLE_ID>.config.toml — exactly the path native
     *  config::LoadGameConfig reads (config.cc:309-311). The app_data_dir is the
     *  parent of the global config file (Application.get_app_data_dir() is
     *  package-private; its public sibling get_global_config_file() lives in it). */
    fun perGameConfigFile(titleId: String): File =
        File(File(globalConfigFile().parentFile, "config"), "$titleId.config.toml")

    /** Open the per-game SPARSE overlay for editing. Bootstraps an EMPTY toml table
     *  when missing (NOT the default template — that would make every key an override,
     *  destroying sparseness).
     *
     *  hakuX-Boosted: a file that does not parse is no longer wiped: its backup is put
     *  back if there is one, the unreadable file is kept aside and the player is told
     *  ([ConfigGuard.ensureGameConfig]). */
    fun openGameConfig(titleId: String): ConfigHandle {
        val file = perGameConfigFile(titleId)
        ConfigGuard.ensureGameConfig(file, titleId)
        return ConfigHandle.openFile(file.absolutePath)
    }

    /** Open the live config for editing.
     *
     *  hakuX-Boosted: a missing or unparseable file is no longer replaced by the
     *  bundled defaults here. [ConfigGuard.ensureLive] first puts the backup back, and
     *  only without one uses the defaults, keeping the player's profile signed in and
     *  leaving a notice. Closing the handle writes beside the file and renames
     *  (xendroid.compose.ConfigFiles), keeping the previous file as the backup. */
    fun openLive(): ConfigHandle {
        ConfigGuard.ensureLive(appContext, this)
        return ConfigHandle.openFile(globalConfigFile().absolutePath)
    }

    /** Open the live config READ-ONLY from a text snapshot. Unlike [openLive], the
     *  returned handle is a STRING handle — closing it via [ConfigHandle.closeString]
     *  frees the native table WITHOUT writing back to disk (close_config_file, the
     *  only disk-write path, would needlessly re-serialize on every read). Use this
     *  for pure reads; use [openLive] when you intend to persist edits. */
    fun openLiveSnapshot(): ConfigHandle {
        ConfigGuard.ensureLive(appContext, this)
        return ConfigHandle.openString(globalConfigFile().readText())
    }

    /** Read-only baseline parsed from the bundled asset template, for modified-from-default diffing. */
    fun openTemplateBaseline(): ConfigHandle =
        ConfigHandle.openString(defaultTemplateText())

    fun defaultTemplateText(): String = templateText
        ?: loadTemplateText().also { templateText = it }

    private var templateText: String? = null

    private fun loadTemplateText(): String =
        appContext.assets.open("config/default_config.toml").use { it.readBytes().toString(Charsets.UTF_8) }

}
