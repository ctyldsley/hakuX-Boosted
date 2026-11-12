package com.rfandango.haku_x.boosted

import androidx.compose.runtime.Composable
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
