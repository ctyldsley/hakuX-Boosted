package xendroid.compose.ui.pause

import android.content.Context
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlin.math.roundToInt
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import xendroid.compose.core.EmulatorSession
import xendroid.compose.gamepad.GamepadConfigDto
import xendroid.compose.gamepad.GamepadController
import xendroid.compose.gamepad.GamepadGlobalsDto
import xendroid.compose.settings.ConfigHandle
import xendroid.compose.settings.ConfigStore
import xendroid.compose.settings.ConfigValueShape
import xendroid.compose.settings.Setting
import xendroid.compose.settings.SettingsSchema

/** The sections of the in-game settings panel, in the order they are shown. */
enum class InGameGroup(val title: String) {
    GRAPHICS("Graphics"),
    EMULATION("Emulation"),
    CONTROLS("Controls"),
}

/**
 * One row of the in-game settings panel. Activating it (a tap, or A on a controller) moves
 * to its next value: the panel is driven by a controller whose D-pad only steps a
 * selection, so every row is a single "next value" action. [value] reads snapshot state.
 */
class InGameRow(
    val group: InGameGroup,
    val title: String,
    /** True: the running game takes the change at once. False: on the next start. */
    val live: Boolean,
    val value: () -> String,
    val onActivate: () -> Unit,
)

/**
 * hakuX-Boosted: the settings the in-game menu offers, and where each change goes.
 *
 * Two kinds of core setting:
 *
 * - live: the core reads the cvar again on every frame, draw or vblank, so setting it
 *   changes the running game ([LIVE]; the same list is in emulator_xendroid.cpp, which
 *   refuses any other name);
 * - on the next start: read once while the core starts (resolution scale, the Turnip mode,
 *   the guest scheduler, ...). The row says so.
 *
 * Both kinds are written to the running title's own config
 * (<storage root>/config/<TITLE ID>.config.toml, the file the per-game settings screen
 * edits), so what is chosen in a game stays with that game. Until the title is open there
 * is no title ID and the panel offers nothing ([ready] false).
 *
 * The touch controls' own settings (opacity, auto-hide, haptics) are not cvars: they are
 * the gamepad layout's globals, which the overlay follows as they change.
 */
class InGameSettingsController(
    appContext: Context,
    private val session: EmulatorSession,
    private val gamepad: GamepadController,
    private val scope: CoroutineScope,
    /** The host's own toggle: it also updates the host's copy of the state. */
    private val touchOverlayShown: () -> Boolean,
    private val toggleTouchOverlay: () -> Unit,
) {
    private val store = ConfigStore(appContext.applicationContext)
    private val writes = Mutex()

    /** Setting key ("Section|name") -> raw value as the config file spells it. */
    private val values = mutableStateMapOf<String, String>()
    private var padConfig by mutableStateOf(GamepadConfigDto())

    /** The running title's ID; null until the title is open. */
    var titleId by mutableStateOf<String?>(null)
        private set

    /** False until [load] has read the title's values: the panel shows a note instead. */
    var ready by mutableStateOf(false)
        private set

    /** The core settings among [rows]; filled as the rows are built, so declared before them. */
    private val settings = ArrayList<Setting>()

    init {
        scope.launch { gamepad.config.collect { padConfig = it } }
    }

    val rows: List<InGameRow> = buildList {
        cvar(InGameGroup.GRAPHICS, "Display|show_debug_overlay", "Show frame rate")
        cvar(InGameGroup.GRAPHICS, "GPU|framerate_limit")
        cvar(InGameGroup.GRAPHICS, "GPU|guest_display_refresh_cap")
        cvar(InGameGroup.GRAPHICS, "Display|present_letterbox", "Keep aspect ratio (off: stretch)")
        cvar(InGameGroup.GRAPHICS, "Vulkan|vulkan_async_skip_draws", "Skip draws while shaders compile")
        cvar(InGameGroup.GRAPHICS, "GPU|draw_resolution_scale_x", "Resolution scale, width")
        cvar(InGameGroup.GRAPHICS, "GPU|draw_resolution_scale_y", "Resolution scale, height")
        cvar(InGameGroup.GRAPHICS, "Display|postprocess_antialiasing")
        cvar(InGameGroup.GRAPHICS, "Display|postprocess_scaling_and_sharpening")
        cvar(InGameGroup.GRAPHICS, "Vulkan|turnip_debug", "Turnip mode")
        cvar(InGameGroup.GRAPHICS, "GPU|render_target_path")
        cvar(InGameGroup.EMULATION, "GPU|occlusion_query")
        cvar(InGameGroup.EMULATION, "GPU|readback_resolve")
        cvar(InGameGroup.EMULATION, "Kernel|guest_scheduler")
        cvar(InGameGroup.EMULATION, "CPU|collapse_memory_delay_spins")
        cvar(InGameGroup.EMULATION, "General|apply_patches", "Apply game patches")
        cvar(InGameGroup.EMULATION, "Kernel|apply_title_update")
        add(InGameRow(
            InGameGroup.CONTROLS, "On-screen controls", live = true,
            value = { if (touchOverlayShown()) "Shown" else "Hidden" },
            onActivate = toggleTouchOverlay,
        ))
        pad("Controls opacity", { "${(it.opacity * 100).roundToInt()}%" }) {
            // 20% to 100% in tenths, then round again
            val tenths = (it.opacity * 10).roundToInt() + 1
            it.copy(opacity = (if (tenths > 10) 2 else tenths) / 10f)
        }
        pad("Hide controls when idle", {
            if (it.autoHideSeconds <= 0f) "Never" else "After ${it.autoHideSeconds.roundToInt()} s"
        }) {
            val next = AUTO_HIDE_STEPS.firstOrNull { step -> step > it.autoHideSeconds + 0.5f } ?: 0f
            it.copy(autoHideSeconds = next)
        }
        pad("Vibrate on touch", { if (it.hapticsEnabled) "On" else "Off" }) {
            it.copy(hapticsEnabled = !it.hapticsEnabled)
        }
    }

    /**
     * Reads the title's ID and each row's value: the running core's for a live setting,
     * otherwise the title's config, then the global config, then the default. Called each
     * time the panel opens (the title may not have been open the last time).
     */
    fun load() {
        scope.launch {
            val id = session.currentTitleId()
            titleId = id
            if (id == null) {
                ready = false
                return@launch
            }
            val read = withContext(Dispatchers.IO) {
                writes.withLock { runCatching { readValues(id) } }
            }
            read.onSuccess { values.putAll(it); ready = true }
                .onFailure { Log.w(TAG, "reading the settings of $id failed", it); ready = false }
        }
    }

    private fun readValues(id: String): Map<String, String> {
        val out = HashMap<String, String>()
        // A title without a config of its own has no file: nothing is created for a read.
        val gameFile = store.perGameConfigFile(id)
        val game = if (gameFile.isFile && gameFile.length() > 0L) {
            runCatching { ConfigHandle.openFile(gameFile.absolutePath) }.getOrNull()
        } else {
            null
        }
        val global = runCatching { store.openLiveSnapshot() }.getOrNull()
        try {
            for (s in settings) {
                val live = if (s.name in LIVE) session.liveSetting(s.name) else null
                out[s.key] = live
                    ?: game?.getString(s.section, s.name)
                    ?: global?.getString(s.section, s.name)
                    ?: defaultOf(s)
            }
        } finally {
            game?.closeDiscard()
            global?.closeDiscard()
        }
        return out
    }

    private fun MutableList<InGameRow>.cvar(group: InGameGroup, key: String, title: String? = null) {
        val s = SettingsSchema.byKey[key] ?: return
        settings.add(s)
        add(InGameRow(
            group, title ?: s.title, live = s.name in LIVE,
            value = { labelOf(s, values[s.key] ?: defaultOf(s)) },
            onActivate = { advance(s) },
        ))
    }

    private fun MutableList<InGameRow>.pad(
        title: String,
        label: (GamepadGlobalsDto) -> String,
        next: (GamepadGlobalsDto) -> GamepadGlobalsDto,
    ) {
        add(InGameRow(
            InGameGroup.CONTROLS, title, live = true,
            value = { label(padConfig.globals) },
            onActivate = {
                val updated = padConfig.copy(globals = next(padConfig.globals))
                padConfig = updated
                scope.launch { runCatching { gamepad.save(updated) } }
            },
        ))
    }

    private fun advance(s: Setting) {
        val id = titleId ?: return
        if (!ready) return
        val current = values[s.key] ?: defaultOf(s)
        val next = nextValue(s, current) ?: return
        if (s.name in LIVE && !session.setLiveSetting(s.name, next)) {
            Log.w(TAG, "the core refused ${s.name} = $next")
            return
        }
        values[s.key] = next
        scope.launch(Dispatchers.IO) {
            writes.withLock {
                runCatching {
                    val handle = store.openGameConfig(id)
                    try {
                        when (s) {
                            is Setting.Bool ->
                                handle.putBool(s.section, s.name, ConfigValueShape.parseBool(next, s.default))
                            is Setting.IntRange ->
                                handle.putInt(s.section, s.name, ConfigValueShape.parseInt(next, s.default))
                            is Setting.ListChoice -> putList(handle, s, next)
                            is Setting.Action -> Unit
                        }
                    } finally {
                        handle.closeFile()
                    }
                }.onFailure { Log.w(TAG, "writing ${s.key} to the config of $id failed", it) }
            }
        }
    }

    /** A list of numbers is an integer in the config (the frame rate limit, the scales). */
    private fun putList(handle: ConfigHandle, s: Setting.ListChoice, value: String) {
        val number = value.toIntOrNull()
        if (number != null && s.options.all { it.value.toIntOrNull() != null }) {
            handle.putInt(s.section, s.name, number)
        } else {
            handle.putString(s.section, s.name, value)
        }
    }

    companion object {
        private const val TAG = "InGameSettings"

        /** The settings the running core takes at once: emulator_xendroid.cpp has the same list. */
        val LIVE: Set<String> = setOf(
            "show_debug_overlay", "present_letterbox", "framerate_limit",
            "guest_display_refresh_cap", "vulkan_async_skip_draws",
        )

        private val AUTO_HIDE_STEPS = floatArrayOf(4f, 8f, 12f, 20f)

        fun defaultOf(s: Setting): String = when (s) {
            is Setting.Bool -> ConfigValueShape.bool(s.default)
            is Setting.IntRange -> s.default.toString()
            is Setting.ListChoice -> s.default
            is Setting.Action -> s.default
        }

        /** The value after [current]: a switch flips, a list moves on and wraps. */
        fun nextValue(s: Setting, current: String): String? = when (s) {
            is Setting.Bool -> ConfigValueShape.bool(!ConfigValueShape.parseBool(current, s.default))
            is Setting.IntRange -> {
                val v = ConfigValueShape.parseInt(current, s.default)
                (if (v >= s.max) s.min else v + 1).toString()
            }
            is Setting.ListChoice -> {
                if (s.options.isEmpty()) null
                else s.options[(s.options.indexOfFirst { it.value == current } + 1) % s.options.size].value
            }
            is Setting.Action -> null
        }

        fun labelOf(s: Setting, raw: String): String = when (s) {
            is Setting.Bool -> if (ConfigValueShape.parseBool(raw, s.default)) "On" else "Off"
            is Setting.ListChoice ->
                s.options.firstOrNull { it.value == raw }?.label ?: raw.ifEmpty { "Default" }
            else -> raw
        }
    }
}
