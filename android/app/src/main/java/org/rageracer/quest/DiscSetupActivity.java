package org.rageracer.quest;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Shown as a flat panel when the game has no disc image. Two ways to supply
 * one: let the app search the headset's storage (needs "all files access"),
 * or pick the folder that holds it. Either way the choice is remembered and
 * the game starts.
 */
public class DiscSetupActivity extends Activity {
    private static final String TAG = "RageDiscSetup";
    private static final int REQUEST_ALL_FILES = 1;
    private static final int REQUEST_FOLDER = 2;
    private static final int SEARCH_DEPTH = 4;

    private final Handler main = new Handler(Looper.getMainLooper());
    private TextView status;
    private Button searchButton;
    private Button folderButton;
    private Button startButton;
    private boolean busy;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(buildLayout());
        if (DiscLocator.hasDisc(this)) {
            showReady("A disc image is already set up.");
        } else if (Environment.isExternalStorageManager()) {
            search();
        }
    }

    private View buildLayout() {
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        LinearLayout column = new LinearLayout(this);
        TextView title = new TextView(this);
        TextView intro = new TextView(this);
        ScrollView scroll = new ScrollView(this);

        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(pad, pad, pad, pad);
        column.setGravity(Gravity.CENTER_HORIZONTAL);

        title.setText("Rage Racer needs your disc image");
        title.setTextSize(26);
        column.addView(title);

        intro.setText("Copy your own Rage Racer disc image onto this headset "
                + "first: a .cue file with all of its .bin tracks in one "
                + "folder, or a single .chd file. The Download folder is a "
                + "good place.\n\nThen choose one of these:");
        intro.setTextSize(17);
        intro.setPadding(0, pad / 2, 0, pad / 2);
        column.addView(intro);

        searchButton = addButton(column, "Search this headset for it");
        searchButton.setOnClickListener(v -> onSearchPressed());
        folderButton = addButton(column, "Choose the folder myself");
        folderButton.setOnClickListener(v -> pickFolder());

        status = new TextView(this);
        status.setTextSize(17);
        status.setPadding(0, pad / 2, 0, pad / 2);
        column.addView(status);

        startButton = addButton(column, "Start Rage Racer");
        startButton.setVisibility(View.GONE);
        startButton.setOnClickListener(v -> startGame());

        scroll.addView(column);
        return scroll;
    }

    private Button addButton(LinearLayout column, String label) {
        Button button = new Button(this);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        params.topMargin = (int) (8 * getResources().getDisplayMetrics().density);
        button.setText(label);
        button.setTextSize(18);
        button.setAllCaps(false);
        column.addView(button, params);
        return button;
    }

    private void setBusy(boolean value) {
        busy = value;
        searchButton.setEnabled(!value);
        folderButton.setEnabled(!value);
    }

    private void say(String text) {
        Log.i(TAG, text);
        status.setText(text);
    }

    private void showReady(String text) {
        say(text);
        startButton.setVisibility(View.VISIBLE);
    }

    /* ---- search the headset ---- */

    private void onSearchPressed() {
        if (Environment.isExternalStorageManager()) {
            search();
            return;
        }
        say("Allow \"all files access\" for Rage Racer on the next screen, "
                + "then come back here.");
        try {
            startActivityForResult(new Intent(
                    Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())), REQUEST_ALL_FILES);
        } catch (RuntimeException first) {
            try {
                startActivityForResult(new Intent(
                        Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION),
                        REQUEST_ALL_FILES);
            } catch (RuntimeException second) {
                say("This headset would not open the file access setting. "
                        + "Use \"Choose the folder myself\" instead.");
            }
        }
    }

    private void search() {
        if (busy) return;
        setBusy(true);
        say("Searching the headset's storage...");
        new Thread(() -> {
            File root = Environment.getExternalStorageDirectory();
            /* Downloads first: it is where the browser and most file
             * transfers put things. */
            File found = DiscLocator.search(new File(root, "Download"), SEARCH_DEPTH);
            if (found == null) found = DiscLocator.search(root, SEARCH_DEPTH);
            final File image = found;
            main.post(() -> {
                setBusy(false);
                if (image == null) {
                    say("No complete Rage Racer disc image found. A .cue "
                            + "needs every .bin track it lists in the same "
                            + "folder; a .chd needs \"Rage Racer\" in its "
                            + "name. Copy "
                            + "the files over and search again, or choose "
                            + "the folder yourself.");
                } else {
                    useInPlace(image);
                }
            });
        }).start();
    }

    private void useInPlace(File image) {
        try {
            DiscLocator.remember(this, image);
            showReady("Found " + image.getAbsolutePath()
                    + "\nThe game will use it from there; keep the files "
                    + "where they are.");
        } catch (IOException e) {
            say("Could not save the location: " + e.getMessage());
        }
    }

    /* ---- pick a folder ---- */

    private void pickFolder() {
        try {
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE),
                    REQUEST_FOLDER);
        } catch (RuntimeException e) {
            say("This headset has no folder picker. Use \"Search this "
                    + "headset\" instead.");
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == REQUEST_ALL_FILES) {
            if (Environment.isExternalStorageManager()) search();
            else say("File access was not allowed. You can still choose the "
                    + "folder yourself.");
        } else if (request == REQUEST_FOLDER && result == RESULT_OK
                && data != null && data.getData() != null) {
            onFolderPicked(data.getData());
        }
    }

    /** The real path of a picked folder on the headset's own storage. */
    private static File pathOfTree(Uri tree) {
        String id = DocumentsContract.getTreeDocumentId(tree);
        if (id == null || !id.startsWith("primary:")) return null;
        return new File(Environment.getExternalStorageDirectory(),
                id.substring("primary:".length()));
    }

    private void onFolderPicked(Uri tree) {
        File directory = pathOfTree(tree);
        /* With file access the image is used where it lies: nothing to copy
         * and nothing duplicated. */
        if (directory != null && Environment.isExternalStorageManager()) {
            File image = DiscLocator.search(directory, 1);
            if (image != null) {
                useInPlace(image);
                return;
            }
        }
        copyFromTree(tree);
    }

    private Map<String, Uri> listTree(Uri tree) {
        Map<String, Uri> files = new HashMap<>();
        ContentResolver resolver = getContentResolver();
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(
                tree, DocumentsContract.getTreeDocumentId(tree));
        try (Cursor cursor = resolver.query(children, new String[] {
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
            while (cursor != null && cursor.moveToNext()) {
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2)))
                    continue;
                files.put(cursor.getString(1), DocumentsContract
                        .buildDocumentUriUsingTree(tree, cursor.getString(0)));
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot list " + tree, e);
        }
        return files;
    }

    private static String findName(Map<String, Uri> files, String suffix) {
        for (String name : files.keySet())
            if (name.toLowerCase(Locale.ROOT).endsWith(suffix)) return name;
        return null;
    }

    private static Uri findIgnoringCase(Map<String, Uri> files, String wanted) {
        Uri exact = files.get(wanted);
        if (exact != null) return exact;
        for (Map.Entry<String, Uri> entry : files.entrySet())
            if (entry.getKey().equalsIgnoreCase(wanted)) return entry.getValue();
        return null;
    }

    private void copyFromTree(Uri tree) {
        if (busy) return;
        setBusy(true);
        say("Looking in that folder...");
        new Thread(() -> {
            String outcome;
            boolean ok = false;
            try {
                outcome = copyImage(listTree(tree));
                ok = true;
            } catch (IOException e) {
                outcome = e.getMessage();
            }
            final String message = outcome;
            final boolean success = ok;
            main.post(() -> {
                setBusy(false);
                if (success) showReady(message);
                else say(message);
            });
        }).start();
    }

    /** Copies the image in the listed folder into the app's own disc folder. */
    private String copyImage(Map<String, Uri> files) throws IOException {
        ContentResolver resolver = getContentResolver();
        File target = DiscLocator.appDiscDirectory(this);
        Map<String, Uri> wanted = new HashMap<>();
        String cue = findName(files, ".cue");
        String chd = findName(files, ".chd");
        int done = 0;

        if (cue != null) {
            List<String> tracks;
            try (InputStream in = resolver.openInputStream(files.get(cue))) {
                tracks = DiscLocator.cueTracks(in);
            }
            if (tracks.isEmpty())
                throw new IOException(cue + " lists no track files.");
            for (String track : tracks) {
                Uri uri = findIgnoringCase(files, track);
                if (uri == null)
                    throw new IOException("That folder is missing \"" + track
                            + "\", which " + cue + " needs.");
                wanted.put(track, uri);
            }
            try (InputStream first =
                         resolver.openInputStream(wanted.get(tracks.get(0)))) {
                if (first == null || !DiscLocator.isRageRacerTrack(first))
                    throw new IOException(cue + " is not a Rage Racer disc.");
            }
            /* Written last, so a half-finished copy is never mistaken for a
             * complete image. */
            wanted.put(cue, files.get(cue));
        } else if (chd != null) {
            if (!DiscLocator.isRageRacerName(chd))
                throw new IOException(chd + " does not look like Rage Racer "
                        + "(its name should contain \"Rage Racer\").");
            wanted.put(chd, files.get(chd));
        } else {
            throw new IOException("No .cue or .chd file in that folder.");
        }

        if (!target.isDirectory() && !target.mkdirs())
            throw new IOException("Cannot create " + target);
        for (int pass = 0; pass < 2; pass++) {
            for (Map.Entry<String, Uri> entry : wanted.entrySet()) {
                boolean isCue = entry.getKey().equals(cue);
                if (isCue != (pass == 1)) continue;
                done++;
                final String progress = "Copying " + done + " of " + wanted.size()
                        + ": " + entry.getKey();
                main.post(() -> say(progress));
                copy(resolver, entry.getValue(), new File(target, entry.getKey()));
            }
        }
        return "Copied the disc image into the game's own folder. You can "
                + "delete the original files if you want the space back.";
    }

    private static void copy(ContentResolver resolver, Uri from, File to)
            throws IOException {
        byte[] buffer = new byte[1 << 20];
        File partial = new File(to.getPath() + ".part");
        try (InputStream in = resolver.openInputStream(from);
             OutputStream out = new FileOutputStream(partial)) {
            int count;
            if (in == null) throw new IOException("Cannot read " + to.getName());
            while ((count = in.read(buffer)) > 0) out.write(buffer, 0, count);
        }
        if (!partial.renameTo(to))
            throw new IOException("Cannot write " + to.getName());
    }

    /* ---- hand over to the game ---- */

    private void startGame() {
        Intent game = new Intent(this, RageRacerActivity.class);
        game.setAction(Intent.ACTION_MAIN);
        game.addCategory(Intent.CATEGORY_LAUNCHER);
        game.addCategory("com.oculus.intent.category.VR");
        game.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(game);
        finish();
    }
}
