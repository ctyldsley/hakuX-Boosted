package xendroid.compose.settings

import android.content.Context
import android.preference.PreferenceManager
import android.util.Log
import android.view.InputDevice
import android.view.MotionEvent

/**
 * A gamepad or joystick is attached right now (virtual devices excluded, since the
 * emulator's own injected device advertises gamepad sources).
 *
 * hakuX-Boosted: hakuX's test of a real controller (data/Controllers.kt): stick axes
 * as well as the sources. Phones have built-in devices that claim gamepad keys
 * without being one (Xiaomi's uinput-xiaomi reports KEYBOARD | GAMEPAD): on those the
 * touch overlay started hidden with no controller attached.
 */
fun hasPhysicalController(): Boolean = InputDevice.getDeviceIds().any { id ->
    val device = InputDevice.getDevice(id) ?: return@any false
    if (device.isVirtual) return@any false
    val name = device.name?.lowercase() ?: ""
    if (listOf("accelerometer", "gyroscope", "sensor", "gpio", "uinput").any { it in name }) return@any false
    val sources = device.sources
    val gamepad = sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD
    val joystick = sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK
    (gamepad || joystick) &&
        device.getMotionRange(MotionEvent.AXIS_X) != null &&
        device.getMotionRange(MotionEvent.AXIS_Y) != null
}

/**
 * Writes HID|show_touch_overlay once per install, from whether a controller was attached:
 * with one, the overlay starts hidden, without one it starts shown. The cvar's own default
 * cannot express that, and re-deriving it every launch would fight the user's own choice
 * whenever a controller was plugged in or out. Call off the main thread, after the native
 * library is loaded.
 */
fun seedTouchOverlayDefault(context: Context, store: ConfigStore) {
    val prefs = PreferenceManager.getDefaultSharedPreferences(context)
    if (prefs.getBoolean(KEY_SEEDED, false)) return
    val show = !hasPhysicalController()
    runCatching {
        val handle = store.openLive()
        handle.putBool("HID", "show_touch_overlay", show)
        handle.closeFile()
    }.onFailure {
        // Leave the flag unset so the next launch retries rather than silently keeping
        // the cvar default.
        Log.w("TouchOverlay", "seeding show_touch_overlay failed", it)
        return
    }
    prefs.edit().putBoolean(KEY_SEEDED, true).apply()
}

private const val KEY_SEEDED = "touch_overlay_default_seeded"
