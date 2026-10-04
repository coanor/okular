/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "mdxdictionary.h"

#include <QRegularExpression>
#include <QTextDocument>

#if HAVE_MDICT
#include <mdict_extern.h>

#include <QFile>
#include <QHash>
#include <QTemporaryFile>

#include <cstdlib>
#include <memory>
#include <mutex>

namespace
{
struct DictionaryHandle {
    QString path;
    void *handle = nullptr;
    std::unique_ptr<QTemporaryFile> resourceFile;
    std::mutex mutex;

    ~DictionaryHandle()
    {
        if (handle) {
            mdict_destroy(handle);
        }
    }
};

DictionaryHandle &dictionaryHandle()
{
    static DictionaryHandle dictionary;
    return dictionary;
}

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
        {QStringLiteral("went"), QStringLiteral("go")},
        {QStringLiteral("gone"), QStringLiteral("go")},
        {QStringLiteral("ate"), QStringLiteral("eat")},
        {QStringLiteral("eaten"), QStringLiteral("eat")},
        {QStringLiteral("saw"), QStringLiteral("see")},
        {QStringLiteral("seen"), QStringLiteral("see")},
        {QStringLiteral("mice"), QStringLiteral("mouse")},
        {QStringLiteral("teeth"), QStringLiteral("tooth")},
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

QString MdxDictionary::word(QString text)
{
    // Reject paragraphs before running regular expressions or trimming them.
    if (text.size() > 1024) {
        return {};
    }
    // Join words split by a line-ending hyphen, preserving ordinary hyphenated words.
    static const QRegularExpression lineHyphen(QStringLiteral(R"((?<=[\p{L}\p{N}])[-\x{00ad}]\h*\R\h*(?=[\p{L}\p{N}]))"));
    text.remove(lineHyphen);
    text = text.trimmed();
    while (!text.isEmpty() && !text.front().isLetterOrNumber()) {
        text.remove(0, 1);
    }
    while (!text.isEmpty() && !text.back().isLetterOrNumber()) {
        text.chop(1);
    }

    static const QRegularExpression wordPattern(QStringLiteral(R"(^[\p{L}\p{N}][\p{L}\p{N}\p{M}]*(?:['\x{2019}\x{2010}\x{2011}-][\p{L}\p{N}\p{M}]+)*$)"));
    return text.size() <= 128 && wordPattern.match(text).hasMatch() ? text : QString();
}

QString MdxDictionary::summary(QString definition)
{
    definition.truncate(16384);
    definition.replace(QRegularExpression(QStringLiteral("(?i)</?br\\s*/?>")), QStringLiteral("<br>"));
    definition.remove(QRegularExpression(QStringLiteral("`[0-9]+`")));
    QTextDocument document;
    document.setHtml(definition);
    const QStringList lines = document.toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QStringList summary;
    for (const QString &line : lines) {
        const QString clean = line.simplified();
        if (!clean.isEmpty()) {
            summary.append(clean);
        }
        if (summary.size() == 5) {
            break;
        }
    }
    return summary.join(QLatin1Char('\n')).left(400);
}

QString MdxDictionary::lookup(const QString &filePath, const QString &word)
{
#if HAVE_MDICT
    DictionaryHandle &dictionary = dictionaryHandle();
    std::lock_guard lock(dictionary.mutex);
    if (dictionary.path != filePath) {
        if (dictionary.handle) {
            mdict_destroy(dictionary.handle);
            dictionary.handle = nullptr;
        }
        dictionary.path = filePath;
        dictionary.resourceFile.reset();
        QString nativePath = filePath;
        if (filePath.startsWith(QLatin1String(":"))) {
            // mdict-cpp uses std::ifstream, so Qt resources need a native file.
            // Keep the extraction on the lookup worker.
            QFile resource(filePath);
            dictionary.resourceFile.reset(QTemporaryFile::createNativeFile(resource));
            if (!dictionary.resourceFile) {
                dictionary.path.clear();
                return {};
            }
            nativePath = dictionary.resourceFile->fileName();
        }
        const QByteArray path = QFile::encodeName(nativePath);
        dictionary.handle = const_cast<void *>(mdict_init(path.constData()).data);
#ifdef Q_OS_UNIX
        // The parser keeps its stream open. Unlink now to avoid leaving large
        // temporary files behind when Android terminates the application.
        dictionary.resourceFile.reset();
#endif
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

void MdxDictionary::invalidate(const QString &filePath)
{
#if HAVE_MDICT
    DictionaryHandle &dictionary = dictionaryHandle();
    std::lock_guard lock(dictionary.mutex);
    if (dictionary.path == filePath) {
        if (dictionary.handle) {
            mdict_destroy(dictionary.handle);
            dictionary.handle = nullptr;
        }
        dictionary.path.clear();
        dictionary.resourceFile.reset();
    }
#else
    Q_UNUSED(filePath)
#endif
}
