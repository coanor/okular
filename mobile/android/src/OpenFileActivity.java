package org.kde.something;

import android.content.ContentResolver;
import android.content.Intent;
import android.util.Log;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.net.Uri;
import android.app.Activity;

import java.io.FileNotFoundException;

import org.qtproject.qt.android.bindings.QtActivity;

class FileClass
{
    public static native void openUri(String uri);
}

public class OpenFileActivity extends QtActivity
{

    public String contentUrlToFd(String url)
    {
        if (Uri.parse(url).getScheme().equals("fd")) {
            return url;
        }

        try {
            ContentResolver resolver = getBaseContext().getContentResolver();
            ParcelFileDescriptor fdObject = resolver.openFileDescriptor(Uri.parse(url), "r");
            return "fd:///" + fdObject.detachFd();
        } catch (FileNotFoundException e) {
            Log.e("Okular", "Cannot find file", e);
        }
        return "";
    }


    private void displayUri(Uri uri)
    {
        if (uri == null)
            return;

        // DocumentItem opens the provider URI and retains it for reopening after sync.
        Log.e("Okular", "opening url: " + uri.toString());
        FileClass.openUri(uri.toString());
    }

    public void handleViewIntent() {
        final Intent bundleIntent = getIntent();
        if (bundleIntent == null)
            return;

        final String action = bundleIntent.getAction();
        Log.v("Okular", "Starting action: " + action);
        if (action == "android.intent.action.VIEW") {
            displayUri(bundleIntent.getData());
        }
    }
}
