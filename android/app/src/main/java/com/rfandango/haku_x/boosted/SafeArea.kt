package com.rfandango.haku_x.boosted

import android.os.Build
import android.view.RoundedCorner
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import kotlin.math.ceil

/*
 * hakuX-Boosted: the part of a game's window that the screen shows whole, for what hakuX's
 * game activity draws at the window's corner (its frame rate counter: NativeCoreActivity
 * calls keepInSafeArea). The Xbox 360 side's counter has the same in Compose
 * (x360/app/.../ui/SafeArea.kt).
 */

/**
 * Keeps [view], which sits at the top left of a game's window, clear of what the screen
 * cuts off there: a display cutout (in landscape it is on the left or the right, by which
 * way the phone is held), the system bars while they show, and the rounded corner. The
 * game's window is laid out under all of these (its picture fills the screen), so a view
 * aligned to the window's corner is partly outside the visible screen.
 *
 * The view's left and top margins become what they were plus the inset, and follow the
 * insets as the phone is turned over. [under]: the view this one is laid out below while
 * that one shows; it then needs the left inset only.
 */
fun keepInSafeArea(view: View, under: View? = null) {
  val params = view.layoutParams as? ViewGroup.MarginLayoutParams ?: return
  val left = params.leftMargin
  val top = params.topMargin
  view.setOnApplyWindowInsetsListener { v, insets ->
    val safe = safeInsets(insets)
    val p = v.layoutParams as ViewGroup.MarginLayoutParams
    val wantLeft = left + maxOf(safe.left, marginX(v))
    val wantTop = if (under != null && under.visibility == View.VISIBLE) top else top + maxOf(safe.top, marginY(v))
    if (p.leftMargin != wantLeft || p.topMargin != wantTop) {
      p.leftMargin = wantLeft
      p.topMargin = wantTop
      v.layoutParams = p
    }
    insets
  }
  view.addOnAttachStateChangeListener(object : View.OnAttachStateChangeListener {
    override fun onViewAttachedToWindow(v: View) = v.requestApplyInsets()
    override fun onViewDetachedFromWindow(v: View) {}
  })
  if (view.isAttachedToWindow) view.requestApplyInsets()
}

/** The left and top of the window that the screen does not show whole, in pixels. */
private class SafeInsets(val left: Int, val top: Int)

private fun safeInsets(insets: WindowInsets): SafeInsets {
  var left = 0
  var top = 0
  if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
    // the cutout, and the system bars while they show (hidden in a game: then nothing)
    val safe = insets.getInsets(WindowInsets.Type.displayCutout() or WindowInsets.Type.systemBars())
    left = safe.left
    top = safe.top
  } else {
    insets.displayCutout?.let {
      left = it.safeInsetLeft
      top = it.safeInsetTop
    }
  }
  if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
    // A corner of radius r hides what is nearer its tip than r(1 - 1/sqrt 2), about 0.3 r,
    // along both edges: a view that far in on each has its own corner inside the arc.
    val radius = insets.getRoundedCorner(RoundedCorner.POSITION_TOP_LEFT)?.radius ?: 0
    val clear = ceil(radius * 0.3).toInt()
    left = maxOf(left, clear)
    top = maxOf(top, clear)
  }
  return SafeInsets(left, top)
}

/** A small margin whatever the insets say: a phone need not report its corners. */
private fun marginX(view: View) = (12 * view.resources.displayMetrics.density).toInt()
private fun marginY(view: View) = (6 * view.resources.displayMetrics.density).toInt()
