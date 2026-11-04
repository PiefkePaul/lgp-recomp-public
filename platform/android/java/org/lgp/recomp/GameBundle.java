// Game data inside the app (P7.3, docs/knowledge/android-ndk-build.md): built
// only on the user's own PC (tools/android/build_game_splits.py), the dump
// travels in split APKs next to the program's APK:
//   assets/game/exefs/<file>        unpacked once into <app>/game/exefs
//   assets/game/systemdata/<id>/... unpacked once into <app>/game/systemdata
//                                   (system fonts, docs/knowledge/horizon-pl-shared-font.md)
//   assets/game/mods/...            unpacked once into <app>/game/mods (mod files
//                                   of the kit build, tools/kit/build.py)
//   assets/game/romfs.bin.<n>       parts below 2 GiB, stored uncompressed,
//                                   read in place (no copy)
// prepare() hands the parts to the program as LGP_APP_GAME_FILES
// ("romfs.bin=/proc/self/fd/N@OFFSET+LENGTH,..."); the descriptors stay open
// for the life of the process.
package org.lgp.recomp;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.res.AssetFileDescriptor;
import android.content.res.AssetManager;
import android.system.Os;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

final class GameBundle {
    private static final String TAG = "lgp";
    private static final String DIR = "game";
    private static final String PARTS = "romfs.bin.";
    private static final List<AssetFileDescriptor> open = new ArrayList<>();  // keeps the fds alive

    private GameBundle() {}

    // appDir: LGP_APP_DIR. Does nothing for an APK without game data.
    static void prepare(Context context, File appDir) {
        try {
            AssetManager assets = context.getAssets();
            String[] names = assets.list(DIR);
            if (names == null || names.length == 0)
                return;
            long t0 = System.nanoTime();
            unpack(context, assets, "exefs", new File(appDir, DIR + "/exefs"));
            unpack(context, assets, "systemdata", new File(appDir, DIR + "/systemdata"));
            unpack(context, assets, "mods", new File(appDir, DIR + "/mods"));
            String spec = romfsSpec(assets, names);
            if (spec != null)
                Os.setenv("LGP_APP_GAME_FILES", "romfs.bin=" + spec, true);
            Log.i(TAG, "game bundle: ready in " + (System.nanoTime() - t0) / 1000000 + " ms"
                    + (spec != null ? ", romfs.bin from the APK" : ", no romfs.bin parts"));
        } catch (Exception e) {
            // The app still starts and asks for the game (GameImport).
            Log.e(TAG, "game bundle: " + e);
        }
    }

    // The parts in order (romfs.bin.0, .1, ...); null without parts.
    private static String romfsSpec(AssetManager assets, String[] names) throws IOException {
        List<String> parts = new ArrayList<>();
        for (int i = 0; Arrays.asList(names).contains(PARTS + i); ++i)
            parts.add(PARTS + i);
        if (parts.isEmpty())
            return null;
        StringBuilder spec = new StringBuilder();
        for (String part : parts) {
            // Only for assets stored uncompressed (build_game_splits.py).
            AssetFileDescriptor fd = assets.openFd(DIR + "/" + part);
            open.add(fd);
            if (spec.length() > 0)
                spec.append(',');
            spec.append("/proc/self/fd/").append(fd.getParcelFileDescriptor().getFd()).append('@')
                .append(fd.getStartOffset()).append('+').append(fd.getLength());
        }
        return spec.toString();
    }

    // assets/game/<sub> into dest, again when the app was installed or updated
    // since the last time. When the APK has no such folder, only removes a
    // dest an earlier APK unpacked (its stamp; e.g. the mods of an older kit
    // build), not one the user copied there.
    private static void unpack(Context context, AssetManager assets, String sub, File dest) throws Exception {
        String[] files = assets.list(DIR + "/" + sub);
        File stamp = new File(dest, ".lgp-unpacked");
        if (files == null || files.length == 0) {
            if (stamp.isFile()) {
                deleteTree(dest);
                Log.i(TAG, "game bundle: removed " + sub + " of an earlier version");
            }
            return;
        }
        PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
        String want = Long.toString(info.lastUpdateTime);
        if (stamp.isFile() && want.equals(readAll(stamp)))
            return;
        deleteTree(dest);  // a file the new APK no longer has (a dropped mod) must not stay
        int count = copyTree(assets, DIR + "/" + sub, dest, new byte[1 << 20]);
        try (OutputStream out = new FileOutputStream(stamp)) {
            out.write(want.getBytes("UTF-8"));
        }
        Log.i(TAG, "game bundle: unpacked " + count + " " + sub + " files");
    }

    // AssetManager.list returns the children of a folder and nothing for a file.
    private static int copyTree(AssetManager assets, String path, File dest, byte[] buf) throws IOException {
        if (!dest.isDirectory() && !dest.mkdirs())
            throw new IOException("cannot create " + dest);
        int count = 0;
        for (String name : assets.list(path)) {
            String child = path + "/" + name;
            String[] inner = assets.list(child);
            if (inner != null && inner.length > 0) {
                count += copyTree(assets, child, new File(dest, name), buf);
                continue;
            }
            try (InputStream in = assets.open(child);
                 OutputStream out = new FileOutputStream(new File(dest, name))) {
                for (int n; (n = in.read(buf)) > 0;)
                    out.write(buf, 0, n);
            }
            ++count;
        }
        return count;
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null)
            for (File c : children)
                deleteTree(c);
        f.delete();
    }

    private static String readAll(File f) throws IOException {
        byte[] data = new byte[(int) f.length()];
        try (InputStream in = new java.io.FileInputStream(f)) {
            int off = 0;
            for (int n; off < data.length && (n = in.read(data, off, data.length - off)) > 0;)
                off += n;
        }
        return new String(data, "UTF-8");
    }
}
