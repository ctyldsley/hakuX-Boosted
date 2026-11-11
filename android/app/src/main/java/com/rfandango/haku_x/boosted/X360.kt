package com.rfandango.haku_x.boosted

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.OpenableColumns
import android.util.Log
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.LocalContext
import java.io.FileInputStream
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.util.Locale

/**
 * hakuX-Boosted: the app's one bridge to the Xbox 360 side (x360/, XenDroid;
 * its modules are boosted/android/x360-core and x360-ui).
 *
 * hakuX's own code reaches the 360 side only through this file: the
 * library's "Xbox 360" button opens the 360 library, and a game image that
 * turns out to be a 360 one goes to the 360 core instead of hakuX's
 * (LauncherActivity.startCore). The 360 side keeps its own settings, data
 * directory and process (":emu"); nothing of hakuX's core is shared with it.
 */
object X360 {
  private const val TAG = "hakuX-x360"

  /** The 360 side's activities (xendroid.compose, in :x360-ui), by name: no class of theirs is loaded here. */
  private const val LIBRARY_ACTIVITY = "xendroid.compose.MainActivity"
  private const val HOST_ACTIVITY = "xendroid.compose.EmulatorHostActivity"
  private const val ACTION_LAUNCH = "xendroid.intent.action.xendroid"
  private const val EXTRA_GAME_URI = "game_uri"

  /**
   * The 360 core's library needs Android 10 (its thread-locals are ELF TLS),
   * hakuX itself Android 9: below 10 the 360 side isn't offered.
   */
  val available: Boolean get() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q

  /** The 360 side's own library and settings. */
  fun openLibrary(context: Context) {
    if (!available) return
    context.startActivity(Intent().setClassName(context, LIBRARY_ACTIVITY))
  }

  /**
   * Starts the image in the 360 core when it is an Xbox 360 one; false:
   * it isn't (or the 360 side is unavailable), and hakuX's core takes it.
   */
  fun launchIfXbox360(context: Context, uri: Uri, launchedExternally: Boolean): Boolean {
    if (!available || !isXbox360(context, uri)) return false
    Log.i(TAG, "an Xbox 360 image, to the 360 core: $uri")
    context.startActivity(Intent(ACTION_LAUNCH).apply {
      setClassName(context, HOST_ACTIVITY)
      // a real path when the URI is a file's, the content URI otherwise
      // (the 360 side resolves it: x360/docs/frontend-integration.md)
      putExtra(EXTRA_GAME_URI, if (uri.scheme == "file") uri.path else uri.toString())
      addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
      if (launchedExternally) addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
    })
    return true
  }

  /**
   * Whether the file is an Xbox 360 title: an XEX, a ZArchive, an STFS
   * package (GOD, XBLA), or a disc image whose game partition holds
   * default.xex. A few small reads; never throws.
   */
  fun isXbox360(context: Context, uri: Uri): Boolean = try {
    val name = displayName(context, uri)?.lowercase(Locale.ROOT).orEmpty()
    when {
      name.endsWith(".xex") || name.endsWith(".zar") -> true
      name.endsWith(".xbe") || name.endsWith(".cso") || name.endsWith(".cci") -> false
      else -> context.contentResolver.openFileDescriptor(uri, "r")?.use { pfd ->
        FileInputStream(pfd.fileDescriptor).channel.use(::sniff)
      } ?: false
    }
  } catch (e: Exception) {
    Log.w(TAG, "couldn't tell what $uri is: $e")
    false
  }

  private fun displayName(context: Context, uri: Uri): String? {
    if (uri.scheme == "file") return uri.lastPathSegment
    return runCatching {
      context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
        if (c.moveToFirst()) c.getString(0) else null
      }
    }.getOrNull() ?: uri.lastPathSegment
  }

  private const val SECTOR = 2048L
  /** The GDFX volume descriptor: sector 32 of the game partition. */
  private const val VOLUME = 32 * SECTOR
  private val MAGIC = "MICROSOFT*XBOX*MEDIA".toByteArray(Charsets.US_ASCII)
  /**
   * Where a game partition starts: a rebuilt image's (0), an original Xbox
   * dump's, and Xbox 360 dumps' (XGD2, XGD3 and their variants: the core's
   * xdvdfs.c and xenia's disc_image_device.cc).
   */
  private val PARTITIONS = longArrayOf(0, 0x18300000, 0x0FD90000, 0x02080000, 0x0000FB20, 0x00020600)
  private const val MAX_ROOT = 256 * 1024

  private fun sniff(ch: FileChannel): Boolean {
    // STFS packages (Games on Demand, XBLA), and an XEX without its extension
    val head = readAt(ch, 0, 4) ?: return false
    val tag = String(head.array(), Charsets.US_ASCII)
    if (tag == "LIVE" || tag == "PIRS" || tag == "CON " || tag == "XEX2") return true

    // A disc image is either console's at any of the offsets: its root
    // directory names the executable (default.xex, default.xbe).
    val partition = PARTITIONS.firstOrNull { hasMagic(ch, it) } ?: return false
    val vd = readAt(ch, partition + VOLUME + MAGIC.size, 8) ?: return false
    vd.order(java.nio.ByteOrder.LITTLE_ENDIAN)
    val rootSector = vd.int.toLong() and 0xFFFFFFFFL
    val rootSize = vd.int.toLong() and 0xFFFFFFFFL
    if (rootSize == 0L || rootSize > MAX_ROOT) return false
    val root = readAt(ch, partition + rootSector * SECTOR, rootSize.toInt()) ?: return false
    val names = String(root.array(), Charsets.ISO_8859_1).lowercase(Locale.ROOT)
    return names.contains("default.xex") && !names.contains("default.xbe")
  }

  private fun hasMagic(ch: FileChannel, partition: Long): Boolean {
    val at = readAt(ch, partition + VOLUME, MAGIC.size) ?: return false
    return at.array().contentEquals(MAGIC)
  }

  private fun readAt(ch: FileChannel, offset: Long, size: Int): ByteBuffer? {
    if (offset < 0 || offset + size > ch.size()) return null
    val buf = ByteBuffer.allocate(size)
    var pos = offset
    while (buf.hasRemaining()) {
      val n = ch.read(buf, pos)
      if (n <= 0) return null
      pos += n
    }
    buf.flip()
    return buf
  }
}

/** The library's way into the 360 side: its own library and settings. */
@Composable
fun X360LibraryButton() {
  if (!X360.available) return
  val context = LocalContext.current
  TextButton(onClick = { X360.openLibrary(context) }) { Text("Xbox 360") }
}
