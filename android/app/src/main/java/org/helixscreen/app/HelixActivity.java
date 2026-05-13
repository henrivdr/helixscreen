package org.helixscreen.app;

import android.graphics.Color;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Scanner;

import org.helixscreen.app.BuildConfig;
import org.libsdl.app.SDLActivity;

/**
 * HelixScreen Android activity.
 * Extends SDLActivity to provide the SDL2 + native code bridge.
 *
 * System UI behavior:
 *   - Default state: status bar AND navigation bar hidden in immersive mode.
 *   - API 30+: Uses WindowInsetsController with transient gesture bars.
 *     System edge-swipe reveals translucent bars that auto-fade. Our explicit
 *     show/hide via the custom swipe gesture uses the 3-second auto-hide timer.
 *   - API 28-29: Uses legacy setSystemUiVisibility with non-sticky IMMERSIVE
 *     flags so the hidden state persists across taps.
 *   - Reveal gesture: a swipe that starts within EDGE_ZONE_DP of the bottom
 *     edge and travels upward more than SWIPE_THRESHOLD_DP shows the nav bar.
 *     LVGL treats the drag as a scroll gesture and cancels any pending click,
 *     so bottom-row app buttons stay tappable without accidental reveals.
 *   - Always-visible mode: setNavBarAlwaysVisible(true) (called from native
 *     when the user toggles the setting) pins the nav bar onscreen and
 *     disables both the swipe reveal and the auto-hide timer. Status bar
 *     stays hidden either way.
 *   - Touch events are never consumed here; SDL / LVGL sees them all.
 */
public class HelixActivity extends SDLActivity {

    /** A swipe must start within this many dp of the bottom edge to arm. */
    private static final int EDGE_ZONE_DP = 32;

    /** Vertical travel required to trigger the reveal, in dp. */
    private static final int SWIPE_THRESHOLD_DP = 16;

    /** Nav bar hides this many ms after the last touch while it is visible. */
    private static final long NAV_HIDE_TIMEOUT_MS = 3000;

    private boolean mNavBarVisible = false;
    private boolean mSwipeArmed = false;
    private float mSwipeStartY = 0f;

    /**
     * User-controlled "keep navbar onscreen" preference (issue #908).
     * Set via the static JNI bridge {@link #setNavBarAlwaysVisible(boolean)}
     * from the C++ DisplaySettingsManager. Static so the value survives
     * activity recreation and can be pushed before onResume.
     */
    private static volatile boolean sNavBarAlwaysVisible = false;

    private final Handler mHideHandler = new Handler(Looper.getMainLooper());
    private final Runnable mHideRunnable = new Runnable() {
        @Override
        public void run() {
            setNavBarVisible(false);
        }
    };
    private final Runnable mApplySystemUiRunnable = new Runnable() {
        @Override
        public void run() {
            applySystemUi();
        }
    };

    @Override
    protected String[] getLibraries() {
        return new String[]{
            "SDL2",
            "main"
        };
    }

    @Override
    protected String[] getArguments() {
        java.util.List<String> args = new java.util.ArrayList<>();
        if (BuildConfig.DEBUG) {
            args.add("-vv");
        }
        // Launch with: adb shell am start -n org.helixscreen.app/.HelixActivity --ez test true
        if (getIntent() != null && getIntent().getBooleanExtra("test", false)) {
            args.add("--test");
        }
        return args.toArray(new String[0]);
    }

    // =========================================================================
    // System UI management
    // =========================================================================

    private void applySystemUi() {
        Window window = getWindow();
        if (Build.VERSION.SDK_INT >= 30) {
            applySystemUiModern(window);
        } else {
            applySystemUiLegacy(window);
        }
    }

    /**
     * API 30+: WindowInsetsController replaces the deprecated
     * setSystemUiVisibility flags with explicit show/hide calls and a
     * behavior enum that controls how gestures interact with hidden bars.
     */
    private void applySystemUiModern(Window window) {
        if (Build.VERSION.SDK_INT < 30) return;

        WindowInsetsController controller = window.getInsetsController();
        if (controller == null) return;

        // Edge-to-edge: app draws behind system bars (replaces LAYOUT_* flags)
        window.setDecorFitsSystemWindows(false);

        // Bars stay visible until explicitly hidden (matches legacy non-sticky
        // IMMERSIVE). Required for 3-button nav where users need time to tap
        // back/home/recents. Our 3-second auto-hide timer re-hides them.
        controller.setSystemBarsBehavior(
                WindowInsetsController.BEHAVIOR_DEFAULT);

        if (sNavBarAlwaysVisible) {
            // Pin nav bar, keep status bar hidden.
            controller.hide(WindowInsets.Type.statusBars());
            controller.show(WindowInsets.Type.navigationBars());
        } else if (mNavBarVisible) {
            controller.show(WindowInsets.Type.navigationBars());
        } else {
            controller.hide(WindowInsets.Type.systemBars());
        }

        // Transparent dark bars — disable Android's default contrast scrim
        // (API 29+ adds a white scrim behind the nav bar when enforced)
        window.setNavigationBarColor(Color.TRANSPARENT);
        window.setStatusBarColor(Color.TRANSPARENT);
        window.setNavigationBarContrastEnforced(false);
        window.setStatusBarContrastEnforced(false);
    }

    /**
     * API 28-29: legacy setSystemUiVisibility path.
     * Uses non-sticky IMMERSIVE so HIDE_NAVIGATION persists across taps.
     */
    @SuppressWarnings("deprecation")
    private void applySystemUiLegacy(Window window) {
        int flags = View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                  | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                  | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                  | View.SYSTEM_UI_FLAG_FULLSCREEN;

        if (!mNavBarVisible && !sNavBarAlwaysVisible) {
            flags |= View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                   | View.SYSTEM_UI_FLAG_IMMERSIVE;
        }
        window.getDecorView().setSystemUiVisibility(flags);

        window.addFlags(WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
        window.setNavigationBarColor(Color.TRANSPARENT);
        window.setStatusBarColor(Color.TRANSPARENT);
    }

    private void scheduleAutoHide() {
        mHideHandler.removeCallbacks(mHideRunnable);
        mHideHandler.postDelayed(mHideRunnable, NAV_HIDE_TIMEOUT_MS);
    }

    private void setNavBarVisible(boolean visible) {
        if (mNavBarVisible == visible) {
            return;
        }
        mNavBarVisible = visible;
        applySystemUi();
        // Auto-hide is meaningless when the user has pinned the bar.
        if (visible && !sNavBarAlwaysVisible) {
            scheduleAutoHide();
        } else {
            mHideHandler.removeCallbacks(mHideRunnable);
        }
    }

    // =========================================================================
    // Touch handling — custom bottom-edge swipe to reveal nav bar
    // =========================================================================

    @Override
    public boolean dispatchTouchEvent(MotionEvent ev) {
        final float density = getResources().getDisplayMetrics().density;
        final int edgeZonePx = (int) (EDGE_ZONE_DP * density + 0.5f);
        final int swipeThresholdPx = (int) (SWIPE_THRESHOLD_DP * density + 0.5f);
        final int decorHeight = getWindow().getDecorView().getHeight();

        switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                if (!mNavBarVisible
                        && !sNavBarAlwaysVisible
                        && decorHeight > 0
                        && ev.getY() >= (decorHeight - edgeZonePx)) {
                    mSwipeArmed = true;
                    mSwipeStartY = ev.getY();
                } else {
                    mSwipeArmed = false;
                }
                if (mNavBarVisible && !sNavBarAlwaysVisible) {
                    scheduleAutoHide();
                }
                break;
            case MotionEvent.ACTION_MOVE:
                if (mSwipeArmed && (mSwipeStartY - ev.getY()) >= swipeThresholdPx) {
                    setNavBarVisible(true);
                    mSwipeArmed = false;
                    // Cancel the touch so LVGL drops any pending click on
                    // bottom-row buttons that overlap the swipe edge zone.
                    ev.setAction(MotionEvent.ACTION_CANCEL);
                    super.dispatchTouchEvent(ev);
                    return true;
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                mSwipeArmed = false;
                break;
        }
        return super.dispatchTouchEvent(ev);
    }

    // =========================================================================
    // Lifecycle — reapply system UI after focus/resume
    // =========================================================================

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applySystemUi();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        // SDL's COMMAND_CHANGE_WINDOW_STYLE handler applies immersive-sticky
        // flags asynchronously on the main thread during startup.  Post our
        // reapply to run after those messages drain so we win the race.
        View decor = getWindow().getDecorView();
        decor.removeCallbacks(mApplySystemUiRunnable);
        decor.post(mApplySystemUiRunnable);
    }

    @Override
    protected void onPause() {
        mHideHandler.removeCallbacks(mHideRunnable);
        getWindow().getDecorView().removeCallbacks(mApplySystemUiRunnable);
        super.onPause();
    }

    /**
     * Legacy visibility change listener for API 28-29.
     * On API 30+ the WindowInsetsController manages bar state directly, so
     * this callback fires but we rely on our own mNavBarVisible tracking.
     */
    @SuppressWarnings("deprecation")
    @Override
    public void onSystemUiVisibilityChange(int visibility) {
        if (Build.VERSION.SDK_INT >= 30) {
            // WindowInsetsController handles everything; ignore legacy callback
            return;
        }
        boolean navVisibleNow = (visibility & View.SYSTEM_UI_FLAG_HIDE_NAVIGATION) == 0;
        if (navVisibleNow == mNavBarVisible) {
            return;
        }
        mNavBarVisible = navVisibleNow;
        if (navVisibleNow) {
            scheduleAutoHide();
        } else {
            mHideHandler.removeCallbacks(mHideRunnable);
        }
    }

    // =========================================================================
    // JNI theme bridge — called from native theme_manager
    // =========================================================================

    /**
     * Toggle the "keep navigation bar onscreen" preference (issue #908).
     * Called from native code via JNI when the user flips the setting and
     * once at startup with the persisted value. Status bar stays hidden
     * either way — this affects only the navigation bar.
     *
     * @param alwaysVisible  true to pin nav bar onscreen, false to restore
     *                       the default immersive + swipe-to-reveal behavior
     */
    public static void setNavBarAlwaysVisible(final boolean alwaysVisible) {
        // Update the static flag unconditionally so onResume/onWindowFocusChanged
        // pick it up even if the activity is being created right now.
        sNavBarAlwaysVisible = alwaysVisible;

        final android.content.Context ctx = SDLActivity.getContext();
        if (!(ctx instanceof HelixActivity)) return;
        final HelixActivity helix = (HelixActivity) ctx;
        helix.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (alwaysVisible) {
                    helix.mHideHandler.removeCallbacks(helix.mHideRunnable);
                    helix.mNavBarVisible = true;
                } else {
                    // Drop back to immersive on next applySystemUi.
                    helix.mNavBarVisible = false;
                }
                helix.applySystemUi();
            }
        });
    }

    /**
     * Set the window background color to match the active theme.
     * Called from native code via JNI whenever the theme changes so the
     * area behind transparent system bars matches the app's screen_bg.
     *
     * @param argb  0xAARRGGBB color value (alpha typically 0xFF)
     */
    public static void setWindowBackgroundColor(final int argb) {
        final SDLActivity activity = (SDLActivity) SDLActivity.getContext();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                activity.getWindow().getDecorView()
                        .setBackgroundColor(argb);
            }
        });
    }

    // =========================================================================
    // JNI HTTPS bridge — called from native crash reporter
    // =========================================================================

    /**
     * Perform an HTTP(S) GET using Android's built-in TLS stack.
     * Called from native code via JNI when libhv lacks SSL support.
     *
     * @param url         Full URL (http:// or https://)
     * @param userAgent   User-Agent header value
     * @param accept      Accept header value (may be empty)
     * @param timeoutSec  Connection + read timeout in seconds
     * @return            "STATUS_CODE\nRESPONSE_BODY" on success,
     *                    "0\nERROR_MESSAGE" on failure
     */
    public static String httpsGet(String url, String userAgent, String accept, int timeoutSec) {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(url).openConnection();
            conn.setRequestMethod("GET");
            conn.setConnectTimeout(timeoutSec * 1000);
            conn.setReadTimeout(timeoutSec * 1000);
            conn.setRequestProperty("User-Agent", userAgent);
            if (accept != null && !accept.isEmpty()) {
                conn.setRequestProperty("Accept", accept);
            }

            int status = conn.getResponseCode();
            String responseBody = "";
            try (Scanner s = new Scanner(
                    status >= 200 && status < 400
                        ? conn.getInputStream() : conn.getErrorStream(),
                    "UTF-8")) {
                s.useDelimiter("\\A");
                if (s.hasNext()) responseBody = s.next();
            }
            return status + "\n" + responseBody;
        } catch (Exception e) {
            Log.w("HelixHTTPS", "GET failed: " + e.getMessage());
            return "0\n" + e.getMessage();
        } finally {
            if (conn != null) conn.disconnect();
        }
    }

    /**
     * Perform an HTTPS POST using Android's built-in TLS stack.
     * Called from native code via JNI when libhv lacks SSL support.
     *
     * @param url         Full HTTPS URL
     * @param body        JSON request body
     * @param userAgent   User-Agent header value
     * @param apiKey      X-API-Key header value
     * @param timeoutSec  Connection + read timeout in seconds
     * @return            "STATUS_CODE\nRESPONSE_BODY" on success,
     *                    "0\nERROR_MESSAGE" on failure
     */
    public static String httpsPost(String url, String body, String userAgent,
                                   String apiKey, int timeoutSec) {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(url).openConnection();
            conn.setRequestMethod("POST");
            conn.setDoOutput(true);
            conn.setConnectTimeout(timeoutSec * 1000);
            conn.setReadTimeout(timeoutSec * 1000);
            conn.setRequestProperty("Content-Type", "application/json");
            conn.setRequestProperty("User-Agent", userAgent);
            conn.setRequestProperty("X-API-Key", apiKey);

            byte[] payload = body.getBytes(StandardCharsets.UTF_8);
            conn.setFixedLengthStreamingMode(payload.length);

            try (OutputStream os = conn.getOutputStream()) {
                os.write(payload);
            }

            int status = conn.getResponseCode();
            String responseBody = "";
            try (Scanner s = new Scanner(
                    status >= 200 && status < 400
                        ? conn.getInputStream() : conn.getErrorStream(),
                    "UTF-8")) {
                s.useDelimiter("\\A");
                if (s.hasNext()) responseBody = s.next();
            }
            return status + "\n" + responseBody;
        } catch (Exception e) {
            Log.w("HelixHTTPS", "POST failed: " + e.getMessage());
            return "0\n" + e.getMessage();
        } finally {
            if (conn != null) conn.disconnect();
        }
    }
}
