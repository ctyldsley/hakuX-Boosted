package xendroid.compose.core

import android.content.Context

/**
 * hakuX-Boosted: whether the 360 side runs inside another app (hakuX) rather
 * than as XenDroid itself, whose package is xendroid.compose (.debug). What
 * belongs to the XenDroid application alone (its updater, which installs
 * XenDroid's releases) is off when embedded.
 */
object Embedded {
    fun isEmbedded(context: Context): Boolean =
        !context.packageName.startsWith("xendroid.compose")
}
