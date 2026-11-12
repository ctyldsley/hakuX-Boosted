package com.rfandango.haku_x.boosted

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.unit.dp
import xendroid.compose.ui.theme.focusRing

/*
 * hakuX-Boosted: the settings screen's three tabs. hakuX's ui/settings/SettingsScreen.kt
 * keeps its sections and groups them by tab with single marked lines; what the tabs
 * themselves are is here, and the Xbox 360 rows are in X360.kt.
 */

/** The settings' tabs: what both consoles share, the Xbox core's, the Xbox 360 core's. */
enum class SettingsTab(val title: String) {
  GLOBAL("Global"),
  XBOX("Xbox"),
  XBOX_360("Xbox 360"),
}

/** The tabs on this phone: the Xbox 360 core needs Android 10 (X360.available). */
private val settingsTabs: List<SettingsTab>
  get() = if (X360.available) SettingsTab.entries else SettingsTab.entries - SettingsTab.XBOX_360

/** The tab the settings show, kept across a rotation and a visit to a screen opened from them. */
@Composable
fun rememberSettingsTab(): MutableState<SettingsTab> = rememberSaveable { mutableStateOf(SettingsTab.GLOBAL) }

/**
 * The settings screen's frame under its top bar: the tab row, and under it the tab's
 * content, which is handed no padding of its own ([padding], the screen's, is applied
 * here). Each tab keeps its own scroll position and open sections while another shows.
 *
 * With a controller: the D-pad reaches the row, A chooses a tab, and the shoulder buttons
 * (L1, R1) step through the tabs from anywhere on the screen.
 */
@Composable
fun SettingsTabsFrame(
  tab: SettingsTab,
  onSelect: (SettingsTab) -> Unit,
  padding: PaddingValues,
  content: @Composable (PaddingValues) -> Unit,
) {
  val tabs = settingsTabs
  val shown = if (tab in tabs) tab else SettingsTab.GLOBAL
  val holder = rememberSaveableStateHolder()
  Column(
    Modifier
      .padding(padding)
      .fillMaxSize()
      .onPreviewKeyEvent { event ->
        val step = when (event.key) {
          Key.ButtonL1 -> -1
          Key.ButtonR1 -> 1
          else -> 0
        }
        if (step == 0) {
          false
        } else {
          if (event.type == KeyEventType.KeyDown) {
            onSelect(tabs[(tabs.indexOf(shown) + step).coerceIn(0, tabs.lastIndex)])
          }
          true
        }
      },
  ) {
    TabRow(
      selectedTabIndex = tabs.indexOf(shown),
      containerColor = MaterialTheme.colorScheme.surface,
      contentColor = MaterialTheme.colorScheme.primary,
    ) {
      for (t in tabs) {
        Tab(
          selected = t == shown,
          onClick = { onSelect(t) },
          text = { Text(t.title, style = MaterialTheme.typography.titleSmall) },
          selectedContentColor = MaterialTheme.colorScheme.primary,
          unselectedContentColor = MaterialTheme.colorScheme.onSurfaceVariant,
          modifier = Modifier.focusRing(),
        )
      }
    }
    holder.SaveableStateProvider(shown.name) {
      content(PaddingValues(0.dp))
    }
  }
}

/** Within a section, whose console's games the rows under it are for. */
@Composable
fun SettingsSubSection(title: String) {
  Text(
    title,
    style = MaterialTheme.typography.labelMedium,
    color = MaterialTheme.colorScheme.onSurfaceVariant,
    modifier = Modifier.padding(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 2.dp),
  )
}

