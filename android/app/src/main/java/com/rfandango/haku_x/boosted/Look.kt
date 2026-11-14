package com.rfandango.haku_x.boosted

import android.graphics.drawable.GradientDrawable
import android.util.TypedValue
import android.widget.TextView
import androidx.compose.runtime.Composable
import xendroid.compose.ui.theme.GameOverlayLook
import xendroid.compose.ui.theme.SharedLook

/**
 * hakuX-Boosted: the app's look, light or [dark], for hakuX's own screens (HakuXTheme in
 * ui/theme/Theme.kt hands over to this in one line, so that taking a newer hakuX stays a
 * small merge).
 *
 * The colours, shapes, type and focus states are defined once, on the 360 side
 * (x360/app/.../ui/theme/HostTheme.kt), where the 360's own screens and the in-game menu,
 * which run without hakuX's classes, wear them too.
 */
@Composable
fun BoostedTheme(dark: Boolean, content: @Composable () -> Unit) = SharedLook(dark, content)

/**
 * hakuX-Boosted: the look of a readout over a running game (the frame rate counter, the
 * stutter monitor, the "building shaders" line), the same as the Xbox 360 side's counter:
 * white on a translucent dark pill with a soft corner (GameOverlayLook, defined once on
 * the 360 side). hakuX's readouts are Android views, so the values are applied by hand;
 * [textSp] is left to the caller where a readout is larger than the counter.
 */
fun wearReadoutLook(view: TextView, textSp: Float = GameOverlayLook.READOUT_TEXT_SP) {
  val density = view.resources.displayMetrics.density
  val horizontal = (GameOverlayLook.READOUT_PADDING_HORIZONTAL_DP * density + 0.5f).toInt()
  val vertical = (GameOverlayLook.READOUT_PADDING_VERTICAL_DP * density + 0.5f).toInt()
  view.setTextColor(GameOverlayLook.READOUT_TEXT_ARGB)
  view.setTextSize(TypedValue.COMPLEX_UNIT_SP, textSp)
  view.setShadowLayer(0f, 0f, 0f, 0)
  view.setPadding(horizontal, vertical, horizontal, vertical)
  view.background = GradientDrawable().apply {
    shape = GradientDrawable.RECTANGLE
    cornerRadius = GameOverlayLook.READOUT_CORNER_DP * density
    setColor(GameOverlayLook.READOUT_BACKGROUND_ARGB)
  }
}
