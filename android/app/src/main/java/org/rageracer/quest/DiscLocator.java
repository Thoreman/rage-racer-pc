package org.rageracer.quest;

import android.content.Context;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Where the game's disc image is. The native side (android_bootstrap.c) looks
 * in files/disc first, then at the path remembered in files/disc-location.txt.
 */
final class DiscLocator {
    static final String LOCATION_FILE = "disc-location.txt";
    private static final Pattern CUE_FILE =
            Pattern.compile("^\\s*FILE\\s+\"([^\"]+)\"", Pattern.CASE_INSENSITIVE);

    private DiscLocator() {}

    static File appDiscDirectory(Context context) {
        return new File(context.getExternalFilesDir(null), "disc");
    }

    static boolean isImageName(String name) {
        String lower = name.toLowerCase(Locale.ROOT);
        return lower.endsWith(".cue") || lower.endsWith(".chd");
    }

    /** The track files a CUE sheet names, in order. */
    static List<String> cueTracks(InputStream cue) throws IOException {
        List<String> tracks = new ArrayList<>();
        BufferedReader reader = new BufferedReader(
                new InputStreamReader(cue, StandardCharsets.ISO_8859_1));
        String line;
        while ((line = reader.readLine()) != null) {
            Matcher matcher = CUE_FILE.matcher(line);
            if (matcher.find()) tracks.add(matcher.group(1));
        }
        return tracks;
    }

    /* The boot file names of the Rage Racer releases (Europe, USA, Japan).
     * They appear in the disc's root directory near the start of track 1. */
    private static final String[] SERIALS = {
        "SCES_006.50", "SLUS_004.03", "SLPS_006.00",
    };
    private static final int SERIAL_SCAN_BYTES = 1 << 20;

    /** True when the start of a data track names a Rage Racer boot file. */
    static boolean isRageRacerTrack(InputStream track) throws IOException {
        byte[] head = new byte[SERIAL_SCAN_BYTES];
        int length = 0;
        int count;
        String text;
        while (length < head.length &&
               (count = track.read(head, length, head.length - length)) > 0) {
            length += count;
        }
        text = new String(head, 0, length, StandardCharsets.ISO_8859_1);
        for (String serial : SERIALS)
            if (text.contains(serial)) return true;
        return false;
    }

    /** A compressed CHD cannot be read here; go by its name. */
    static boolean isRageRacerName(String name) {
        String lower = name.toLowerCase(Locale.ROOT);
        return lower.contains("rage") && lower.contains("racer");
    }

    /**
     * A Rage Racer image that is all there: a CHD named for the game, or a
     * CUE whose every track file sits beside it and whose first track is the
     * game's.
     */
    static boolean isCompleteImage(File image) {
        if (!image.isFile() || !image.canRead()) return false;
        if (!image.getName().toLowerCase(Locale.ROOT).endsWith(".cue"))
            return isRageRacerName(image.getName());
        try (InputStream in = new java.io.FileInputStream(image)) {
            List<String> tracks = cueTracks(in);
            if (tracks.isEmpty()) return false;
            for (String track : tracks) {
                File file = new File(image.getParentFile(), track);
                if (!file.isFile() || !file.canRead()) return false;
            }
            try (InputStream first = new java.io.FileInputStream(
                    new File(image.getParentFile(), tracks.get(0)))) {
                return isRageRacerTrack(first);
            }
        } catch (IOException e) {
            return false;
        }
    }

    /** A usable image directly inside the directory, or null. */
    static File findImageIn(File directory) {
        File[] files = directory.listFiles();
        File chd = null;
        if (files == null) return null;
        for (File file : files) {
            if (!file.isFile() || !isImageName(file.getName())) continue;
            if (!isCompleteImage(file)) continue;
            /* A CUE carries the CD audio tracks; prefer it over a CHD. */
            if (file.getName().toLowerCase(Locale.ROOT).endsWith(".cue")) return file;
            chd = file;
        }
        return chd;
    }

    /** Searches a folder tree (bounded depth) for a usable image. */
    static File search(File directory, int depth) {
        File found = findImageIn(directory);
        File[] files;
        if (found != null || depth <= 0) return found;
        files = directory.listFiles();
        if (files == null) return null;
        for (File file : files) {
            if (!file.isDirectory() || file.getName().startsWith(".")) continue;
            /* Other apps' private folders are large and never hold it. */
            if (file.getName().equals("Android")) continue;
            found = search(file, depth - 1);
            if (found != null) return found;
        }
        return null;
    }

    static File rememberedImage(Context context) {
        File note = new File(context.getExternalFilesDir(null), LOCATION_FILE);
        try (BufferedReader reader = new BufferedReader(new FileReader(note))) {
            String path = reader.readLine();
            return path == null || path.trim().isEmpty() ? null : new File(path.trim());
        } catch (IOException e) {
            return null;
        }
    }

    static void remember(Context context, File image) throws IOException {
        File note = new File(context.getExternalFilesDir(null), LOCATION_FILE);
        try (FileOutputStream out = new FileOutputStream(note)) {
            out.write((image.getAbsolutePath() + "\n").getBytes(StandardCharsets.UTF_8));
        }
    }

    /** True when the game will find a disc image on start. */
    static boolean hasDisc(Context context) {
        File directory = appDiscDirectory(context);
        File remembered;
        if (directory.isDirectory() && findImageIn(directory) != null) return true;
        remembered = rememberedImage(context);
        return remembered != null && isCompleteImage(remembered);
    }
}
