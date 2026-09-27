/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "mdxdictionary.h"

#if HAVE_MDICT
#include <mdict_extern.h>

#include <QFile>
#include <QHash>

#include <cstdlib>
#include <mutex>

namespace
{
struct DictionaryHandle {
    QString path;
    void *handle = nullptr;
    std::mutex mutex;

    ~DictionaryHandle()
    {
        if (handle) {
            mdict_destroy(handle);
        }
    }
};

QString lookupExact(void *handle, const QString &word)
{
    QString keyWord = word;
    for (int redirect = 0; redirect < 4; ++redirect) {
        const QByteArray key = keyWord.toUtf8();
        const SizedData result = mdict_lookup(handle, key.constData());
        if (!result.data) {
            return {};
        }
        QString definition = QString::fromUtf8(static_cast<const char *>(result.data), result.size).remove(QChar::Null);
        std::free(const_cast<void *>(result.data));
        if (!definition.startsWith(QLatin1String("@@@LINK="))) {
            return definition;
        }
        keyWord = definition.mid(8).trimmed();
    }
    return {};
}

QStringList fallbackWords(const QString &word)
{
    const QString lower = word.toLower();
    static const QHash<QString, QString> irregular {
        {QStringLiteral("went"), QStringLiteral("go")},      {QStringLiteral("gone"), QStringLiteral("go")},
        {QStringLiteral("ate"), QStringLiteral("eat")},      {QStringLiteral("eaten"), QStringLiteral("eat")},
        {QStringLiteral("saw"), QStringLiteral("see")},      {QStringLiteral("seen"), QStringLiteral("see")},
        {QStringLiteral("mice"), QStringLiteral("mouse")},  {QStringLiteral("teeth"), QStringLiteral("tooth")},
        {QStringLiteral("children"), QStringLiteral("child")},
    };
    QStringList candidates;
    if (irregular.contains(lower)) {
        candidates << irregular.value(lower);
    }
    if (lower.endsWith(QLatin1String("ies")) && lower.size() > 4) {
        candidates << lower.left(lower.size() - 3) + QLatin1Char('y');
    }
    if (lower.endsWith(QLatin1String("ing")) && lower.size() > 5) {
        const QString stem = lower.left(lower.size() - 3);
        candidates << stem << stem + QLatin1Char('e');
        if (stem.size() > 2 && stem.back() == stem.at(stem.size() - 2)) {
            candidates << stem.left(stem.size() - 1);
        }
    }
    if (lower.endsWith(QLatin1String("ed")) && lower.size() > 4) {
        const QString stem = lower.left(lower.size() - 2);
        candidates << stem << stem + QLatin1Char('e');
    }
    if (lower.endsWith(QLatin1Char('s')) && lower.size() > 3) {
        candidates << lower.left(lower.size() - 1);
    }
    candidates.removeDuplicates();
    return candidates;
}
}
#endif

QString MdxDictionary::lookup(const QString &filePath, const QString &word)
{
#if HAVE_MDICT
    static DictionaryHandle dictionary;
    std::lock_guard lock(dictionary.mutex);
    if (dictionary.path != filePath) {
        if (dictionary.handle) {
            mdict_destroy(dictionary.handle);
            dictionary.handle = nullptr;
        }
        dictionary.path = filePath;
        const QByteArray path = QFile::encodeName(filePath);
        dictionary.handle = const_cast<void *>(mdict_init(path.constData()).data);
    }
    if (!dictionary.handle) {
        return {};
    }
    QString definition = lookupExact(dictionary.handle, word);
    if (!definition.isEmpty()) {
        return definition;
    }
    for (const QString &candidate : fallbackWords(word)) {
        definition = lookupExact(dictionary.handle, candidate);
        if (!definition.isEmpty()) {
            return definition;
        }
    }
#else
    Q_UNUSED(filePath)
    Q_UNUSED(word)
#endif
    return {};
}
