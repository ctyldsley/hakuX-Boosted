package xendroid.compose.ui

import android.os.Build
import android.view.RoundedCorner
import android.view.View
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.absolutePadding
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.layout.union
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import kotlin.math.ceil

/**
 * hakuX-Boosted: padding that keeps what is drawn over a game inside the part of the
 * window the screen shows whole. The game's window is laid out edge to edge, under the
 * display cutout and the rounded corners, so a readout at the window's corner is cut off
 * (the frame rate counter at the top left of the owner's phone).
 *
 * On each side, the largest of:
 * - the display cutout and the system bars (WindowInsets; the bars count while they show,
 *   and in a game they are hidden). In landscape the cutout is on the left or the right by
 *   which way the phone is held, and the insets follow as it is turned over;
 * - what the rounded corners on that side hide. A corner of radius r hides what is nearer
 *   its tip than r(1 - 1/sqrt 2), about 0.3 r, along both edges, so a box that far in on
 *   each has its own corner inside the arc;
 * - a small margin, for a phone that does not report its corners (and before Android 12,
 *   where they can't be asked for).
 */
@Composable
fun Modifier.screenSafePadding(): Modifier {
    val view = LocalView.current
    val density = LocalDensity.current
    val insets = WindowInsets.systemBars.union(WindowInsets.displayCutout)
    // Read here, in composition: a change of the insets (the phone turned over, the bars
    // shown) recomposes this and the corners are asked for again with it.
    val left = insets.getLeft(density, LayoutDirection.Ltr)
    val top = insets.getTop(density)
    val right = insets.getRight(density, LayoutDirection.Ltr)
    val bottom = insets.getBottom(density)
    val corners = remember(view, left, top, right, bottom, view.width, view.height) { cornerRadii(view) }
    val marginX = with(density) { 12.dp.roundToPx() }
    val marginY = with(density) { 6.dp.roundToPx() }
    fun clear(a: Int, b: Int) = ceil(maxOf(a, b) * 0.3).toInt()
    return with(density) {
        absolutePadding(
            left = maxOf(left, clear(corners.topLeft, corners.bottomLeft), marginX).toDp(),
            top = maxOf(top, clear(corners.topLeft, corners.topRight), marginY).toDp(),
            right = maxOf(right, clear(corners.topRight, corners.bottomRight), marginX).toDp(),
            bottom = maxOf(bottom, clear(corners.bottomLeft, corners.bottomRight), marginY).toDp(),
        )
    }
}

/** The screen's corner radii as the window has them, in pixels; 0 where not reported. */
private class CornerRadii(val topLeft: Int, val topRight: Int, val bottomRight: Int, val bottomLeft: Int)

private fun cornerRadii(view: View): CornerRadii {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return CornerRadii(0, 0, 0, 0)
    val insets = view.rootWindowInsets ?: return CornerRadii(0, 0, 0, 0)
    fun radius(position: Int) = insets.getRoundedCorner(position)?.radius ?: 0
    return CornerRadii(
        topLeft = radius(RoundedCorner.POSITION_TOP_LEFT),
        topRight = radius(RoundedCorner.POSITION_TOP_RIGHT),
        bottomRight = radius(RoundedCorner.POSITION_BOTTOM_RIGHT),
        bottomLeft = radius(RoundedCorner.POSITION_BOTTOM_LEFT),
    )
}
