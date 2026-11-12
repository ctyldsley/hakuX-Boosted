package xendroid.compose.ui.settings

import android.app.Activity
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.input.InputMode
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalInputModeManager
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import java.io.File
import kotlin.math.roundToInt
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import xendroid.compose.Utils
import xendroid.compose.core.BundledDriver
import xendroid.compose.core.SessionLogs
import xendroid.compose.settings.Setting
import xendroid.compose.settings.SettingValue
import xendroid.compose.settings.SettingsCategory
import xendroid.compose.settings.SettingsViewModel

/*
 * hakuX-Boosted: this side's settings as a part of the host's settings screen (its
 * "Xbox 360" tab), in the host's own look: Material list rows with the value in the line
 * under the title, a switch at the end of an on/off row, a section title in the accent
 * colour. [SettingsScreen] and SettingRows.kt, which XenDroid's own screens are made of,
 * are left as upstream has them; the per-game editor still uses them.
 *
 * Nothing about how a setting is stored differs: the rows read and write through the same
 * SettingsViewModel, so the same repository, the same config file and the same flush on
 * pause as [SettingsScreen].
 */

/**
 * Flushes [vm]'s edits when the screen is paused or this leaves the composition, and opens
 * its config again on resume (as [SettingsScreen] does). An observer added to a resumed
 * lifecycle is told ON_RESUME at once, so a pane shown again after another one's flush
 * finds the config open.
 */
@Composable
fun HostSettingsLifecycle(vm: SettingsViewModel) {
    val owner = LocalLifecycleOwner.current
    DisposableEffect(owner, vm) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_PAUSE -> vm.flush()
                Lifecycle.Event.ON_RESUME -> vm.onResume()
                else -> {}
            }
        }
        owner.lifecycle.addObserver(observer)
        onDispose {
            owner.lifecycle.removeObserver(observer)
            vm.flush()
        }
    }
}

/** The sections in the order a player looks for them; the schema's order is the config file's. */
private val SECTION_ORDER = listOf(
    "Display", "Video", "GPU", "Vulkan", "CPU", "Memory", "Kernel", "APU",
    "Controller", "HID", "XConfig", "Storage", "Content", "UI", "General", "Logging",
)

/** What a section is called on screen. The schema's own names are the config file's sections. */
private fun sectionLabel(title: String) = when (title) {
    "GPU" -> "Graphics"
    "Vulkan" -> "Graphics driver (Vulkan)"
    "CPU" -> "Processor"
    "APU" -> "Audio"
    "Controller" -> "On-screen controller"
    "HID" -> "Controller input"
    "XConfig" -> "Console language and region"
    "Content" -> "Content licences"
    "UI" -> "Notifications and prompts"
    else -> title
}

/**
 * This side's settings, every one of the schema's, as one list of sections that open in
 * place. [vm] is null while the core's library loads, and [failed] says it could not be:
 * the host's own rows ([header], [footer]) show either way.
 */
@Composable
fun HostSettingsPane(
    vm: SettingsViewModel?,
    failed: Boolean,
    modifier: Modifier = Modifier,
    header: LazyListScope.() -> Unit = {},
    footer: LazyListScope.() -> Unit = {},
) {
    // Which sections are open: kept across a rotation, and across the host's tabs where the
    // host keeps a tab's saved state (hakuX's SettingsTabsFrame does). The first one starts
    // open, so the tab opens on settings and not on a list of headings.
    var open by rememberSaveable { mutableStateOf(listOf(SECTION_ORDER.first())) }
    val listState = rememberLazyListState()
    if (vm == null) {
        LazyColumn(modifier.fillMaxSize(), state = listState) {
            header()
            item(key = "x360.state") {
                HostNote(
                    if (failed) "The Xbox 360 core's settings could not be read on this phone."
                    else "Reading the Xbox 360 core's settings…"
                )
            }
            footer()
        }
        return
    }
    HostSettingsLifecycle(vm)
    val values by vm.values.collectAsStateWithLifecycle()
    val sections = remember(vm) {
        val byTitle = vm.categories.associateBy { it.title }
        // a section the order doesn't name (one upstream adds later) still shows, at the end
        SECTION_ORDER.mapNotNull { byTitle[it] } + vm.categories.filter { it.title !in SECTION_ORDER }
    }
    LazyColumn(modifier.fillMaxSize(), state = listState) {
        header()
        item(key = "x360.title") { HostSection("Emulation settings") }
        item(key = "x360.hint") {
            HostNote(
                "For every Xbox 360 game; a game's own settings (hold its card in the library) " +
                    "take their place for that game. Most apply the next time a game starts."
            )
        }
        for (section in sections) {
            val expanded = section.title in open
            item(key = "section." + section.title) {
                SectionHeader(
                    section = section,
                    expanded = expanded,
                    changed = section.settings.count { values[it.key]?.modified == true },
                    onToggle = {
                        open = if (expanded) open - section.title else open + section.title
                    },
                )
            }
            if (expanded) {
                items(section.settings, key = { "row." + it.key }) { setting ->
                    HostSettingRow(vm, setting, values[setting.key])
                }
            }
        }
        footer()
    }
}

/** A group's title, as the host's settings have it. */
@Composable
fun HostSection(title: String) {
    Text(
        title,
        style = MaterialTheme.typography.labelLarge,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(start = 16.dp, end = 16.dp, top = 24.dp, bottom = 4.dp),
    )
}

@Composable
private fun HostNote(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
    )
}

@Composable
private fun SectionHeader(section: SettingsCategory, expanded: Boolean, changed: Int, onToggle: () -> Unit) {
    ListItem(
        headlineContent = { Text(sectionLabel(section.title), style = MaterialTheme.typography.titleMedium) },
        supportingContent = {
            Text(buildString {
                append(if (section.settings.size == 1) "1 setting" else "${section.settings.size} settings")
                if (changed > 0) append(", $changed changed")
            })
        },
        trailingContent = {
            Icon(
                if (expanded) Icons.Filled.KeyboardArrowUp else Icons.Filled.KeyboardArrowDown,
                contentDescription = if (expanded) "Close" else "Open",
            )
        },
        modifier = Modifier.clickable(role = Role.Button, onClick = onToggle),
    )
}

/**
 * One setting, in the host's row. A write is dropped, not thrown, in the moment after a
 * resume when the config is not open again yet (the repository refuses it): the row shows
 * the stored value, so nothing on screen says what was not saved.
 */
@Composable
fun HostSettingRow(vm: SettingsViewModel, s: Setting, value: SettingValue?) {
    val modified = value?.modified == true
    when (s) {
        is Setting.Bool -> {
            val checked = vm.currentBool(s)
            ListItem(
                headlineContent = { Text(s.title) },
                supportingContent = supporting(null, s.desc, modified),
                trailingContent = { Switch(checked = checked, onCheckedChange = null) },
                modifier = Modifier.toggleable(value = checked, role = Role.Switch) { on ->
                    runCatching { vm.onBoolChanged(s, on) }
                },
            )
        }
        is Setting.IntRange -> {
            var dialog by remember { mutableStateOf(false) }
            val current = vm.currentInt(s)
            ListItem(
                headlineContent = { Text(s.title) },
                supportingContent = supporting(current.toString(), s.desc, modified),
                modifier = Modifier.clickable { dialog = true },
            )
            if (dialog) {
                NumberDialog(s, current, onPick = { runCatching { vm.onIntChanged(s, it) }; dialog = false }) {
                    dialog = false
                }
            }
        }
        is Setting.ListChoice -> {
            var dialog by remember { mutableStateOf(false) }
            val current = vm.currentListValue(s)
            val label = s.options.firstOrNull { it.value == current }?.label
                ?: current.ifEmpty { "Default" }
            ListItem(
                headlineContent = { Text(s.title) },
                supportingContent = supporting(label, s.desc, modified),
                modifier = Modifier.clickable { dialog = true },
            )
            if (dialog) {
                OptionsDialog(s, current, onPick = { runCatching { vm.onListChanged(s, it) }; dialog = false }) {
                    dialog = false
                }
            }
        }
        is Setting.Action ->
            if (s.name == "dump_session_logs") ExportLogsRow(s) else HostDriverRow(vm, s, value, s.title)
    }
}

/** The line under a row's title: its value, what the setting does, and whether it was changed. */
private fun supporting(value: String?, desc: String, modified: Boolean): (@Composable () -> Unit)? {
    if (value == null && desc.isEmpty() && !modified) return null
    return {
        Column {
            val line = listOfNotNull(value, desc.takeIf { it.isNotEmpty() }).joinToString(". ")
            if (line.isNotEmpty()) Text(line)
            if (modified) {
                Text(
                    "Changed from the default",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.primary,
                )
            }
        }
    }
}

/** With a controller or keyboard in use, what a dialog opens on takes the focus. */
@Composable
private fun rememberDialogFocus(): Pair<FocusRequester, Boolean> {
    val requester = remember { FocusRequester() }
    val keys = LocalInputModeManager.current.inputMode == InputMode.Keyboard
    return requester to keys
}

/** A number in a range: a slider, with a step down and up for a controller's buttons. */
@Composable
private fun NumberDialog(s: Setting.IntRange, current: Int, onPick: (Int) -> Unit, onDismiss: () -> Unit) {
    var picked by remember { mutableIntStateOf(current.coerceIn(s.min, s.max)) }
    val (focus, keys) = rememberDialogFocus()
    LaunchedEffect(Unit) { if (keys) runCatching { focus.requestFocus() } }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(s.title) },
        text = {
            Column {
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    OutlinedButton(onClick = { picked = (picked - 1).coerceAtLeast(s.min) }) { Text("−") }
                    Text(picked.toString(), style = MaterialTheme.typography.headlineSmall)
                    OutlinedButton(
                        onClick = { picked = (picked + 1).coerceAtMost(s.max) },
                        modifier = Modifier.focusRequester(focus),
                    ) { Text("+") }
                }
                if (s.max > s.min) {
                    Slider(
                        value = picked.toFloat(),
                        onValueChange = { picked = it.roundToInt().coerceIn(s.min, s.max) },
                        valueRange = s.min.toFloat()..s.max.toFloat(),
                        // a tick per value while there are few enough to tell apart
                        steps = if (s.max - s.min <= 32) (s.max - s.min - 1).coerceAtLeast(0) else 0,
                    )
                }
                Text(
                    "${s.min} to ${s.max}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        },
        confirmButton = { TextButton(onClick = { onPick(picked) }) { Text("OK") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

/** A choice of one, as radio buttons; the list scrolls (the countries are 107). */
@Composable
private fun OptionsDialog(s: Setting.ListChoice, current: String, onPick: (String) -> Unit, onDismiss: () -> Unit) {
    val selected = s.options.indexOfFirst { it.value == current }
    // opens on the current choice: it is composed, so it can take the focus
    val listState = rememberLazyListState(initialFirstVisibleItemIndex = selected.coerceAtLeast(0))
    val (focus, keys) = rememberDialogFocus()
    LaunchedEffect(Unit) { if (keys && selected >= 0) runCatching { focus.requestFocus() } }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(s.title) },
        text = {
            LazyColumn(state = listState, modifier = Modifier.fillMaxWidth().heightIn(max = 400.dp)) {
                items(s.options, key = { it.value }) { option ->
                    val isCurrent = option.value == current
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .then(if (isCurrent) Modifier.focusRequester(focus) else Modifier)
                            .selectable(selected = isCurrent, role = Role.RadioButton) { onPick(option.value) }
                            .padding(vertical = 8.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(selected = isCurrent, onClick = null)
                        Text(option.label, modifier = Modifier.padding(start = 12.dp))
                    }
                }
            }
        },
        confirmButton = {},
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun ExportLogsRow(s: Setting.Action) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var busy by remember { mutableStateOf(false) }
    ListItem(
        headlineContent = { Text(s.title) },
        supportingContent = supporting(
            if (busy) "Exporting" else "The Xbox 360 core's kept sessions and this one",
            s.desc,
            modified = false,
        ),
        modifier = Modifier.clickable(enabled = !busy) {
            busy = true
            scope.launch {
                val dest = withContext(Dispatchers.IO) { runCatching { SessionLogs.exportAll() }.getOrNull() }
                Toast.makeText(
                    context,
                    dest?.let { "Logs exported to ${it.name} in Downloads" } ?: "No logs to export",
                    Toast.LENGTH_LONG,
                ).show()
                busy = false
            }
        },
    )
}

/** What a stored driver path is called: the bundled Turnip, the phone's own, or a package's folder. */
private fun driverLabel(path: String): String = when {
    path.isEmpty() -> "The phone's own driver"
    path.startsWith(BundledDriver.bundledDir().absolutePath + File.separator) -> "Turnip, bundled with the app"
    else -> File(path).let { f -> (f.parentFile?.name?.takeIf { it.isNotEmpty() } ?: f.name) + " (installed package)" }
}

/**
 * The Vulkan driver Xbox 360 games run on: the row says which, a tap offers the choices
 * (the bundled Turnip where the app has one for this phone, a package from a .zip, the
 * phone's own). Not shown where another driver can't be loaded (no KGSL: not an Adreno).
 * The same stored path (Vulkan|vulkan_lib_path) and the same installer as XenDroid's row.
 */
@Composable
fun HostDriverRow(vm: SettingsViewModel, s: Setting.Action, value: SettingValue?, title: String) {
    if (!vm.isCustomDriverSupported) return
    val context = LocalContext.current
    val current = value?.raw ?: vm.currentDriverPath(s)
    var dialog by remember { mutableStateOf(false) }
    val pickZip = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val activity = context as? Activity ?: return@rememberLauncherForActivityResult
        if (uri != null) {
            Utils.install_custom_driver_from_zip(activity, uri) { path -> vm.onDriverPathChanged(s, path) }
        }
    }
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = supporting(driverLabel(current), s.desc, value?.modified == true),
        modifier = Modifier.clickable { dialog = true },
    )
    if (dialog) {
        val bundled = remember { BundledDriver.bundledLibrary(context.applicationContext) }
        val (focus, keys) = rememberDialogFocus()
        LaunchedEffect(Unit) { if (keys) runCatching { focus.requestFocus() } }
        AlertDialog(
            onDismissRequest = { dialog = false },
            title = { Text(title) },
            text = {
                Column {
                    Text(
                        "In use: " + driverLabel(current) + ". A change applies the next time a game starts.",
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    if (bundled != null) {
                        TextButton(
                            onClick = { vm.onDriverPathChanged(s, bundled.absolutePath); dialog = false },
                            modifier = Modifier.focusRequester(focus),
                        ) { Text("Use the bundled Turnip driver") }
                    }
                    TextButton(
                        onClick = { dialog = false; pickZip.launch(arrayOf("application/zip")) },
                        modifier = if (bundled == null) Modifier.focusRequester(focus) else Modifier,
                    ) { Text("Install a driver package (.zip)") }
                    // "" clears the path: the core then loads the phone's own driver
                    TextButton(onClick = { vm.onDriverPathChanged(s, ""); dialog = false }) {
                        Text("Use the phone's own driver")
                    }
                }
            },
            confirmButton = {},
            dismissButton = { TextButton(onClick = { dialog = false }) { Text("Cancel") } },
        )
    }
}
