/* -*- Mode: Java; c-basic-offset: 4; tab-width: 20; indent-tabs-mode: nil; -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.gecko.delegates;

import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Resources;
import android.graphics.Color;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.support.design.widget.Snackbar;
import android.support.v4.content.ContextCompat;
import android.util.Log;
import android.view.View;
import android.widget.ListView;

import org.json.JSONException;
import org.json.JSONObject;
import org.mozilla.gecko.AboutPages;
import org.mozilla.gecko.BrowserApp;
import org.mozilla.gecko.EditBookmarkDialog;
import org.mozilla.gecko.GeckoAppShell;
import org.mozilla.gecko.GeckoSharedPrefs;
import org.mozilla.gecko.R;
import org.mozilla.gecko.SnackbarHelper;
import org.mozilla.gecko.Tab;
import org.mozilla.gecko.Tabs;
import org.mozilla.gecko.Telemetry;
import org.mozilla.gecko.TelemetryContract;
import org.mozilla.gecko.home.HomeConfig;
import org.mozilla.gecko.promotion.SimpleHelperUI;
import org.mozilla.gecko.prompts.Prompt;
import org.mozilla.gecko.prompts.PromptListItem;
import org.mozilla.gecko.util.DrawableUtil;
import org.mozilla.gecko.util.ThreadUtils;

import java.lang.ref.WeakReference;

/**
 * Delegate to watch for bookmark state changes.
 *
 * This is responsible for showing snackbars and helper UIs related to the addition/removal
 * of bookmarks, or reader view bookmarks.
 */
public class BookmarkStateChangeDelegate extends BrowserAppDelegateWithReference implements Tabs.OnTabsChangedListener {
    private static final String LOGTAG = "BookmarkDelegate";

    @Override
    public void onResume(BrowserApp browserApp) {
        Tabs.registerOnTabsChangedListener(this);
    }

    @Override
    public void onPause(BrowserApp browserApp) {
        Tabs.unregisterOnTabsChangedListener(this);
    }

    @Override
    public void onTabChanged(Tab tab, Tabs.TabEvents msg, String data) {
        switch (msg) {
            case BOOKMARK_ADDED:
                // We always show the special offline snackbar whenever we bookmark a reader page.
                // It's possible that the page is already stored offline, however this is highly
                // unlikely, and even so it is probably nicer to show the same offline notification
                // every time we bookmark an about:reader page.
                if (!AboutPages.isAboutReader(tab.getURL())) {
                    showBookmarkAddedSnackbar();
                } else {
                    if (!promoteReaderViewBookmarkAdded()) {
                        showReaderModeBookmarkAddedSnackbar();
                    }
                }
                break;

            case BOOKMARK_REMOVED:
                showBookmarkRemovedSnackbar();
                break;
        }
    }

    @Override
    public void onActivityResult(BrowserApp browserApp, int requestCode, int resultCode, Intent data) {
        if (requestCode == BrowserApp.ACTIVITY_REQUEST_FIRST_READERVIEW_BOOKMARK) {
            if (resultCode == BrowserApp.ACTIVITY_RESULT_FIRST_READERVIEW_BOOKMARKS_GOTO_BOOKMARKS) {
                browserApp.openUrlAndStopEditing("about:home?panel=" + HomeConfig.getIdForBuiltinPanelType(HomeConfig.PanelType.BOOKMARKS));
            } else if (resultCode == BrowserApp.ACTIVITY_RESULT_FIRST_READERVIEW_BOOKMARKS_IGNORE) {
                showReaderModeBookmarkAddedSnackbar();
            }
        }
    }

    private boolean promoteReaderViewBookmarkAdded() {
        final BrowserApp browserApp = getBrowserApp();
        if (browserApp == null) {
            return false;
        }

        final SharedPreferences prefs = GeckoSharedPrefs.forProfile(browserApp);

        final boolean hasFirstReaderViewPromptBeenShownBefore = prefs.getBoolean(SimpleHelperUI.PREF_FIRST_RVBP_SHOWN, false);

        if (hasFirstReaderViewPromptBeenShownBefore) {
            return false;
        }

        SimpleHelperUI.show(browserApp,
                SimpleHelperUI.FIRST_RVBP_SHOWN_TELEMETRYEXTRA,
                BrowserApp.ACTIVITY_REQUEST_FIRST_READERVIEW_BOOKMARK,
                R.string.helper_first_offline_bookmark_title, R.string.helper_first_offline_bookmark_message,
                R.drawable.helper_readerview_bookmark, R.string.helper_first_offline_bookmark_button,
                BrowserApp.ACTIVITY_RESULT_FIRST_READERVIEW_BOOKMARKS_GOTO_BOOKMARKS,
                BrowserApp.ACTIVITY_RESULT_FIRST_READERVIEW_BOOKMARKS_IGNORE);

        GeckoSharedPrefs.forProfile(browserApp)
                .edit()
                .putBoolean(SimpleHelperUI.PREF_FIRST_RVBP_SHOWN, true)
                .apply();

        return true;
    }

    private void showBookmarkAddedSnackbar() {
        final BrowserApp browserApp = getBrowserApp();
        if (browserApp == null) {
            return;
        }

        // This flow is from the option menu which has check to see if a bookmark was already added.
        // So, it is safe here to show the snackbar that bookmark_added without any checks.
        final SnackbarHelper.SnackbarCallback callback = new SnackbarHelper.SnackbarCallback() {
            @Override
            public void onClick(View v) {
                Telemetry.sendUIEvent(TelemetryContract.Event.SHOW, TelemetryContract.Method.TOAST, "bookmark_options");
                showBookmarkDialog(browserApp);
            }
        };

        SnackbarHelper.showSnackbarWithAction(browserApp,
                browserApp.getResources().getString(R.string.bookmark_added),
                Snackbar.LENGTH_LONG,
                browserApp.getResources().getString(R.string.bookmark_options),
                callback);
    }

    private void showBookmarkRemovedSnackbar() {
        final BrowserApp browserApp = getBrowserApp();
        if (browserApp == null) {
            return;
        }

        SnackbarHelper.showSnackbar(browserApp, browserApp.getResources().getString(R.string.bookmark_removed), Snackbar.LENGTH_LONG);
    }

    private static void showBookmarkDialog(final BrowserApp browserApp) {
        final Resources res = browserApp.getResources();
        final Tab tab = Tabs.getInstance().getSelectedTab();

        final Prompt ps = new Prompt(browserApp, new Prompt.PromptCallback() {
            @Override
            public void onPromptFinished(String result) {
                int itemId = -1;
                try {
                    itemId = new JSONObject(result).getInt("button");
                } catch (JSONException ex) {
                    Log.e(LOGTAG, "Exception reading bookmark prompt result", ex);
                }

                if (tab == null) {
                    return;
                }

                if (itemId == 0) {
                    final String extrasId = res.getResourceEntryName(R.string.contextmenu_edit_bookmark);
                    Telemetry.sendUIEvent(TelemetryContract.Event.ACTION,
                            TelemetryContract.Method.DIALOG, extrasId);

                    new EditBookmarkDialog(browserApp).show(tab.getURL());
                } else if (itemId == 1) {
                    final String extrasId = res.getResourceEntryName(R.string.contextmenu_add_to_launcher);
                    Telemetry.sendUIEvent(TelemetryContract.Event.ACTION,
                            TelemetryContract.Method.DIALOG, extrasId);

                    final String url = tab.getURL();
                    final String title = tab.getDisplayTitle();

                    if (url != null && title != null) {
                        ThreadUtils.postToBackgroundThread(new Runnable() {
                            @Override
                            public void run() {
                                GeckoAppShell.createShortcut(title, url);
                            }
                        });
                    }
                }
            }
        });

        final PromptListItem[] items = new PromptListItem[2];
        items[0] = new PromptListItem(res.getString(R.string.contextmenu_edit_bookmark));
        items[1] = new PromptListItem(res.getString(R.string.contextmenu_add_to_launcher));

        ps.show("", "", items, ListView.CHOICE_MODE_NONE);
    }

    private void showReaderModeBookmarkAddedSnackbar() {
        final BrowserApp browserApp = getBrowserApp();
        if (browserApp == null) {
            return;
        }

        final Drawable iconDownloaded = DrawableUtil.tintDrawable(browserApp, R.drawable.status_icon_readercache, Color.WHITE);

        final SnackbarHelper.SnackbarCallback callback = new SnackbarHelper.SnackbarCallback() {
            @Override
            public void onClick(View v) {
                browserApp.openUrlAndStopEditing("about:home?panel=" + HomeConfig.getIdForBuiltinPanelType(HomeConfig.PanelType.BOOKMARKS));
            }
        };

        SnackbarHelper.showSnackbarWithActionAndColors(browserApp,
                browserApp.getResources().getString(R.string.reader_saved_offline),
                Snackbar.LENGTH_LONG,
                browserApp.getResources().getString(R.string.reader_switch_to_bookmarks),
                callback,
                iconDownloaded,
                ContextCompat.getColor(browserApp, R.color.link_blue),
                Color.WHITE);
    }
}
