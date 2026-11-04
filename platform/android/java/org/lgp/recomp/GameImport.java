// First start of the Android app without game data (P7.3,
// docs/knowledge/android-ndk-build.md): the user picks the folder of their own
// dump with the system's folder picker (Storage Access Framework), and its
// exefs/ plus romfs.bin or romfs/ (and systemdata/ when present: the system
// fonts, without which LGP crashes before frame 1) are copied into the app's
// game folder. Nothing else is read; the app gets no broad storage permission.
//
// run() is called by the program (game_import.cpp) on SDL's main thread and
// blocks until the user is done; the dialogs run on the UI thread.
package org.lgp.recomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.util.Log;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;

public final class GameImport {
    private static final String TAG = "lgp";
    static final int REQUEST = 0x4c47;  // "LG"
    private static final long SPACE_RESERVE = 64L << 20;  // free space left after the copy

    // Result of run(): imported, cancelled, or an error text.
    public static final int IMPORTED = 0, CANCELLED = 1, FAILED = 2;

    private static final Object lock = new Object();
    private static CountDownLatch picked;
    private static Uri pickedTree;

    private static final class Entry {
        final String docId, path;  // path relative to the game folder, '/'-separated; docId null: a directory
        final long size;
        Entry(String docId, String path, long size) {
            this.docId = docId;
            this.path = path;
            this.size = size;
        }
    }

    private static final class Child {
        final String docId, name;
        final boolean dir;
        final long size;
        Child(String docId, String name, boolean dir, long size) {
            this.docId = docId;
            this.name = name;
            this.dir = dir;
            this.size = size;
        }
    }

    private GameImport() {}

    // Called from native code. message: why the game is needed (shown first).
    // dest: the game folder to fill (replaced only after a complete copy).
    // Returns IMPORTED, CANCELLED or FAILED; the error text goes to error[0].
    public static int run(final String message, final String dest, final String[] error) {
        final Activity activity = LgpActivity.current();
        if (activity == null) {
            error[0] = "No activity";
            return FAILED;
        }
        // 1. Explain, then the system's folder picker.
        final int[] choice = {-1};
        final CountDownLatch asked = new CountDownLatch(1);
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                new AlertDialog.Builder(activity)
                        .setTitle("LGP")
                        .setMessage(message)
                        .setCancelable(false)
                        .setPositiveButton(text(activity, "import_choose_folder"), answer(choice, 0, asked))
                        .setNegativeButton(text(activity, "import_quit"), answer(choice, 1, asked))
                        .show();
            }
        });
        if (!await(asked) || choice[0] != 0)
            return CANCELLED;
        final Uri tree;
        synchronized (lock) {
            picked = new CountDownLatch(1);
            pickedTree = null;
        }
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    activity.startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST);
                } catch (Exception e) {
                    Log.e(TAG, "folder picker: " + e);
                    onPicked(null);
                }
            }
        });
        CountDownLatch latch;
        synchronized (lock) {
            latch = picked;
        }
        if (!await(latch))
            return CANCELLED;
        synchronized (lock) {
            tree = pickedTree;
        }
        if (tree == null)
            return CANCELLED;

        // 2. What to copy.
        final ContentResolver resolver = activity.getContentResolver();
        final List<Entry> files = new ArrayList<>();
        try {
            final String problem = plan(activity, resolver, tree, files);
            if (problem != null) {
                error[0] = problem;
                return FAILED;
            }
        } catch (Exception e) {
            error[0] = text(activity, "import_unreadable", e.getMessage());
            return FAILED;
        }
        long total = 0;
        for (Entry f : files)
            total += f.size;

        // 3. Space: the copy goes next to the game folder first.
        final File target = new File(dest);
        final File parent = target.getParentFile();
        final File staging = new File(parent, target.getName() + ".import");
        if (parent == null || (!parent.isDirectory() && !parent.mkdirs())) {
            error[0] = text(activity, "import_no_app_folder", dest);
            return FAILED;
        }
        deleteTree(staging);
        final long free = new StatFs(parent.getAbsolutePath()).getAvailableBytes();
        if (free < total + SPACE_RESERVE) {
            error[0] = text(activity, "import_no_space", mib(total + SPACE_RESERVE), mib(free));
            return FAILED;
        }

        // 4. Copy with a progress dialog (Cancel stops after the current block).
        final boolean[] cancel = {false};
        final ProgressBar bar = new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal);
        final TextView label = new TextView(activity);
        final AlertDialog[] dialog = {null};
        final CountDownLatch shown = new CountDownLatch(1);
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                LinearLayout box = new LinearLayout(activity);
                box.setOrientation(LinearLayout.VERTICAL);
                int pad = (int) (16 * activity.getResources().getDisplayMetrics().density);
                box.setPadding(pad, pad, pad, pad);
                bar.setMax(1000);
                box.addView(bar);
                box.addView(label);
                dialog[0] = new AlertDialog.Builder(activity)
                        .setTitle(text(activity, "import_copying"))
                        .setView(box)
                        .setCancelable(false)
                        .setNegativeButton(text(activity, "import_cancel"), new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface d, int which) {
                                synchronized (cancel) {
                                    cancel[0] = true;
                                }
                            }
                        })
                        .show();
                activity.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                shown.countDown();
            }
        });
        await(shown);
        int result;
        try {
            result = copy(resolver, tree, files, staging, total, bar, label, cancel, activity) ? IMPORTED : CANCELLED;
            if (result == IMPORTED) {
                deleteTree(target);
                if (!staging.renameTo(target))
                    throw new IOException("cannot rename " + staging + " to " + target);
            }
        } catch (Exception e) {
            Log.e(TAG, "import: " + e);
            error[0] = text(activity, "import_copy_failed", e.getMessage());
            result = FAILED;
        }
        if (result != IMPORTED)
            deleteTree(staging);
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (dialog[0] != null)
                    dialog[0].dismiss();
                activity.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            }
        });
        Log.i(TAG, "import: " + (result == IMPORTED ? "done, " + mib(total) : result == CANCELLED ? "cancelled" : error[0]));
        return result;
    }

    // A text of res/values/strings.xml in the device's language (German from
    // res/values-de), by name: the APK is linked without a generated R class
    // (tools/android/build_apk.py). Unknown names come back as they are.
    static String text(Context context, String name, Object... args) {
        int id = context.getResources().getIdentifier(name, "string", context.getPackageName());
        if (id == 0)
            return name;
        return args.length == 0 ? context.getString(id) : context.getString(id, args);
    }

    // From LgpActivity.onActivityResult (null: cancelled).
    static void onPicked(Uri tree) {
        synchronized (lock) {
            pickedTree = tree;
            if (picked != null)
                picked.countDown();
        }
    }

    // The dump is the picked folder or one folder inside it: exefs/ and either
    // romfs.bin or romfs/, optionally systemdata/. Returns a problem text, or
    // null with `out` filled.
    private static String plan(Context context, ContentResolver resolver, Uri tree, List<Entry> out) {
        String root = DocumentsContract.getTreeDocumentId(tree);
        List<Child> top = children(resolver, tree, root);
        if (find(top, "exefs", true) == null) {
            Child inner = null;
            for (Child c : top)
                if (c.dir && find(children(resolver, tree, c.docId), "exefs", true) != null) {
                    if (inner != null)
                        return text(context, "import_several_games");
                    inner = c;
                }
            if (inner == null)
                return text(context, "import_no_exefs");
            top = children(resolver, tree, inner.docId);
        }
        collect(resolver, tree, find(top, "exefs", true), "exefs", out);
        if (out.isEmpty())
            return text(context, "import_exefs_empty");
        Child bin = find(top, "romfs.bin", false);
        Child dir = find(top, "romfs", true);
        if (bin != null)
            out.add(new Entry(bin.docId, "romfs.bin", bin.size));
        else if (dir != null)
            collect(resolver, tree, dir, "romfs", out);
        else
            return text(context, "import_no_romfs");
        Child sys = find(top, "systemdata", true);
        if (sys != null)
            collect(resolver, tree, sys, "systemdata", out);
        else
            Log.w(TAG, "game import: no systemdata/ next to exefs (system fonts missing, pl:u reports it)");
        return null;
    }

    private static void collect(ContentResolver resolver, Uri tree, Child dir, String path, List<Entry> out) {
        for (Child c : children(resolver, tree, dir.docId)) {
            String p = path + "/" + c.name;
            if (c.dir) {
                out.add(new Entry(null, p, 0));  // kept even when empty (guest-visible listing)
                collect(resolver, tree, c, p, out);
            } else
                out.add(new Entry(c.docId, p, c.size));
        }
    }

    private static Child find(List<Child> list, String name, boolean dir) {
        for (Child c : list)
            if (c.dir == dir && c.name.equals(name))
                return c;
        return null;
    }

    private static List<Child> children(ContentResolver resolver, Uri tree, String docId) {
        List<Child> out = new ArrayList<>();
        Uri uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        String[] cols = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                         DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE};
        try (Cursor c = resolver.query(uri, cols, null, null, null)) {
            while (c != null && c.moveToNext()) {
                String name = c.getString(1);
                // Names come from the provider: no path separators or dot entries.
                if (name == null || name.isEmpty() || name.contains("/") || name.equals(".") || name.equals(".."))
                    continue;
                boolean dir = DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2));
                out.add(new Child(c.getString(0), name, dir, c.isNull(3) ? 0 : c.getLong(3)));
            }
        }
        return out;
    }

    private static boolean copy(ContentResolver resolver, Uri tree, List<Entry> files, File staging, long total,
                                ProgressBar bar, TextView label, boolean[] cancel, Activity activity)
            throws IOException {
        byte[] buf = new byte[1 << 20];
        long done = 0, shownAt = 0;
        for (Entry f : files) {
            File out = new File(staging, f.path);
            if (f.docId == null) {
                if (!out.isDirectory() && !out.mkdirs())
                    throw new IOException("cannot create " + out);
                continue;
            }
            File dir = out.getParentFile();
            if (dir != null && !dir.isDirectory() && !dir.mkdirs())
                throw new IOException("cannot create " + dir);
            Uri uri = DocumentsContract.buildDocumentUriUsingTree(tree, f.docId);
            try (InputStream in = resolver.openInputStream(uri); OutputStream os = new FileOutputStream(out)) {
                if (in == null)
                    throw new IOException("cannot open " + f.path);
                for (int n; (n = in.read(buf)) > 0;) {
                    os.write(buf, 0, n);
                    done += n;
                    synchronized (cancel) {
                        if (cancel[0])
                            return false;
                    }
                    long now = System.nanoTime();
                    if (now - shownAt > 200_000_000L) {
                        shownAt = now;
                        final int permille = total > 0 ? (int) Math.min(1000, done * 1000 / total) : 0;
                        final String line = text(activity, "import_progress", mib(done), mib(total), f.path);
                        activity.runOnUiThread(new Runnable() {
                            @Override
                            public void run() {
                                bar.setProgress(permille);
                                label.setText(line);
                            }
                        });
                    }
                }
            }
        }
        return true;
    }

    private static void deleteTree(File f) {
        File[] list = f.listFiles();
        if (list != null)
            for (File c : list)
                deleteTree(c);
        if (f.exists() && !f.delete())
            Log.w(TAG, "cannot delete " + f);
    }

    private static DialogInterface.OnClickListener answer(final int[] choice, final int value,
                                                          final CountDownLatch done) {
        return new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface d, int which) {
                choice[0] = value;
                done.countDown();
            }
        };
    }

    private static boolean await(CountDownLatch latch) {
        try {
            latch.await();
            return true;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
    }

    private static String mib(long bytes) {
        return String.format("%.0f MiB", bytes / 1048576.0);
    }
}
