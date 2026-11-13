package xendroid.compose.settings

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import xendroid.compose.ConfigFiles
import java.io.File
import java.nio.file.Files

/** hakuX-Boosted: the whole-file writer and the pure parts of the config guard. */
class ConfigFilesTest {

    private fun tempDir(): File = Files.createTempDirectory("configfiles").toFile()

    @Test
    fun `a write keeps the file it replaces as the backup`() {
        val file = File(tempDir(), "x.config.toml")
        ConfigFiles.writeAtomic(file, "[A]\na = 1\n")
        assertFalse(ConfigFiles.backupOf(file).exists())
        ConfigFiles.writeAtomic(file, "[A]\na = 2\n")
        assertEquals("[A]\na = 2\n", file.readText())
        assertEquals("[A]\na = 1\n", ConfigFiles.backupOf(file).readText())
    }

    @Test
    fun `an empty file never replaces a backup`() {
        val file = File(tempDir(), "x.config.toml")
        ConfigFiles.writeAtomic(file, "[A]\na = 1\n")
        ConfigFiles.writeAtomic(file, ByteArray(0), true)
        assertEquals("[A]\na = 1\n", ConfigFiles.backupOf(file).readText())
        ConfigFiles.writeAtomic(file, "[A]\na = 3\n")
        assertEquals("[A]\na = 1\n", ConfigFiles.backupOf(file).readText())
    }

    @Test
    fun `no temporary file is left behind`() {
        val dir = tempDir()
        val file = File(dir, "x.config.toml")
        ConfigFiles.writeAtomic(file, "[A]\na = 1\n")
        ConfigFiles.writeAtomic(file, "[A]\na = 2\n")
        assertEquals(setOf("x.config.toml", "x.config.toml.bak"), dir.list()!!.toSet())
    }

    @Test
    fun `settings are counted, comments and section names are not`() {
        assertEquals(0, ConfigFiles.countSettings(""))
        assertEquals(0, ConfigFiles.countSettings("# a = 1\n[S]\n\n"))
        assertEquals(2, ConfigFiles.countSettings("[A]\na = 2\n# c\n[B]\n  b = \"x=y\"\n"))
        assertEquals(0, ConfigFiles.countSettings(null))
    }

    @Test
    fun `setting names come with their section, in file order`() {
        val names = ConfigGuard.settingNames(
            "[GPU]\ngpu = \"vulkan\"   \t# Graphics system\n\n# note = 1\n[Vulkan]\nvulkan_device = -1\n")
        assertEquals(listOf("GPU" to "gpu", "Vulkan" to "vulkan_device"), names)
        assertTrue(ConfigGuard.settingNames("orphan = 1\n").isEmpty())
    }
}
