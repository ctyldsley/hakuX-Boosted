package xendroid.compose;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/**
 * hakuX-Boosted: the one place a config file is written from Java or Kotlin.
 *
 * A config that is written in place can be left empty or half written when the
 * process dies mid-write, and the next start then takes it for missing and
 * falls back to defaults (which signs the player out). So:
 *
 * - the new text is written to a file beside the target, synced, and renamed
 *   over it, so the target is always either the old file or the new one;
 * - the file being replaced is kept beside it as {@code <name>.bak}.
 *
 * Plain Java with no Android types, so it can be tested on a desktop JVM. What
 * to do with a missing or unreadable config (restore the backup before any
 * default) is the app's policy, in settings/ConfigGuard.kt.
 */
public final class ConfigFiles {
    private ConfigFiles() {}

    public static final String BACKUP_SUFFIX = ".bak";
    /** A file that did not parse, kept so a hand edit with a mistake in it is not lost. */
    public static final String UNREADABLE_SUFFIX = ".unreadable";
    private static final String TEMP_SUFFIX = ".tmp";

    public static File backupOf(File target) {
        return new File(target.getPath() + BACKUP_SUFFIX);
    }

    public static File unreadableOf(File target) {
        return new File(target.getPath() + UNREADABLE_SUFFIX);
    }

    /** One name per writer, so two processes writing at once never share a temporary file. */
    private static File tempOf(File target) {
        return new File(target.getPath() + TEMP_SUFFIX + "." + System.nanoTime()
                + "." + Thread.currentThread().getId());
    }

    /**
     * Replaces {@code target} with {@code text}: written beside it, synced, renamed over it.
     * The file it replaces becomes the backup unless it is empty (an empty file is a
     * placeholder or a casualty, never worth keeping over an older good backup).
     */
    public static void writeAtomic(File target, String text) throws IOException {
        writeAtomic(target, text.getBytes(StandardCharsets.UTF_8), true);
    }

    public static void writeAtomic(File target, byte[] bytes, boolean keepBackup)
            throws IOException {
        File parent = target.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs() && !parent.isDirectory()) {
            throw new IOException("cannot create " + parent);
        }
        File temp = tempOf(target);
        try {
            writeSynced(temp, bytes);
            if (keepBackup && target.isFile() && target.length() > 0) {
                // A copy, not a rename: the target must not be absent at any moment.
                try {
                    copySynced(target, backupOf(target));
                } catch (IOException e) {
                    // Without a backup the write is still worth making.
                }
            }
            if (!temp.renameTo(target)) {
                // Some filesystems refuse to rename over an existing file.
                if (!target.delete() || !temp.renameTo(target)) {
                    throw new IOException("cannot move " + temp.getName() + " over " + target);
                }
            }
        } finally {
            if (temp.exists()) {
                //noinspection ResultOfMethodCallIgnored
                temp.delete();
            }
        }
    }

    /** Copies {@code source} to {@code destination} through a synced temporary file and a rename. */
    public static void copySynced(File source, File destination) throws IOException {
        byte[] bytes = readBytes(source);
        File temp = tempOf(destination);
        try {
            writeSynced(temp, bytes);
            if (!temp.renameTo(destination)) {
                if (!destination.delete() || !temp.renameTo(destination)) {
                    throw new IOException("cannot move " + temp.getName() + " over " + destination);
                }
            }
        } finally {
            if (temp.exists()) {
                //noinspection ResultOfMethodCallIgnored
                temp.delete();
            }
        }
    }

    private static void writeSynced(File file, byte[] bytes) throws IOException {
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(bytes);
            out.flush();
            try {
                out.getFD().sync();
            } catch (IOException e) {
                // A filesystem that cannot sync still gets the rename.
            }
        }
    }

    public static byte[] readBytes(File file) throws IOException {
        try (FileInputStream in = new FileInputStream(file)) {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream(
                    (int) Math.max(1024, Math.min(file.length(), 1 << 20)));
            byte[] buffer = new byte[16384];
            int n;
            while ((n = in.read(buffer)) > 0) {
                out.write(buffer, 0, n);
            }
            return out.toByteArray();
        }
    }

    /** The file's text, or null when it is missing or cannot be read. */
    public static String readTextOrNull(File file) {
        if (!file.isFile()) {
            return null;
        }
        try {
            return new String(readBytes(file), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return null;
        }
    }

    /**
     * How many settings a config's text holds: lines of the form {@code key = value}
     * outside comments. 0 means the text is empty or only comments and section names,
     * which no config the app or the core wrote ever is.
     */
    public static int countSettings(String text) {
        if (text == null) {
            return 0;
        }
        int count = 0;
        int start = 0;
        int length = text.length();
        while (start < length) {
            int end = text.indexOf('\n', start);
            if (end < 0) {
                end = length;
            }
            int i = start;
            while (i < end && (text.charAt(i) == ' ' || text.charAt(i) == '\t')) {
                i++;
            }
            if (i < end) {
                char first = text.charAt(i);
                if (first != '#' && first != '[') {
                    int equals = text.indexOf('=', i);
                    if (equals > i && equals < end) {
                        count++;
                    }
                }
            }
            start = end + 1;
        }
        return count;
    }

    /** Removes leftover temporary files of writers that died (older than a minute). */
    public static void sweepTemporaries(File target) {
        File parent = target.getParentFile();
        if (parent == null) {
            return;
        }
        final String prefix = target.getName() + TEMP_SUFFIX + ".";
        File[] stale = parent.listFiles((dir, name) -> name.startsWith(prefix));
        if (stale == null) {
            return;
        }
        long cutoff = System.currentTimeMillis() - 60_000L;
        for (File f : stale) {
            if (f.lastModified() < cutoff) {
                //noinspection ResultOfMethodCallIgnored
                f.delete();
            }
        }
    }
}
