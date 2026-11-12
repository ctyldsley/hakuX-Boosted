package com.rfandango.haku_x.boosted

import android.content.Context
import android.content.Intent
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.DocumentsContract
import android.provider.DocumentsContract.Document
import android.provider.OpenableColumns
import android.util.Log
import android.widget.Toast
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Build as BuildIcon
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Star
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.LifecycleResumeEffect
import com.rfandango.haku_x.data.Game
import com.rfandango.haku_x.ui.common.Section
import com.rfandango.haku_x.ui.common.rememberFocusOnShow
import java.io.File
import java.io.FileInputStream
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.util.ArrayDeque
import java.util.Locale
import xendroid.compose.core.HostApp

/**
 * An Xbox 360 title in hakuX's library. path: what the 360 core boots (the
 * file, or a folder's default.xex), null while it isn't known (no All files
 * access yet). titleId: 8 hex digits, null when the image wasn't read.
 */
class X360Title(val path: String?, val titleId: String?)

/**
 * What X360.scanLibrary found, for LibraryViewModel to merge into its list:
 * the titles hakuX's own scan doesn't list (extra: XEX folders, ZArchives,
 * GOD and STFS packages), and for every 360 title its name and icon, by the
 * game's URI.
 */
class X360Library(
  val titles: Map<Uri, X360Title>,
  val names: Map<Uri, String>,
  val icons: Map<Uri, ImageBitmap>,
  val extra: List<Game>,
  val needsAccess: Boolean,
) {
  companion object {
    val NONE = X360Library(emptyMap(), emptyMap(), emptyMap(), emptyList(), false)
  }
}

/**
 * hakuX-Boosted: the app's one bridge to the Xbox 360 side (x360/, XenDroid;
 * its modules are boosted/android/x360-core and x360-ui).
 *
 * hakuX's own code reaches the 360 side only through this file, and this
 * file reaches it through xendroid.compose.core.HostApp:
 *
 * - the library lists the 360 titles in hakuX's games folders beside the
 *   Xbox ones (scanLibrary, from LibraryViewModel.refresh), marks them
 *   (X360Badge) and gives them the 360 side's menu (X360GameMenu);
 * - a game image that is a 360 one goes to the 360 core instead of hakuX's
 *   (launchIfXbox360, from LauncherActivity.startCore);
 * - the settings have an Xbox 360 tab with the 360 core's settings in hakuX's
 *   rows (X360SettingsTab), and the Global tab has the 360's share of what both
 *   consoles need: file access, controls, GPU driver (from SettingsScreen).
 *
 * The 360 side keeps its own settings, data directory and process (":emu");
 * nothing of hakuX's core is shared with it. It reads games by their real
 * paths, which needs "All files access": hakuX's folders are SAF trees, so
 * the library asks for it once there is a 360 title to show (X360AccessBanner).
 */
object X360 {
  private const val TAG = "hakuX-x360"

  /** The 360 core's activity (xendroid.compose, in :x360-ui). */
  private const val HOST_ACTIVITY = "xendroid.compose.EmulatorHostActivity"

  /**
   * The 360 core's library needs Android 10 (its thread-locals are ELF TLS),
   * hakuX itself Android 9: below 10 the 360 side isn't offered.
   */
  val available: Boolean get() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q

  /**
   * Listing and starting from the library also needs Android 11: the 360 side
   * reads real paths, and "All files access" is Android 11's.
   */
  private val libraryAvailable: Boolean get() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R

  /** The library's 360 titles by the game's URI: Compose state, the cards follow it. */
  private val titles = mutableStateMapOf<Uri, X360Title>()

  /** There are 360 titles and no "All files access" yet: the library says so (X360AccessBanner). */
  var needsAccess by mutableStateOf(false)
    private set

  /** The 360 side's own record of the titles of the last scan: a launch needs a title's set of discs. */
  @Volatile private var hostTitles: List<HostApp.Title> = emptyList()

  /** Whether the last scan had "All files access"; null: none yet. */
  @Volatile private var scannedWithAccess: Boolean? = null

  fun title(uri: Uri): X360Title? = titles[uri]

  /** "All files access" was given or taken since the last scan: the library lists again. */
  fun accessChanged(): Boolean {
    val before = scannedWithAccess ?: return false
    return libraryAvailable && before != HostApp.allFilesAccessGranted
  }

  fun requestAccess(context: Context) = HostApp.requestAllFilesAccess(context)

  /** One of the 360 side's own screens (HostApp.SCREEN_ names), back returning here. */
  fun openScreen(context: Context, screen: String, titleId: String? = null, name: String? = null) {
    if (!available) return
    context.startActivity(HostApp.screenIntent(context, screen, titleId, name))
  }

  /**
   * The Xbox 360 titles in the games folders. games: what hakuX's scan listed
   * there; isXbox: a game whose disc hakuX read as an original Xbox one (not
   * looked at again). Blocking, and slow the first time a title is seen (its
   * image is opened for its name and icon; the 360 side caches both).
   *
   * Without "All files access" only the images hakuX listed can be told
   * apart (through SAF): they are marked, under their file names.
   */
  fun scanLibrary(context: Context, folders: List<Uri>, games: List<Game>, isXbox: (Game) -> Boolean): X360Library {
    if (!libraryAvailable) return X360Library.NONE
    val granted = HostApp.allFilesAccessGranted
    scannedWithAccess = granted
    val sniffed = games.filter { !isXbox(it) && isXbox360(context, it.uri) }
    if (!granted) {
      hostTitles = emptyList()
      return X360Library(
        titles = sniffed.associate { it.uri to X360Title(null, null) },
        names = emptyMap(),
        icons = emptyMap(),
        extra = emptyList(),
        needsAccess = sniffed.isNotEmpty() || folders.any { hasOtherFormats(context, it) },
      )
    }
    val roots = folders.mapNotNull { folder -> realPath(context, folder)?.let { it to folder } }
    // Nothing of the 360's in the folders: its library isn't loaded into this process at all.
    if (sniffed.isEmpty() && roots.none { hasTitles(File(it.first), 0) }) {
      hostTitles = emptyList()
      return X360Library.NONE
    }
    val found = try {
      HostApp.startSession(context)
      HostApp.scan(context, roots.map { it.first }, ::isXbox360File)
    } catch (e: Throwable) {
      // the 360 core's library missing or failing to load: the images stay marked, unnamed
      Log.w(TAG, "the 360 side's scan failed: $e")
      emptyList()
    }
    hostTitles = found

    val listed = HashMap<String, Game>()
    for (g in games) realPath(context, g.uri)?.let { listed[it] = g }
    val outTitles = HashMap<Uri, X360Title>()
    val names = HashMap<Uri, String>()
    val icons = HashMap<Uri, ImageBitmap>()
    val extra = ArrayList<Game>()
    for (t in found) {
      val game = listed[t.path]
      val uri = game?.uri ?: Uri.fromFile(File(t.path))
      if (game == null) {
        val root = roots.firstOrNull { t.path.startsWith(it.first + "/") }
        // an XEX folder is known by its folder, not by the default.xex in it
        val shown = if (t.path.endsWith("/default.xex", ignoreCase = true)) t.path.substringBeforeLast('/') else t.path
        extra += Game(
          title = t.name,
          uri = uri,
          relativePath = if (root != null) shown.removePrefix(root.first + "/") else shown.substringAfterLast('/'),
          sizeBytes = File(t.path).length(),
          folder = root?.second ?: folders.first(),
        )
      }
      outTitles[uri] = X360Title(t.path, t.titleId)
      names[uri] = t.name
      val bitmap = t.icon?.let { BitmapFactory.decodeFile(it.path) }
      if (bitmap != null) {
        bitmap.prepareToDraw()
        icons[uri] = bitmap.asImageBitmap()
      }
    }
    // told apart through SAF but not found by the 360 side's scan: marked, started by its URI
    for (g in sniffed) {
      if (g.uri !in outTitles) outTitles[g.uri] = X360Title(realPath(context, g.uri), null)
    }
    return X360Library(outTitles, names, icons, extra, needsAccess = false)
  }

  /** The scan's result on screen (the main thread: Compose state). */
  fun show(library: X360Library) {
    titles.clear()
    titles.putAll(library.titles)
    needsAccess = library.needsAccess
  }

  /**
   * Starts the image in the 360 core when it is an Xbox 360 one; false:
   * it isn't (or the 360 side is unavailable), and hakuX's core takes it.
   */
  fun launchIfXbox360(context: Context, uri: Uri, launchedExternally: Boolean): Boolean {
    if (!available) return false
    val known = titles[uri]
    if (known == null && !isXbox360(context, uri)) return false
    // From the library: the 360 core's activity closes at once without the access, so ask here.
    if (!launchedExternally && !HostApp.allFilesAccessGranted) {
      Toast.makeText(context, "Xbox 360 games need \"All files access\"", Toast.LENGTH_LONG).show()
      HostApp.requestAllFilesAccess(context)
      return true
    }
    Log.i(TAG, "an Xbox 360 image, to the 360 core: $uri")
    HostApp.startSession(context)
    val host = known?.path?.let { path -> hostTitles.firstOrNull { it.path == path } }
    val intent = if (host != null) {
      // with the other discs of its set, for the game's disc change
      HostApp.launchIntent(context, host, hostTitles)
    } else {
      // a real path when it is known or the URI is a file's, the content URI otherwise
      // (the 360 side resolves it: x360/docs/frontend-integration.md)
      Intent(HostApp.ACTION_LAUNCH)
        .putExtra(HostApp.EXTRA_GAME_URI, known?.path ?: if (uri.scheme == "file") uri.path else uri.toString())
    }
    intent.setClassName(context, HOST_ACTIVITY)
    intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    if (launchedExternally) intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
    context.startActivity(intent)
    return true
  }

  /** A disc image at a real path is an Xbox 360 one (the 360 side's scan asks, for each .iso). */
  private fun isXbox360File(file: File): Boolean = try {
    FileInputStream(file).channel.use(::sniff)
  } catch (e: Exception) {
    false
  }

  /**
   * A SAF document's or tree's real path: the external storage provider's IDs
   * are "<volume>:<path>" (primary, or a card's UUID). Null for any other
   * provider's.
   */
  private fun realPath(context: Context, uri: Uri): String? {
    if (uri.scheme == "file") return uri.path
    if (uri.authority != "com.android.externalstorage.documents") return null
    val id = try {
      if (DocumentsContract.isDocumentUri(context, uri)) {
        DocumentsContract.getDocumentId(uri)
      } else {
        DocumentsContract.getTreeDocumentId(uri)
      }
    } catch (_: IllegalArgumentException) {
      return null
    }
    val parts = id.split(":", limit = 2)
    if (parts.size != 2) return null
    val base = if (parts[0].equals("primary", ignoreCase = true)) {
      Environment.getExternalStorageDirectory().absolutePath
    } else {
      "/storage/${parts[0]}"
    }
    return if (parts[1].isEmpty()) base else "$base/${parts[1]}"
  }

  private const val MAX_DEPTH = 12
  private val STFS_MAGICS = setOf("LIVE", "PIRS", "CON ")

  /**
   * Whether a folder (a real path) holds a 360 title in a form hakuX's scan
   * doesn't list: a ZArchive, a folder with default.xex, a GOD or STFS
   * package (a file without an extension that starts with a package's
   * magic). Cheap: names, and four bytes of the files without an extension.
   */
  private fun hasTitles(dir: File, depth: Int): Boolean {
    val children = dir.listFiles() ?: return false
    for (child in children) {
      val name = child.name
      if (child.isDirectory) continue
      if (name.endsWith(".zar", ignoreCase = true) || name.equals("default.xex", ignoreCase = true)) return true
      if (!name.contains('.') && child.length() >= 4 && magicOf(child) in STFS_MAGICS) return true
    }
    if (depth >= MAX_DEPTH) return false
    return children.any {
      // a GOD package's own data folder holds only its numbered parts
      it.isDirectory && !it.name.startsWith(".") && !it.name.endsWith(".data", ignoreCase = true) &&
        hasTitles(it, depth + 1)
    }
  }

  private fun magicOf(file: File): String? = try {
    FileInputStream(file).use { input ->
      val head = ByteArray(4)
      if (input.read(head) == 4) String(head, Charsets.US_ASCII) else null
    }
  } catch (e: Exception) {
    null
  }

  /**
   * The same question as hasTitles for a games folder that can only be listed
   * through SAF (no "All files access" yet), by names alone: a ZArchive, a
   * default.xex, a GOD package's ".data" folder. Decides only whether the
   * library asks for the access.
   */
  private fun hasOtherFormats(context: Context, folder: Uri): Boolean {
    val rootId = try {
      DocumentsContract.getTreeDocumentId(folder)
    } catch (_: IllegalArgumentException) {
      return false
    }
    val stack = ArrayDeque<String>()
    stack.add(rootId)
    var listed = 0
    while (stack.isNotEmpty() && listed < 500) {
      val id = stack.removeLast()
      listed++
      try {
        val children = DocumentsContract.buildChildDocumentsUriUsingTree(folder, id)
        val columns = arrayOf(Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE)
        context.contentResolver.query(children, columns, null, null, null)?.use { c ->
          while (c.moveToNext()) {
            val childId = c.getString(0) ?: continue
            val name = c.getString(1) ?: continue
            if (c.getString(2) == Document.MIME_TYPE_DIR) {
              if (name.endsWith(".data", ignoreCase = true)) return true
              if (!name.startsWith(".")) stack.add(childId)
            } else if (name.endsWith(".zar", ignoreCase = true) || name.equals("default.xex", ignoreCase = true)) {
              return true
            }
          }
        }
      } catch (_: Exception) {
        // a folder that can't be listed now is left out
      }
    }
    return false
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

/**
 * A 360 title's line in its library card, where an Xbox disc has its ID and
 * region flags: the console, and the title's ID once its image was read.
 */
@Composable
fun X360Badge(uri: Uri, style: TextStyle) {
  val title = X360.title(uri) ?: return
  val green = MaterialTheme.colorScheme.primary
  Row(Modifier.padding(top = 3.dp), verticalAlignment = Alignment.CenterVertically) {
    Text(
      "XBOX 360",
      style = style.copy(color = green, fontWeight = FontWeight.Bold),
      modifier = Modifier
        .border(1.dp, green, RoundedCornerShape(4.dp))
        .padding(horizontal = 4.dp, vertical = 1.dp),
    )
    if (title.titleId != null) {
      Text(title.titleId, style = style, modifier = Modifier.padding(start = 6.dp))
    }
  }
}

/**
 * A 360 title's long-press menu: the 360 side's settings for the game, its
 * patches, and its add-ons and updates. They are kept by the title's ID,
 * so without one (the image not read: no "All files access", or a file the
 * 360 side couldn't open) the menu says what is missing.
 */
@Composable
fun X360GameMenu(uri: Uri, name: String, onDismiss: () -> Unit) {
  val title = X360.title(uri) ?: return
  val context = LocalContext.current
  val first = Modifier.focusRequester(rememberFocusOnShow())
  DropdownMenu(expanded = true, onDismissRequest = onDismiss) {
    val id = title.titleId
    if (id == null) {
      val granted = HostApp.allFilesAccessGranted
      DropdownMenuItem(
        text = { Text(if (granted) "This game's ID couldn't be read" else "Allow \"All files access\"") },
        modifier = first,
        onClick = {
          onDismiss()
          if (!granted) X360.requestAccess(context)
        },
      )
    } else {
      DropdownMenuItem(
        text = { Text("Game settings") },
        leadingIcon = { Icon(Icons.Filled.Settings, contentDescription = null) },
        modifier = first,
        onClick = {
          onDismiss()
          X360.openScreen(context, HostApp.SCREEN_GAME_SETTINGS, id, name)
        },
      )
      DropdownMenuItem(
        text = { Text("Patches") },
        leadingIcon = { Icon(Icons.Filled.BuildIcon, contentDescription = null) },
        onClick = {
          onDismiss()
          X360.openScreen(context, HostApp.SCREEN_GAME_PATCHES, id, name)
        },
      )
      DropdownMenuItem(
        text = { Text("Add-ons and updates") },
        leadingIcon = { Icon(Icons.Filled.Star, contentDescription = null) },
        onClick = {
          onDismiss()
          X360.openScreen(context, HostApp.SCREEN_GAME_CONTENT, id, name)
        },
      )
    }
  }
}

/** The library's note that its Xbox 360 titles wait for "All files access", and the way to give it. */
@Composable
fun X360AccessBanner() {
  if (!X360.needsAccess) return
  val context = LocalContext.current
  Row(
    Modifier.fillMaxWidth().padding(start = 16.dp, end = 8.dp, top = 8.dp),
    verticalAlignment = Alignment.CenterVertically,
  ) {
    Text(
      "Xbox 360 games need \"All files access\" to show with their names and to start.",
      style = MaterialTheme.typography.bodySmall,
      color = MaterialTheme.colorScheme.onSurfaceVariant,
      modifier = Modifier.weight(1f),
    )
    TextButton(onClick = { X360.requestAccess(context) }) { Text("Allow") }
  }
}

/**
 * In the settings' Global tab a section holds each console's rows under the console's
 * name. Where the 360 core isn't offered (Android 9) the section is hakuX's alone and
 * needs no such label.
 */
@Composable
fun ConsoleRows(console: String) {
  if (X360.available) SettingsSubSection(console)
}

/**
 * Global, Games: "All files access", which the folders' Xbox 360 games need to be listed
 * with their names and to start (the 360 core reads files by path).
 */
@Composable
fun X360FilesAccess() {
  if (!X360.available || Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return
  val context = LocalContext.current
  // read again when back from the system's page for it
  var granted by remember { mutableStateOf(HostApp.allFilesAccessGranted) }
  LifecycleResumeEffect(Unit) {
    granted = HostApp.allFilesAccessGranted
    onPauseOrDispose {}
  }
  ListItem(
    headlineContent = { Text("All files access") },
    supportingContent = {
      Text(
        if (granted) "Allowed. Xbox 360 games are read by their file paths."
        else "Not allowed. Xbox 360 games need it to be listed and to start: tap to allow."
      )
    },
    modifier = Modifier.clickable { X360.requestAccess(context) },
  )
}

/**
 * Global, Controls: the Xbox 360's. The two cores keep their own touch layout and
 * controller mapping (they share no code), so each console has its rows.
 */
@Composable
fun X360Controls() {
  if (!X360.available) return
  SettingsSubSection("Xbox 360 games")
  X360ScreenRow(
    "On-screen controls", "Move and resize each control of the on-screen controller.", HostApp.SCREEN_TOUCH_CONTROLS,
  )
  X360ScreenRow(
    "Controller buttons", "Which button of a controller is which on the Xbox 360's.", HostApp.SCREEN_KEYMAP,
  )
}

/**
 * Global, GPU drivers: the driver Xbox 360 games run on (the 360 core's own setting, in
 * the same row style; the bundled Turnip unless another was chosen).
 */
@Composable
fun X360GpuDriver() {
  if (!X360.available) return
  HostApp.GpuDriverSetting("GPU driver") { SettingsSubSection("Xbox 360 games") }
}

/**
 * The settings' Xbox 360 tab: every setting of the 360 core's, in hakuX's rows, read
 * from and written to the 360 core's own configuration file as its own settings screen
 * does (HostApp.Settings). Above them, what belongs to the console rather than to the
 * emulation: its profiles and its content installer, each a screen of the 360 side's
 * (as hakuX's hard disk and Insignia rows open theirs); below, its About.
 */
@Composable
fun X360SettingsTab(modifier: Modifier = Modifier) {
  HostApp.Settings(
    modifier = modifier,
    header = {
      item { Section("Console") }
      item {
        X360ScreenRow("Profiles", "The gamer profiles Xbox 360 games sign in with.", HostApp.SCREEN_PROFILES)
      }
      item {
        X360ScreenRow(
          "Install content", "Add-ons, title updates and packaged games from a file.", HostApp.SCREEN_INSTALL_CONTENT,
        )
      }
    },
    footer = {
      item { Section("About") }
      item {
        X360ScreenRow("About the Xbox 360 core", "XenDroid, a Xenia fork, and its licences.", HostApp.SCREEN_ABOUT)
      }
    },
  )
}

/** A row that opens one of the 360 side's screens (HostApp.SCREEN_ names), back returning here. */
@Composable
private fun X360ScreenRow(title: String, summary: String, screen: String) {
  val context = LocalContext.current
  ListItem(
    headlineContent = { Text(title) },
    supportingContent = { Text(summary) },
    modifier = Modifier.clickable { X360.openScreen(context, screen) },
  )
}
