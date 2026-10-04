/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QJsonObject>

namespace ChatGptToken
{
// Returns claims only after signature, identity and lifetime checks succeed.
QJsonObject validate(const QString &token, const QJsonObject &jwks, const QString &issuer, const QString &clientId, const QString &nonce, const QString &subject);
}
