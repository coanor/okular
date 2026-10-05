/* SPDX-License-Identifier: GPL-2.0-or-later */
package org.kde.okular;

import android.content.Context;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;

import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.security.cert.X509Certificate;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;

public final class CloudPlatform {
    private static final String ALIAS = "org.kde.okular.s3";
    private static final String PREFERENCES = "okular-cloud";

    private CloudPlatform() {}

    private static SecretKey key(boolean create) throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        if (!store.containsAlias(ALIAS)) {
            if (!create) {
                return null;
            }
            KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
            generator.init(new KeyGenParameterSpec.Builder(ALIAS,
                    KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                    .setKeySize(256)
                    .build());
            return generator.generateKey();
        }
        return (SecretKey) store.getKey(ALIAS, null);
    }

    public static String load(Context context) {
        SharedPreferences settings = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE);
        String data = settings.getString("data", "");
        if (data.isEmpty()) {
            return "";
        }
        try {
            SecretKey key = key(false);
            if (key == null) {
                return null;
            }
            byte[] iv = Base64.decode(settings.getString("iv", ""), Base64.NO_WRAP);
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(128, iv));
            return new String(cipher.doFinal(Base64.decode(data, Base64.NO_WRAP)), StandardCharsets.UTF_8);
        } catch (Exception error) {
            // A restored backup or invalidated key requires entering credentials again.
            return null;
        }
    }

    public static boolean save(Context context, String payload) {
        SharedPreferences settings = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE);
        try {
            if (payload.isEmpty()) {
                if (!settings.edit().clear().commit()) {
                    return false;
                }
                KeyStore store = KeyStore.getInstance("AndroidKeyStore");
                store.load(null);
                store.deleteEntry(ALIAS);
                return true;
            }
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.ENCRYPT_MODE, key(true));
            byte[] data = cipher.doFinal(payload.getBytes(StandardCharsets.UTF_8));
            return settings.edit()
                    .putString("data", Base64.encodeToString(data, Base64.NO_WRAP))
                    .putString("iv", Base64.encodeToString(cipher.getIV(), Base64.NO_WRAP))
                    .commit();
        } catch (Exception error) {
            return false;
        }
    }

    public static String documentName(Context context, String url) {
        try (Cursor cursor = context.getContentResolver().query(Uri.parse(url),
                new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            return cursor != null && cursor.moveToFirst() ? cursor.getString(0) : "";
        } catch (Exception error) {
            return "";
        }
    }

    // libcurl needs a PEM bundle; Android does not expose a Unix CA bundle path.
    public static String systemCertificates() {
        try {
            TrustManagerFactory factory = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
            factory.init((KeyStore) null);
            StringBuilder certificates = new StringBuilder();
            for (TrustManager manager : factory.getTrustManagers()) {
                if (manager instanceof X509TrustManager) {
                    for (X509Certificate certificate : ((X509TrustManager) manager).getAcceptedIssuers()) {
                        certificates.append("-----BEGIN CERTIFICATE-----\n")
                                .append(Base64.encodeToString(certificate.getEncoded(), Base64.NO_WRAP))
                                .append("\n-----END CERTIFICATE-----\n");
                    }
                }
            }
            return certificates.toString();
        } catch (Exception error) {
            return "";
        }
    }
}
