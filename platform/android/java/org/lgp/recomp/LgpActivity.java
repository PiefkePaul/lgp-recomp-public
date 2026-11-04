// Activity of the Android apps (P7.3, docs/knowledge/android-ndk-build.md).
//
// SDL3 is linked statically into the program's shared library, so only that
// library is loaded; its name comes from the manifest (meta-data
// "lgp.library"). The app's folder for settings, saves, logs and the game
// dump is the app-specific external storage
// (/sdcard/Android/data/<package>/files), handed to the program as
// LGP_APP_DIR. Arguments: the intent's string extra "args" (split at
// spaces), e.g.
//   adb shell am start -n <package>/org.lgp.recomp.LgpActivity -e args "--frames 600"
// Without it the program starts without arguments (app mode of lgp_game).
// Either way lgp_game logs to <folder>/logs/lgp.log and resolves relative
// paths against that folder (LGP_BOOT_APP_LOG_ALWAYS).
// A pipeline cache seed in the APK's assets (LGP_APP_PIPELINE_SEED) is copied
// to <folder>/package/pipeline-seed.lgppipes, beside the (absent) packaged
// game folder <folder>/package/game (platform::packaged_game_dir), where the
// program takes over only the records its own cache lacks. The driver's
// pipeline cache of the same build (pipeline-seed.lgppipes.driver) goes beside
// it; the program starts from it while it has no driver cache of its own.
// Without game data the app mode asks for the dump's folder (GameImport).
package org.lgp.recomp;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.system.Os;
import android.util.Log;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

public class LgpActivity extends SDLActivity {
    private static final String TAG = "lgp";
    private static volatile Activity current;

    static Activity current() {
        return current;
    }

    // The program's own limit for a .lgppipes file.
    private static final long MAX_SEED_BYTES = 32L << 20;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        File dir = getExternalFilesDir(null);
        if (dir == null)
            dir = getFilesDir();
        try {
            Os.setenv("LGP_APP_DIR", dir.getAbsolutePath(), true);
            // Apps get no TMPDIR and there is no /tmp: the app's internal
            // cache (not the FUSE-backed external folder), e.g. for the
            // file-backed guest block (LGP_HOST_BLOCK_FILE=1, off by default).
            Os.setenv("TMPDIR", getCacheDir().getAbsolutePath(), false);
        } catch (Exception e) {
            Log.e(TAG, "setenv LGP_APP_DIR/TMPDIR: " + e);
        }
        copySeed(dir, "pipeline-seed.lgppipes");
        copySeed(dir, "pipeline-seed.lgppipes.driver");
        GameBundle.prepare(this, dir);  // game data from the APK's splits, if built with them
        current = this;
        super.onCreate(savedInstanceState);
    }

    @Override
    protected void onDestroy() {
        if (current == this)
            current = null;
        final boolean ended = mSDLMainFinished;
        super.onDestroy();
        // The program has returned (--frames N, quitting from the menu): SDL
        // only finishes the activity, Android keeps the process cached, and
        // with it the guest's host threads and memory. A new start in that
        // process is refused by SDL anyway (it calls System.exit in onCreate).
        // So end it here. A kill, not System.exit: exit() would run the
        // library's static destructors while host threads may still use them.
        // lgp.log is complete by now (stderr unbuffered, stdout line-buffered).
        if (ended) {
            Log.i(TAG, "program ended, ending the process");
            android.os.Process.killProcess(android.os.Process.myPid());
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == GameImport.REQUEST) {
            GameImport.onPicked(resultCode == RESULT_OK && data != null ? data.getData() : null);
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    // A pipeline cache seed of the APK (asset name) into the app folder: once,
    // and again after an update of the app (the seed may have grown). Never
    // touches the app's own cache (webgpu-pipelines.lgppipes[.driver]).
    private void copySeed(File dir, String name) {
        File pkg = new File(dir, "package");
        File out = new File(pkg, name);
        long installed = 0;
        try {
            installed = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
        } catch (PackageManager.NameNotFoundException e) {
            Log.e(TAG, "no package info: " + e);
        }
        if (out.isFile() && out.lastModified() >= installed)
            return;
        // On the UI thread before SDL starts the program, which reads the
        // file right away. The program refuses a .lgppipes above 32 MiB; the
        // same cap holds here for both seeds, so the copy stays short. A
        // 32 MiB copy out of the APK should take well under a second
        // (Verdacht, not measured; the ANR limit is 5 s).
        File tmp = new File(pkg, name + ".tmp");
        try (InputStream src = getAssets().open(name)) {
            if (!pkg.isDirectory() && !pkg.mkdirs())
                throw new IOException("cannot create " + pkg);
            try (OutputStream dst = new FileOutputStream(tmp)) {
                byte[] buf = new byte[1 << 16];
                long total = 0;
                for (int n; (n = src.read(buf)) > 0;) {
                    total += n;
                    if (total > MAX_SEED_BYTES)
                        throw new IOException("larger than " + MAX_SEED_BYTES + " bytes, not used");
                    dst.write(buf, 0, n);
                }
            }
            if (!tmp.renameTo(out))
                throw new IOException("cannot rename " + tmp + " to " + out);
            Log.i(TAG, "pipeline cache seed copied to " + out + " (" + out.length() + " bytes)");
        } catch (FileNotFoundException e) {
            // Built without LGP_APP_PIPELINE_SEED: a seed of an earlier APK goes.
            if (out.delete())
                Log.i(TAG, "pipeline cache seed of an earlier version removed");
        } catch (IOException e) {
            Log.e(TAG, "pipeline cache seed " + name + ": " + e);
            tmp.delete();
            out.delete();  // a copy of an earlier version would not match this one
        }
    }

    // Tells the native side before the surface goes (LgpSurface).
    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new LgpSurface(context);
    }

    @Override
    protected String[] getLibraries() {
        String lib = "main";
        try {
            ApplicationInfo info = getPackageManager().getApplicationInfo(getPackageName(), PackageManager.GET_META_DATA);
            if (info.metaData != null && info.metaData.getString("lgp.library") != null)
                lib = info.metaData.getString("lgp.library");
        } catch (PackageManager.NameNotFoundException e) {
            Log.e(TAG, "no application info: " + e);
        }
        return new String[] { lib };
    }

    @Override
    protected String[] getArguments() {
        String args = getIntent() != null ? getIntent().getStringExtra("args") : null;
        if (args == null || args.trim().isEmpty())
            return new String[0];
        return args.trim().split("\\s+");
    }
}
