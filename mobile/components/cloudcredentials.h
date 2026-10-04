/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QString>

// Persistent credentials are only supported with a platform-backed key store.
namespace CloudCredentials
{
bool available();
bool load(QString *payload, QString *error);
bool save(const QString &payload, QString *error);
}
