package xendroid.compose.ui.theme

import android.content.Context
import android.content.res.Configuration
import androidx.compose.foundation.IndicationNodeFactory
import androidx.compose.foundation.LocalIndication
import androidx.compose.foundation.border
import androidx.compose.foundation.interaction.FocusInteraction
import androidx.compose.foundation.interaction.HoverInteraction
import androidx.compose.foundation.interaction.InteractionSource
import androidx.compose.foundation.interaction.PressInteraction
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.ripple.RippleAlpha
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.LocalRippleConfiguration
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RippleConfiguration
import androidx.compose.material3.RippleDefaults
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.drawscope.ContentDrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.node.DelegatableNode
import androidx.compose.ui.node.DrawModifierNode
import androidx.compose.ui.node.invalidateDraw
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.launch

/**
 * hakuX-Boosted: the app's one look, for both consoles' screens.
 *
 * [SharedLook] is the whole of it: the colours (dark first: near-black surfaces lifted in
 * steps, one saturated green as the accent), the shapes, the type, and what a controller's
 * focus looks like. hakuX's own screens wear it through hakuX's HakuXTheme (its
 * ui/theme/Theme.kt hands over to boosted/Look.kt, which calls [SharedLook]); this side's
 * screens wear it through [HostTheme], to which [xendroidTheme] hands over when this side
 * is part of hakuX's app (core/Embedded.kt). There is no second copy of the palette.
 *
 * Nothing here is a console maker's: no logo, no trademark, no font or image of theirs.
 * The colours are this file's own and the type is the phone's.
 *
 * Light or dark is hakuX's setting (AppPrefs.theme: dark unless chosen otherwise, the
 * phone's, or light). hakuX passes it to [SharedLook]; this side's screens, which can run
 * in a process of their own, read it from hakuX's preferences file by name ([HostLook]).
 */
object HostLook {
    /** hakuX's AppPrefs: its file, its theme key and the key's values. */
    private const val PREFS = "x1box_prefs"
    private const val KEY_THEME = "app_theme"
    private const val THEME_SYSTEM = "system"
    private const val THEME_DARK = "dark"

    fun isDark(context: Context, systemDark: Boolean): Boolean {
        // dark while nothing was chosen, as hakuX's AppPrefs.theme
        val mode = runCatching {
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY_THEME, THEME_DARK)
        }.getOrNull() ?: THEME_DARK
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

/** The accent: one green, bright enough to read on the dark surfaces. */
private val Green = Color(0xFF54D66A)
/** The same accent where the surface is light: deep enough for white text on it. */
private val GreenOnLight = Color(0xFF1F8A3B)
/** What marks a value as changed from its default (the 360 side's rows): amber, apart from the accent. */
private val Amber = Color(0xFFF2C14E)

private val LightColors = lightColorScheme(
    primary = GreenOnLight,
    onPrimary = Color.White,
    primaryContainer = Color(0xFFCDEFD3),
    onPrimaryContainer = Color(0xFF0B4A1C),
    inversePrimary = Green,
    secondary = GreenOnLight,
    onSecondary = Color.White,
    secondaryContainer = Color(0xFFE4F5E7),
    onSecondaryContainer = Color(0xFF0B4A1C),
    tertiary = Color(0xFF8A6100),
    onTertiary = Color.White,
    tertiaryContainer = Color(0xFFFFE08A),
    onTertiaryContainer = Color(0xFF2B1D00),
    background = Color.White,
    onBackground = Color(0xFF1A1C1B),
    surface = Color.White,
    onSurface = Color(0xFF1A1C1B),
    surfaceVariant = Color(0xFFF1F4F2),
    onSurfaceVariant = Color(0xFF5A625E),
    surfaceDim = Color(0xFFE2E6E3),
    surfaceBright = Color.White,
    surfaceContainerLowest = Color.White,
    surfaceContainerLow = Color(0xFFF8FAF8),
    surfaceContainer = Color(0xFFF2F5F3),
    surfaceContainerHigh = Color(0xFFECEFED),
    surfaceContainerHighest = Color(0xFFE5E9E6),
    inverseSurface = Color(0xFF2E3330),
    inverseOnSurface = Color(0xFFF0F3F1),
    outline = Color(0xFFC9CFCB),
    outlineVariant = Color(0xFFDDE2DF),
    error = Color(0xFFC62828),
)

/**
 * Dark: the background is the window's (HostLook.DARK_WINDOW), and what lies on it is
 * lifted in steps with a trace of the accent's hue, so a card, a dialog and a menu each
 * read as a layer without an outline.
 */
private val DarkColors = darkColorScheme(
    primary = Green,
    onPrimary = Color(0xFF00390F),
    primaryContainer = Color(0xFF155D27),
    onPrimaryContainer = Color(0xFFC6F7CE),
    inversePrimary = GreenOnLight,
    secondary = Green,
    onSecondary = Color(0xFF00390F),
    secondaryContainer = Color(0xFF1F3A27),
    onSecondaryContainer = Color(0xFFC6F7CE),
    tertiary = Amber,
    onTertiary = Color(0xFF3D2E00),
    tertiaryContainer = Color(0xFF5A4300),
    onTertiaryContainer = Color(0xFFFFE08A),
    background = Color(0xFF121212),
    onBackground = Color(0xFFECF0ED),
    surface = Color(0xFF121212),
    onSurface = Color(0xFFECF0ED),
    surfaceVariant = Color(0xFF2A2E2C),
    onSurfaceVariant = Color(0xFFB3BAB6),
    surfaceDim = Color(0xFF121212),
    surfaceBright = Color(0xFF363B38),
    surfaceContainerLowest = Color(0xFF0C0C0C),
    surfaceContainerLow = Color(0xFF181A19),
    surfaceContainer = Color(0xFF1D201E),
    surfaceContainerHigh = Color(0xFF252927),
    surfaceContainerHighest = Color(0xFF2E3330),
    inverseSurface = Color(0xFFECF0ED),
    inverseOnSurface = Color(0xFF1D201E),
    outline = Color(0xFF737A76),
    outlineVariant = Color(0xFF383D3A),
    error = Color(0xFFFFA39B),
)

/**
 * The spacing every screen of both sides is laid out on, so that a row, a section title
 * and a card start on the same line whichever side's screen they are on. Material's own
 * list rows already use the gutter.
 */
object LookSpacing {
    /** From the screen's edge to text and cards. */
    val Gutter = 16.dp
    /** Above a section's title. */
    val SectionTop = 24.dp
    /** Between a section's title and its first row. */
    val SectionBottom = 4.dp
    /** Between stacked buttons or cards. */
    val Gap = 8.dp
    /** Inside a card or an in-game panel; the smaller one where the window is short. */
    val Panel = 20.dp
    val PanelCompact = 12.dp
}

/**
 * What is drawn over a running game, the same for both consoles: the dimming behind a
 * menu or a prompt, and the small readout (the frame rate counter). Plain values, because
 * hakuX's counter is an Android view and this side's is Compose.
 *
 * The dimming is near-black with a trace of the accent's hue, as the dark surfaces are.
 * The readout is white on a translucent dark pill with a soft corner: readable over a
 * bright scene and a dark one, and small enough to leave on.
 */
object GameOverlayLook {
    val Scrim = Color(0xFF040705).copy(alpha = 0.66f)

    const val READOUT_TEXT_ARGB = 0xE6FFFFFF.toInt()
    const val READOUT_BACKGROUND_ARGB = 0x73000000
    const val READOUT_TEXT_SP = 11f
    const val READOUT_CORNER_DP = 6f
    const val READOUT_PADDING_HORIZONTAL_DP = 6f
    const val READOUT_PADDING_VERTICAL_DP = 2f

    val ReadoutText = Color(READOUT_TEXT_ARGB)
    val ReadoutBackground = Color(READOUT_BACKGROUND_ARGB)
    val ReadoutShape = RoundedCornerShape(READOUT_CORNER_DP.dp)
}

/** Rounder than Material's: a card, a tile and a menu have the same generous corner. */
private val LookShapes = Shapes(
    extraSmall = RoundedCornerShape(6.dp),
    small = RoundedCornerShape(10.dp),
    medium = RoundedCornerShape(14.dp),
    large = RoundedCornerShape(20.dp),
    extraLarge = RoundedCornerShape(28.dp),
)

/** The phone's typeface, with the titles and labels a weight heavier so a screen's structure reads at arm's length. */
private val LookTypography = Typography().let { base ->
    Typography(
        headlineMedium = base.headlineMedium.copy(fontWeight = FontWeight.SemiBold),
        headlineSmall = base.headlineSmall.copy(fontWeight = FontWeight.SemiBold),
        titleLarge = base.titleLarge.copy(fontWeight = FontWeight.SemiBold),
        titleMedium = base.titleMedium.copy(fontWeight = FontWeight.SemiBold),
        titleSmall = base.titleSmall.copy(fontWeight = FontWeight.SemiBold),
        labelLarge = base.labelLarge.copy(fontWeight = FontWeight.SemiBold, letterSpacing = 0.4.sp),
    )
}

/**
 * Material's own components (buttons, switches, tabs, menu items) keep their ripple; what
 * has a controller's or keyboard's focus stands out at 24 %, Material's 10 % is hard to
 * find on a handheld's screen (hakuX's choice, kept).
 */
@OptIn(ExperimentalMaterial3Api::class)
private val FocusRipple = RippleDefaults.RippleAlpha.let {
    RippleConfiguration(
        rippleAlpha = RippleAlpha(
            draggedAlpha = it.draggedAlpha,
            focusedAlpha = 0.24f,
            hoveredAlpha = it.hoveredAlpha,
            pressedAlpha = it.pressedAlpha,
        ),
    )
}

/**
 * What a row, a card or a tile shows while it is pressed, pointed at or focused: every
 * Modifier.clickable, toggleable and selectable that names no indication of its own takes
 * this one (LocalIndication), on both sides' screens. Focus is a ring in the accent colour
 * around the element and a wash over it, so the place a controller is at never depends on
 * a faint change of shade; a press is a wash alone.
 *
 * It stands in for the ripple on those elements (one indication draws at a time). The
 * node keeps counts, not flags: a second press can begin before the first one's release
 * is told.
 */
private class FocusRingIndication(private val ring: Color, private val wash: Color) : IndicationNodeFactory {
    override fun create(interactionSource: InteractionSource): DelegatableNode =
        FocusRingNode(interactionSource, ring, wash)

    override fun equals(other: Any?): Boolean =
        other is FocusRingIndication && other.ring == ring && other.wash == wash

    override fun hashCode(): Int = 31 * ring.hashCode() + wash.hashCode()
}

private class FocusRingNode(
    private val interactionSource: InteractionSource,
    private val ring: Color,
    private val wash: Color,
) : Modifier.Node(), DrawModifierNode {
    private var presses = 0
    private var focuses = 0
    private var hovers = 0

    override fun onAttach() {
        coroutineScope.launch {
            interactionSource.interactions.collect { interaction ->
                when (interaction) {
                    is PressInteraction.Press -> presses++
                    is PressInteraction.Release, is PressInteraction.Cancel -> presses = (presses - 1).coerceAtLeast(0)
                    is FocusInteraction.Focus -> focuses++
                    is FocusInteraction.Unfocus -> focuses = (focuses - 1).coerceAtLeast(0)
                    is HoverInteraction.Enter -> hovers++
                    is HoverInteraction.Exit -> hovers = (hovers - 1).coerceAtLeast(0)
                }
                invalidateDraw()
            }
        }
    }

    override fun onDetach() {
        presses = 0
        focuses = 0
        hovers = 0
    }

    override fun ContentDrawScope.draw() {
        drawContent()
        val alpha = when {
            presses > 0 -> 0.16f
            focuses > 0 -> 0.12f
            hovers > 0 -> 0.06f
            else -> 0f
        }
        if (alpha > 0f) drawRect(wash, alpha = alpha)
        val stroke = 2.dp.toPx()
        if (focuses > 0 && size.width > stroke * 4 && size.height > stroke * 4) {
            val radius = 12.dp.toPx().coerceAtMost(size.minDimension / 2)
            // inside the element's bounds: what clips it (a card's shape) must not cut the ring
            drawRoundRect(
                color = ring,
                topLeft = Offset(stroke / 2, stroke / 2),
                size = Size(size.width - stroke, size.height - stroke),
                cornerRadius = CornerRadius(radius, radius),
                style = Stroke(width = stroke),
            )
        }
    }
}

/**
 * The same ring for an element that draws its own states (a Material component, which
 * keeps its ripple): put before the modifier that makes it focusable.
 */
fun Modifier.focusRing(shape: Shape = RoundedCornerShape(12.dp)): Modifier = composed {
    var focused by remember { mutableStateOf(false) }
    val color = MaterialTheme.colorScheme.primary
    onFocusChanged { focused = it.isFocused }
        .then(if (focused) Modifier.border(2.dp, color, shape) else Modifier)
}

/** The app's look around [content], light or [dark]: see the top of this file. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SharedLook(dark: Boolean, content: @Composable () -> Unit) {
    val colors = if (dark) DarkColors else LightColors
    val indication = remember(colors) { FocusRingIndication(ring = colors.primary, wash = colors.onSurface) }
    MaterialTheme(colorScheme = colors, shapes = LookShapes, typography = LookTypography) {
        CompositionLocalProvider(
            LocalRippleConfiguration provides FocusRipple,
            LocalIndication provides indication,
            content = content,
        )
    }
}

/** [SharedLook] for this side's screens: light or dark as hakuX's setting says. */
@Composable
fun HostTheme(content: @Composable () -> Unit) {
    val context = LocalContext.current
    val systemDark = isSystemInDarkTheme()
    val dark = remember(systemDark) { HostLook.isDark(context, systemDark) }
    SharedLook(dark, content)
}
