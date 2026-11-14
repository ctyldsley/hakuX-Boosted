package xendroid.compose.ui.pause

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import xendroid.compose.ui.panel.GuestPanelOption
import xendroid.compose.ui.panel.GuestPanelOptionsRow

/**
 * Option order, mirrored by the host's PanelNav so the controller and the touch UI agree.
 * hakuX-Boosted: "Edit controls" and "Settings" added; the order is the order on screen,
 * row by row.
 */
const val PAUSE_OPTION_TOUCH_OVERLAY = 0
const val PAUSE_OPTION_EDIT_CONTROLS = 1
const val PAUSE_OPTION_SETTINGS = 2
const val PAUSE_OPTION_RESUME = 3
const val PAUSE_OPTION_QUIT = 4
const val PAUSE_OPTION_COUNT = 5

/**
 * The in-game menu, styled as the guest prompt panels (keyboard, disc swap, message box) so
 * every in-game overlay looks and navigates alike. An in-window Surface rather than a Dialog:
 * a Dialog takes window focus and trips the host's focus-loss pause. [selected] is driven by
 * the host because the D-pad arrives as hat axes that never reach a composable.
 */
@Composable
fun PauseMenuPanel(
    selected: Int,
    touchOverlayShown: Boolean,
    onToggleTouchOverlay: () -> Unit,
    onEditControls: () -> Unit,
    onSettings: () -> Unit,
    onResume: () -> Unit,
    onQuit: () -> Unit,
    modifier: Modifier = Modifier,
) {
    BoxWithConstraints(
        modifier
            .fillMaxSize()
            .background(xendroid.compose.ui.theme.GameOverlayLook.Scrim)  // hakuX-Boosted: one dimming for every in-game panel
            // Swallow taps meant for the game surface underneath.
            .pointerInput(Unit) { awaitPointerEventScope { while (true) awaitPointerEvent() } },
        contentAlignment = Alignment.Center,
    ) {
        val compact = maxHeight < 400.dp
        val outerPadding = if (compact) 8.dp else 24.dp
        val innerPadding = if (compact) xendroid.compose.ui.theme.LookSpacing.PanelCompact else xendroid.compose.ui.theme.LookSpacing.Panel

        Surface(
            modifier = Modifier
                .widthIn(max = 520.dp)
                .fillMaxWidth()
                .padding(outerPadding),
            shape = MaterialTheme.shapes.large,
            tonalElevation = 6.dp,
        ) {
            Column(Modifier.padding(innerPadding)) {
                Text("Paused", style = MaterialTheme.typography.titleMedium)

                GuestPanelOptionsRow(Modifier.padding(top = if (compact) 8.dp else 16.dp)) {
                    GuestPanelOption(
                        label = if (touchOverlayShown) "Hide controls" else "Show controls",
                        selected = selected == PAUSE_OPTION_TOUCH_OVERLAY,
                        onClick = onToggleTouchOverlay,
                        modifier = Modifier.weight(1f),
                    )
                    GuestPanelOption(
                        label = "Edit controls",
                        selected = selected == PAUSE_OPTION_EDIT_CONTROLS,
                        onClick = onEditControls,
                        modifier = Modifier.weight(1f),
                    )
                }
                GuestPanelOption(
                    label = "Settings",
                    selected = selected == PAUSE_OPTION_SETTINGS,
                    onClick = onSettings,
                    modifier = Modifier.padding(top = 8.dp),
                )
                GuestPanelOptionsRow(Modifier.padding(top = 8.dp)) {
                    GuestPanelOption(
                        label = "Resume",
                        selected = selected == PAUSE_OPTION_RESUME,
                        onClick = onResume,
                        modifier = Modifier.weight(1f),
                    )
                    GuestPanelOption(
                        label = "Quit to library",
                        selected = selected == PAUSE_OPTION_QUIT,
                        onClick = onQuit,
                        modifier = Modifier.weight(1f),
                    )
                }
            }
        }
    }
}

/** One entry of the settings list: a section's heading, or the row at [index] of the rows. */
private class SettingsListItem(val heading: String?, val index: Int)

/**
 * hakuX-Boosted: the in-game settings, a second page of the in-game menu. Same rules as
 * [PauseMenuPanel]: in the game's window, [selected] driven by the host.
 *
 * The options are [rows] in order and then "Back" (index rows.size): the host's PanelNav
 * counts rows.size + 1. A row shows its value and whether a change applies now or on the
 * next start; activating it moves to the next value. When [ready] is false (the title is
 * not open yet, so there is no config to write to) only a note and "Back" are shown, and
 * the host's PanelNav counts 1.
 */
@Composable
fun InGameSettingsPanel(
    rows: List<InGameRow>,
    ready: Boolean,
    selected: Int,
    onActivate: (Int) -> Unit,
    onBack: () -> Unit,
    modifier: Modifier = Modifier,
) {
    BoxWithConstraints(
        modifier
            .fillMaxSize()
            .background(xendroid.compose.ui.theme.GameOverlayLook.Scrim)  // hakuX-Boosted: one dimming for every in-game panel
            .pointerInput(Unit) { awaitPointerEventScope { while (true) awaitPointerEvent() } },
        contentAlignment = Alignment.Center,
    ) {
        val compact = maxHeight < 400.dp
        val outerPadding = if (compact) 8.dp else 24.dp
        val innerPadding = if (compact) xendroid.compose.ui.theme.LookSpacing.PanelCompact else xendroid.compose.ui.theme.LookSpacing.Panel

        // Headings are list entries too, so a row's place in the list is not its index.
        val entries = remember(rows) {
            buildList<SettingsListItem> {
                var group: InGameGroup? = null
                rows.forEachIndexed { i, row ->
                    if (row.group != group) {
                        group = row.group
                        add(SettingsListItem(row.group.title, -1))
                    }
                    add(SettingsListItem(null, i))
                }
            }
        }
        val listState = rememberLazyListState()
        // Keep the controller's selection on screen, with the entry above it (its heading,
        // for a section's first row) still visible.
        LaunchedEffect(selected, ready) {
            if (!ready || selected >= rows.size) return@LaunchedEffect
            val at = entries.indexOfFirst { it.index == selected }
            if (at >= 0) listState.animateScrollToItem((at - 1).coerceAtLeast(0))
        }

        Surface(
            modifier = Modifier
                .widthIn(max = 640.dp)
                .fillMaxWidth()
                .fillMaxHeight()
                .padding(outerPadding),
            shape = MaterialTheme.shapes.large,
            tonalElevation = 6.dp,
        ) {
            Column(Modifier.padding(innerPadding)) {
                Text("Settings", style = MaterialTheme.typography.titleMedium)
                Text(
                    "For this game. Tap a setting, or press A, for its next value.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )

                if (ready) {
                    LazyColumn(
                        state = listState,
                        modifier = Modifier.weight(1f).fillMaxWidth().padding(top = 8.dp),
                    ) {
                        items(entries.size) { at ->
                            val item = entries[at]
                            if (item.heading != null) {
                                Text(
                                    item.heading,
                                    style = MaterialTheme.typography.labelLarge,
                                    color = MaterialTheme.colorScheme.primary,
                                    modifier = Modifier.padding(top = 12.dp, bottom = 4.dp),
                                )
                            } else {
                                InGameSettingRow(
                                    row = rows[item.index],
                                    selected = selected == item.index,
                                    onClick = { onActivate(item.index) },
                                )
                            }
                        }
                    }
                } else {
                    Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
                        Text(
                            "Settings are available once the game has started.",
                            style = MaterialTheme.typography.bodyMedium,
                        )
                    }
                }

                GuestPanelOption(
                    label = "Back",
                    selected = !ready || selected == rows.size,
                    onClick = onBack,
                    modifier = Modifier.padding(top = 8.dp),
                )
            }
        }
    }
}

/** The highlight is an outline and a fill, as [GuestPanelOption]'s: never colour alone. */
@Composable
private fun InGameSettingRow(row: InGameRow, selected: Boolean, onClick: () -> Unit) {
    val shape = MaterialTheme.shapes.medium
    val colors = MaterialTheme.colorScheme
    Row(
        Modifier
            .fillMaxWidth()
            .padding(vertical = 2.dp)
            .clip(shape)
            .then(
                if (selected) {
                    Modifier
                        .background(colors.primaryContainer)
                        .border(2.dp, colors.onPrimaryContainer, shape)
                } else {
                    Modifier
                }
            )
            .clickable(onClick = onClick)
            .heightIn(min = 48.dp)
            .padding(horizontal = 12.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        val content = if (selected) colors.onPrimaryContainer else colors.onSurface
        Column(Modifier.weight(1f)) {
            Text(
                row.title,
                style = MaterialTheme.typography.bodyLarge,
                color = content,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                if (row.live) "Applies now" else "Applies on the next start",
                style = MaterialTheme.typography.labelSmall,
                color = if (selected) content else colors.onSurfaceVariant,
            )
        }
        Text(
            row.value(),
            style = MaterialTheme.typography.bodyMedium,
            color = if (selected) content else colors.primary,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(start = 12.dp).widthIn(max = 220.dp),
        )
    }
}
