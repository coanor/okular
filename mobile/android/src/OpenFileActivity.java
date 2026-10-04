package org.kde.something;

import android.content.ContentResolver;
import android.content.Intent;
import android.util.Log;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.net.Uri;
import android.app.Activity;

import java.io.FileNotFoundException;
import java.util.concurrent.ConcurrentHashMap;

import org.qtproject.qt.android.bindings.QtActivity;

class FileClass
{
    public static native void openUri(String uri);
}

public class OpenFileActivity extends QtActivity
{
    private final ConcurrentHashMap<String, String> sourceUrls = new ConcurrentHashMap<>();

    private String descriptorUrl(ParcelFileDescriptor file, Uri source) throws FileNotFoundException
    {
        if (file == null)
            throw new FileNotFoundException("Document provider returned no descriptor");
        String url = "fd:///" + file.detachFd();
        sourceUrls.put(url, source.toString());
        return url;
    }

    public String takeSourceUrl(String descriptorUrl)
    {
        String source = sourceUrls.remove(descriptorUrl);
        return source == null ? descriptorUrl : source;
    }

    public String contentUrlToFd(String url)
    {
        if (url.isEmpty())
            return "";
        Uri source = Uri.parse(url);
        if ("fd".equals(source.getScheme())) {
            return url;
        }

        try {
            ContentResolver resolver = getBaseContext().getContentResolver();
            ParcelFileDescriptor fdObject = resolver.openFileDescriptor(source, "r");
            return descriptorUrl(fdObject, source);
        } catch (FileNotFoundException e) {
            Log.e("Okular", "Cannot find file", e);
        }
        return "";
    }


    private void displayUri(Uri uri)
    {
        if (uri == null)
            return;

        if (!uri.getScheme().equals("file")) {
            try {
                ContentResolver resolver = getBaseContext().getContentResolver();
                ParcelFileDescriptor fdObject = resolver.openFileDescriptor(uri, "r");
                uri = Uri.parse(descriptorUrl(fdObject, uri));
            } catch (Exception e) {
                e.printStackTrace();

                //TODO: emit warning that couldn't be opened
                Log.e("Okular", "failed to open");
                return;
            }
        }

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
