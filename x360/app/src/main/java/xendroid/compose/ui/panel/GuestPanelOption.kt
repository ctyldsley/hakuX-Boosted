package xendroid.compose.ui.panel

import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp

/** One selectable option in a guest panel. The highlighted option is a filled+outlined
 *  Button and the rest are OutlinedButtons, so the highlight never depends on colour alone. */
@Composable
fun GuestPanelOption(
    label: String,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val shape = MaterialTheme.shapes.medium
    val sizing = modifier.fillMaxWidth().heightIn(min = 48.dp)
    val content = @Composable {
        Text(label, maxLines = 2, overflow = TextOverflow.Ellipsis)
    }
    if (selected) {
        // hakuX-Boosted: what a controller is on looks here as it does on every other
        // screen: a ring in the accent colour around it and a tonal fill, not a solid
        // accent block (ui/theme/HostTheme.kt, the focus ring).
        FilledTonalButton(
            onClick = onClick,
            shape = shape,
            modifier = sizing.border(2.dp, MaterialTheme.colorScheme.primary, shape),
        ) { content() }
    } else {
        OutlinedButton(onClick = onClick, shape = shape, modifier = sizing) { content() }
    }
}

/** The pinned option list at the bottom of a guest panel. */
@Composable
fun GuestPanelOptions(
    modifier: Modifier = Modifier,
    content: @Composable ColumnScope.() -> Unit,
) {
    Column(
        modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(8.dp),
        content = content,
    )
}

/** Side-by-side variant, for a panel whose labels are ours and known short. Give each option
 *  Modifier.weight(1f) so they share the width evenly. */
@Composable
fun GuestPanelOptionsRow(
    modifier: Modifier = Modifier,
    content: @Composable RowScope.() -> Unit,
) {
    Row(
        modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        content = content,
    )
}
