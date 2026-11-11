package xendroid.compose.core

import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.graphics.drawable.ColorDrawable
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.enableEdgeToEdge
import androidx.activity.compose.setContent
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.viewmodel.compose.viewModel
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread
import kotlinx.coroutines.runBlocking
import xendroid.compose.AppContainer
import xendroid.compose.Application
import xendroid.compose.data.Game
import xendroid.compose.data.GameFormat
import xendroid.compose.gamepad.GamepadController
import xendroid.compose.gamepad.GamepadEditorScreen
import xendroid.compose.patches.GamePatchesViewModel
import xendroid.compose.settings.ConfigStore
import xendroid.compose.settings.GameSettingsViewModel
import xendroid.compose.settings.SettingsViewModel
import xendroid.compose.settings.seedTouchOverlayDefault
import xendroid.compose.ui.about.AboutScreen
import xendroid.compose.ui.content.ContentManagerScreen
import xendroid.compose.ui.content.ContentManagerViewModel
import xendroid.compose.ui.content.InstallContentScreen
import xendroid.compose.ui.content.InstallContentViewModel
import xendroid.compose.ui.keymap.KeymapScreen
import xendroid.compose.ui.keymap.KeymapViewModel
import xendroid.compose.ui.patches.GamePatchesScreen
import xendroid.compose.ui.profile.ProfileManagerViewModel
import xendroid.compose.ui.profile.ProfilesScreen
import xendroid.compose.ui.settings.PerGameSettingsScreen
import xendroid.compose.ui.settings.SettingsScreen
import xendroid.compose.ui.theme.HostLook
import xendroid.compose.ui.theme.xendroidTheme

/**
 * hakuX-Boosted: what the app this side is embedded in (hakuX) asks of it. hakuX's
 * library is the app's only library, so this side's own library screen
 * (ui/library) is no longer opened; what it did besides listing games is here:
 *
 * - [startSession]: the once-a-process work MainActivity did as the 360 library opened;
 * - [scan]: the Xbox 360 titles under some folders, with their names, icons and title
 *   IDs (GameLibraryRepository, which also knows XEX folders, ZArchives, GOD and STFS);
 * - [launchIntent]: the intent that starts a title, with the discs of its set;
 * - [show]: one of this side's screens on its own in MainActivity, back closing it.
 *
 * The host reaches this file through one bridge of its own (hakuX's boosted/X360.kt).
 */
object HostApp {
    /** A title [scan] found. [path] is what the core boots: the file, or a folder's default.xex. */
    class Title(
        val path: String,
        val name: String,
        val format: GameFormat,
        /** The title's icon as a PNG file, or null: none in the image. */
        val icon: File?,
        /** 8 hex digits, or null when the image doesn't say. */
        val titleId: String?,
        val discNumber: Int,
        val discCount: Int,
    )

    /** One rotation of the session logs per main process: MainActivity shares the flag. */
    val sessionStarted = AtomicBoolean(false)

    @Volatile private var container: AppContainer? = null

    private fun containerOf(context: Context): AppContainer =
        container ?: synchronized(this) {
            container ?: AppContainer(context.applicationContext).also { container = it }
        }

    val allFilesAccessGranted: Boolean get() = AllFilesAccess.isGranted()

    fun requestAllFilesAccess(context: Context) = AllFilesAccess.requestAccess(context)

    /**
     * The process's start for this side, and once a process the session's: the logs of
     * the previous session shelved and a new capture started, the core's library loaded,
     * the touch controls' first default written. Returns at once; the work is on a thread.
     */
    fun startSession(context: Context) {
        val appContext = context.applicationContext
        Application.ensure_init(appContext)
        if (sessionStarted.getAndSet(true)) return
        thread(name = "SessionLogs") {
            runCatching { SessionLogs.startAppSession(appContext) }
            runCatching { EmulatorRuntime.ensureLoaded() }
            runCatching { seedTouchOverlayDefault(appContext, ConfigStore(appContext)) }
        }
    }

    /**
     * The Xbox 360 titles under [roots] (real paths, their subfolders included), by name.
     * [isXbox360Iso] decides for a disc image: the host lists the original Xbox's too, and
     * this side's scan would take every .iso for its own. Needs All Files Access. Blocking,
     * and the first time slow: each new title's image is opened for its name and icon.
     */
    fun scan(context: Context, roots: List<String>, isXbox360Iso: (File) -> Boolean): List<Title> {
        val appContext = context.applicationContext
        Application.ensure_init(appContext)
        EmulatorRuntime.ensureLoaded()
        // A profile to sign in with, as this side's library made on its first scan.
        runCatching { ProfileBootstrap.ensureDefaultProfile(appContext) }
        val c = containerOf(appContext)
        val games = runBlocking {
            // This side's own games folder setting went with its library: what still reads
            // it (the content installer copies an installed game there) gets the host's
            // first folder.
            roots.firstOrNull()?.let { c.repository.saveGameDirPath(it) }
            c.repository.scanRoots(roots, isXbox360Iso)
        }
        return games.map { g ->
            Title(
                path = g.launchUri,
                name = g.name,
                format = g.format,
                icon = g.iconCacheName?.let { c.iconCache.fileFor(it) }?.takeIf { it.exists() },
                titleId = g.titleId?.takeIf { it.isNotBlank() && it != "00000000" },
                discNumber = g.discNumber,
                discCount = g.discCount,
            )
        }
    }

    /**
     * Starts [title] in the core's own process. [all] is the library: the other discs of
     * the title's set travel with the launch, because the core's process can't list them
     * when the game asks for the next disc (as GameLibraryViewModel.buildLaunchIntent).
     */
    fun launchIntent(context: Context, title: Title, all: List<Title>): Intent =
        Intent(ACTION_LAUNCH).apply {
            setPackage(context.packageName)
            putExtra(EXTRA_GAME_URI, title.path)
            val discs = if (title.titleId == null) emptyList() else all.filter {
                it.format != GameFormat.STFS && it.titleId.equals(title.titleId, ignoreCase = true)
            }.sortedWith(compareBy({ if (it.discNumber > 0) it.discNumber else Int.MAX_VALUE },
                                    { it.name.lowercase() }))
            putExtra(EXTRA_DISC_LABELS, discs.map { discLabel(it) }.toTypedArray())
            putExtra(EXTRA_DISC_PATHS, discs.map { it.path }.toTypedArray())
        }

    private fun discLabel(t: Title): String = when {
        t.discNumber > 0 && t.discCount > 1 -> "Disc ${t.discNumber} of ${t.discCount}"
        t.discNumber > 0 -> "Disc ${t.discNumber}"
        else -> File(t.path).name
    }

    /** The intent of [launchIntent] for a path alone (no set of discs known). */
    const val ACTION_LAUNCH = "xendroid.intent.action.xendroid"
    const val EXTRA_GAME_URI = "game_uri"
    private const val EXTRA_DISC_LABELS = "disc_labels"
    private const val EXTRA_DISC_PATHS = "disc_paths"

    // ---- This side's screens, one at a time, for the host's menus ----

    /** MainActivity's extras: the screen to show (one of the SCREEN_ names), and for a
     *  title's screens its ID (8 hex digits) and name. */
    const val EXTRA_SCREEN = "x360_screen"
    const val EXTRA_TITLE_ID = "x360_title_id"
    const val EXTRA_TITLE_NAME = "x360_title_name"

    const val SCREEN_SETTINGS = "settings"
    const val SCREEN_GAME_SETTINGS = "game_settings"
    const val SCREEN_GAME_PATCHES = "game_patches"
    const val SCREEN_GAME_CONTENT = "game_content"
    const val SCREEN_INSTALL_CONTENT = "install_content"
    const val SCREEN_PROFILES = "profiles"
    const val SCREEN_KEYMAP = "keymap"
    const val SCREEN_TOUCH_CONTROLS = "touch_controls"
    const val SCREEN_ABOUT = "about"

    /** MainActivity by name, for the host: [screen] alone on screen, back closes it. */
    fun screenIntent(context: Context, screen: String, titleId: String? = null, name: String? = null): Intent =
        Intent().setClassName(context, "xendroid.compose.MainActivity")
            .putExtra(EXTRA_SCREEN, screen)
            .putExtra(EXTRA_TITLE_ID, titleId)
            .putExtra(EXTRA_TITLE_NAME, name)

    /**
     * Called by MainActivity as it starts: when its intent names a screen, that screen is
     * the activity's whole content and true is returned (the activity then leaves its own
     * library out). False: no screen asked, or one that needs a title without one.
     */
    fun show(activity: ComponentActivity, container: AppContainer): Boolean {
        val screen = activity.intent?.getStringExtra(EXTRA_SCREEN) ?: return false
        val titleId = activity.intent?.getStringExtra(EXTRA_TITLE_ID)
        val name = activity.intent?.getStringExtra(EXTRA_TITLE_NAME).orEmpty()
        val needsTitle = screen == SCREEN_GAME_SETTINGS || screen == SCREEN_GAME_PATCHES ||
            screen == SCREEN_GAME_CONTENT
        if (needsTitle && titleId.isNullOrBlank()) return false
        // The window as the host's own screens have it: the bars' icons and the colour
        // behind Compose follow the host's light or dark, not only the phone's.
        val dark = HostLook.isDark(activity)
        val bars = if (dark) SystemBarStyle.dark(Color.TRANSPARENT)
            else SystemBarStyle.light(Color.TRANSPARENT, Color.TRANSPARENT)
        activity.enableEdgeToEdge(statusBarStyle = bars, navigationBarStyle = bars)
        activity.window.setBackgroundDrawable(
            ColorDrawable(if (dark) HostLook.DARK_WINDOW else Color.WHITE))
        activity.setContent {
            xendroidTheme {
                Screen(container, screen, titleId.orEmpty(), name, onBack = { activity.finish() })
            }
        }
        return true
    }

    @Composable
    private fun Screen(
        container: AppContainer,
        screen: String,
        titleId: String,
        name: String,
        onBack: () -> Unit,
    ) {
        when (screen) {
            SCREEN_SETTINGS -> {
                val vm: SettingsViewModel = viewModel(factory = container.settingsViewModelFactory())
                SettingsScreen(vm = vm, onBack = onBack)
            }
            SCREEN_GAME_SETTINGS -> {
                val vm: GameSettingsViewModel =
                    viewModel(factory = container.gameSettingsViewModelFactory(titleId))
                PerGameSettingsScreen(vm = vm, gameName = name, onBack = onBack)
            }
            SCREEN_GAME_PATCHES -> {
                val vm: GamePatchesViewModel =
                    viewModel(factory = container.gamePatchesViewModelFactory(titleId))
                GamePatchesScreen(vm = vm, gameName = name, onBack = onBack)
            }
            SCREEN_GAME_CONTENT -> {
                val vm: ContentManagerViewModel =
                    viewModel(factory = container.gameContentManagerViewModelFactory(titleId))
                ContentManagerScreen(vm = vm, gameName = name, onBack = onBack)
            }
            SCREEN_INSTALL_CONTENT -> {
                val vm: InstallContentViewModel =
                    viewModel(factory = container.installContentViewModelFactory())
                InstallContentScreen(vm = vm, onBack = onBack)
            }
            SCREEN_PROFILES -> {
                val vm: ProfileManagerViewModel =
                    viewModel(factory = container.profileManagerViewModelFactory())
                ProfilesScreen(vm = vm, onBack = onBack)
            }
            SCREEN_KEYMAP -> {
                val vm: KeymapViewModel = viewModel(factory = container.keymapViewModelFactory())
                KeymapScreen(vm = vm, onBack = onBack)
            }
            SCREEN_TOUCH_CONTROLS -> {
                val appContext = LocalContext.current.applicationContext
                val controller = remember { GamepadController(appContext) }
                GamepadEditorScreen(controller = controller, onDone = onBack)
            }
            else -> AboutScreen(onBack = onBack)
        }
    }
}
