/* SPDX-License-Identifier: GPL-2.0-or-later */
package org.kde.something;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;

// Keep the loopback callback reachable while the sign-in browser is in front.
// This service holds no authorization URLs or credentials.
public class ChatGptSignInService extends Service
{
    private static final String CHANNEL = "chatgpt_sign_in";
    private final Handler timer = new Handler(Looper.getMainLooper());
    private final Runnable timeout = () -> stopSelf();

    public static boolean start(Context context)
    {
        try {
            Intent intent = new Intent(context, ChatGptSignInService.class);
            if (Build.VERSION.SDK_INT >= 26)
                context.startForegroundService(intent);
            else
                context.startService(intent);
            return true;
        } catch (RuntimeException e) {
            return false;
        }
    }

    public static void stop(Context context)
    {
        context.stopService(new Intent(context, ChatGptSignInService.class));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId)
    {
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationManager manager = getSystemService(NotificationManager.class);
            manager.createNotificationChannel(new NotificationChannel(CHANNEL,
                getString(org.kde.okular.kirigami.R.string.chatgpt_sign_in), NotificationManager.IMPORTANCE_LOW));
        }
        Intent returnIntent = new Intent(this, OpenFileActivity.class);
        returnIntent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent returnToApp = PendingIntent.getActivity(this, 0, returnIntent,
            PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Notification.Builder notification = Build.VERSION.SDK_INT >= 26
            ? new Notification.Builder(this, CHANNEL) : new Notification.Builder(this);
        notification.setSmallIcon(android.R.drawable.ic_lock_idle_lock)
            .setContentTitle(getString(org.kde.okular.kirigami.R.string.chatgpt_sign_in))
            .setContentText(getString(org.kde.okular.kirigami.R.string.chatgpt_sign_in_waiting))
            .setContentIntent(returnToApp)
            .setOngoing(true);
        startForeground(1, notification.build());
        // Bound the service even if native cleanup cannot run. The native flow
        // allows ten minutes for login, followed by bounded network requests.
        timer.removeCallbacks(timeout);
        timer.postDelayed(timeout, 15 * 60 * 1000);
        return START_NOT_STICKY;
    }

    @Override
    public void onTimeout(int startId, int foregroundServiceType)
    {
        stopSelf();
    }

    @Override
    public void onDestroy()
    {
        timer.removeCallbacks(timeout);
        stopForeground(true);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent)
    {
        return null;
    }
}
