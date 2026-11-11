package xendroid.compose.ui.theme

import android.content.Context
import android.content.res.Configuration
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material.ripple.RippleAlpha
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.LocalRippleConfiguration
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RippleConfiguration
import androidx.compose.material3.RippleDefaults
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.remember
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

/**
 * hakuX-Boosted: hakuX's look, for this side's screens when they are part of hakuX's app
 * (core/Embedded.kt), so that the app is one app to the eye: [xendroidTheme] hands over to
 * [HostTheme] then.
 *
 * This module can't use hakuX's theme itself (hakuX's app depends on this module, not the
 * other way round, and the game's screens run in a process of their own), so the palette
 * and the theme setting are read here:
 *
 * - the colours are a copy of hakuX's ui/theme/Theme.kt (HakuXColors, HakuXDarkColors,
 *   and its stronger focus highlight for controllers). When hakuX changes them, change
 *   them here;
 * - light or dark is hakuX's setting (AppPrefs.theme: the phone's, light or dark), read
 *   from hakuX's preferences file by name as a screen starts.
 */
object HostLook {
    /** hakuX's AppPrefs: its file, its theme key and the key's values. */
    private const val PREFS = "x1box_prefs"
    private const val KEY_THEME = "app_theme"
    private const val THEME_SYSTEM = "system"
    private const val THEME_DARK = "dark"

    fun isDark(context: Context, systemDark: Boolean): Boolean {
        val mode = runCatching {
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY_THEME, THEME_SYSTEM)
        }.getOrNull() ?: THEME_SYSTEM
        return mode == THEME_DARK || (mode == THEME_SYSTEM && systemDark)
    }

    /** For an activity setting up its window, outside composition. */
    fun isDark(context: Context): Boolean = isDark(
        context,
        (context.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
            Configuration.UI_MODE_NIGHT_YES,
    )

    /** The window behind Compose in the dark theme (hakuX's LauncherActivity.DARK_WINDOW). */
    const val DARK_WINDOW = 0xFF121212.toInt()
}

private val HostColors = lightColorScheme(
    primary = Color(0xFF2E7D32),
    onPrimary = Color.White,
    primaryContainer = Color(0xFFC8E6C9),
    onPrimaryContainer = Color(0xFF1B5E20),
    secondary = Color(0xFF2E7D32),
    onSecondary = Color.White,
    secondaryContainer = Color(0xFFE8F5E9),
    onSecondaryContainer = Color(0xFF1B5E20),
    background = Color.White,
    onBackground = Color(0xFF1C1B1F),
    surface = Color.White,
    onSurface = Color(0xFF1C1B1F),
    surfaceVariant = Color(0xFFF5F5F5),
    onSurfaceVariant = Color(0xFF5F6368),
    surfaceContainerLowest = Color.White,
    surfaceContainerLow = Color(0xFFFAFAFA),
    surfaceContainer = Color(0xFFF5F5F5),
    surfaceContainerHigh = Color(0xFFEEEEEE),
    surfaceContainerHighest = Color(0xFFE8E8E8),
    outline = Color(0xFFDADCE0),
    outlineVariant = Color(0xFFE0E0E0),
    error = Color(0xFFD32F2F),
)

private val HostDarkColors = darkColorScheme(
    primary = Color(0xFF81C784),
    onPrimary = Color(0xFF003A0B),
    primaryContainer = Color(0xFF1B5E20),
    onPrimaryContainer = Color(0xFFC8E6C9),
    secondary = Color(0xFF81C784),
    onSecondary = Color(0xFF003A0B),
    secondaryContainer = Color(0xFF23392A),
    onSecondaryContainer = Color(0xFFC8E6C9),
    background = Color(0xFF121212),
    onBackground = Color(0xFFE6E6E6),
    surface = Color(0xFF121212),
    onSurface = Color(0xFFE6E6E6),
    surfaceVariant = Color(0xFF2B2B2B),
    onSurfaceVariant = Color(0xFFB4B8BC),
    surfaceContainerLowest = Color(0xFF0E0E0E),
    surfaceContainerLow = Color(0xFF1A1A1A),
    surfaceContainer = Color(0xFF1F1F1F),
    surfaceContainerHigh = Color(0xFF262626),
    surfaceContainerHighest = Color(0xFF2F2F2F),
    outline = Color(0xFF6E7378),
    outlineVariant = Color(0xFF3A3A3A),
    error = Color(0xFFEF9A9A),
)

/** hakuX's: what has a controller's focus stands out at 24 %, Material's 10 % is hard to find. */
@OptIn(ExperimentalMaterial3Api::class)
private val HostFocusRipple = RippleDefaults.RippleAlpha.let {
    RippleConfiguration(
        rippleAlpha = RippleAlpha(
            draggedAlpha = it.draggedAlpha,
            focusedAlpha = 0.24f,
            hoveredAlpha = it.hoveredAlpha,
            pressedAlpha = it.pressedAlpha,
        ),
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HostTheme(content: @Composable () -> Unit) {
    val context = LocalContext.current
    val systemDark = isSystemInDarkTheme()
    val dark = remember(systemDark) { HostLook.isDark(context, systemDark) }
    MaterialTheme(colorScheme = if (dark) HostDarkColors else HostColors) {
        CompositionLocalProvider(LocalRippleConfiguration provides HostFocusRipple, content = content)
    }
}
