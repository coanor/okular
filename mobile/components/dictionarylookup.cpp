/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "dictionarylookup.h"

#include "part/mdxdictionary.h"
#include "settings.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtConcurrent>

#include <KLocalizedString>

namespace
{
struct ImportResult {
    QString path;
    QString error;
};

ImportResult importDictionary(const QUrl &source)
{
    const QString sourcePath = source.isLocalFile() ? source.toLocalFile() : source.toString(QUrl::FullyEncoded);
    QFile input(sourcePath);
    if (!input.open(QIODevice::ReadOnly)) {
        return {{}, input.errorString()};
    }

    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/dictionaries");
    if (!QDir().mkpath(directory)) {
        return {{}, QStringLiteral("Could not create the dictionary directory")};
    }
    const QString targetPath = directory + QStringLiteral("/imported.mdx");
    QSaveFile output(targetPath);
    if (!output.open(QIODevice::WriteOnly)) {
        return {{}, output.errorString()};
    }

    qint64 bytesCopied = 0;
    for (;;) {
        const QByteArray chunk = input.read(1024 * 1024);
        if (chunk.isEmpty()) {
            if (input.error() != QFileDevice::NoError) {
                return {{}, input.errorString()};
            }
            break;
        }
        if (output.write(chunk) != chunk.size()) {
            return {{}, output.errorString()};
        }
        bytesCopied += chunk.size();
    }
    if (bytesCopied == 0) {
        return {{}, QStringLiteral("The selected dictionary is empty")};
    }
    if (!output.commit()) {
        return {{}, output.errorString()};
    }
    MdxDictionary::invalidate(targetPath);
    return {targetPath, {}};
}
}

DictionaryLookup::DictionaryLookup(QObject *parent)
    : QObject(parent)
{
    Okular::Settings::instance(QStringLiteral("okularproviderrc"));
}

bool DictionaryLookup::autoLookupEnabled() const
{
    return Okular::Settings::autoLookupSelectedWords();
}

void DictionaryLookup::setAutoLookupEnabled(bool enabled)
{
    if (enabled == autoLookupEnabled()) {
        return;
    }
    Okular::Settings::setAutoLookupSelectedWords(enabled);
    Okular::Settings::self()->save();
    if (!enabled) {
        clear();
    }
    Q_EMIT autoLookupEnabledChanged();
}

QString DictionaryLookup::dictionaryFile() const
{
    return Okular::Settings::dictionaryFile();
}

void DictionaryLookup::setDictionaryFile(const QString &file)
{
    const QString path = file.trimmed();
    if (path == dictionaryFile()) {
        return;
    }
    Okular::Settings::setDictionaryFile(path);
    Okular::Settings::self()->save();
    clear();
    Q_EMIT dictionaryFileChanged();
}

QString DictionaryLookup::word() const
{
    return m_word;
}

QString DictionaryLookup::definition() const
{
    return m_definition;
}

QString DictionaryLookup::error() const
{
    return m_error;
}

bool DictionaryLookup::loading() const
{
    return m_loading;
}

bool DictionaryLookup::importing() const
{
    return m_importing;
}

QString DictionaryLookup::importError() const
{
    return m_importError;
}

bool DictionaryLookup::mdxAvailable() const
{
#if HAVE_MDICT
    return true;
#else
    return false;
#endif
}

void DictionaryLookup::clear()
{
    ++m_request;
    m_word.clear();
    m_lookupFile.clear();
    m_definition.clear();
    m_error.clear();
    m_loading = false;
    Q_EMIT resultChanged();
}

void DictionaryLookup::retry(const QString &selectedText)
{
    clear();
    lookup(selectedText);
}

void DictionaryLookup::lookup(const QString &selectedText)
{
    const QString selectedWord = MdxDictionary::word(selectedText);
    if (selectedWord.isEmpty()) {
        clear();
        return;
    }

    const QString path = dictionaryFile().trimmed();
    if (selectedWord == m_word && path == m_lookupFile) {
        return;
    }
    clear();
    m_word = selectedWord;
    m_lookupFile = path;
    Q_EMIT resultChanged();

    if (path.isEmpty()) {
        QUrl url;
        url.setScheme(QStringLiteral("eudic"));
#ifdef Q_OS_ANDROID
        url.setHost(QStringLiteral("peek"));
#else
        url.setHost(QStringLiteral("dict"));
#endif
        url.setPath(QLatin1Char('/') + selectedWord);
        if (!QDesktopServices::openUrl(url)) {
            m_error = i18n("Could not open Eudic. Install it or select an MDX dictionary in settings.");
            Q_EMIT resultChanged();
        }
        return;
    }

    const QFileInfo dictionary(path);
    if (!dictionary.isFile() || dictionary.suffix().compare(QLatin1String("mdx"), Qt::CaseInsensitive) != 0) {
        m_error = i18n("Select a valid MDX dictionary file in settings.");
        Q_EMIT resultChanged();
        return;
    }

#if HAVE_MDICT
    const int request = m_request;
    m_loading = true;
    Q_EMIT resultChanged();
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, request] {
        const QString result = watcher->result();
        watcher->deleteLater();
        if (request != m_request) {
            return;
        }
        m_loading = false;
        if (result.isEmpty()) {
            m_error = i18n("No definition found in the selected MDX dictionary.");
        } else {
            m_definition = MdxDictionary::summary(result);
        }
        Q_EMIT resultChanged();
    });
    watcher->setFuture(QtConcurrent::run([path, selectedWord] {
        return MdxDictionary::lookup(path, selectedWord);
    }));
#else
    m_error = i18n("This build does not include MDX support. Install mdict-cpp and rebuild Okular.");
    Q_EMIT resultChanged();
#endif
}

void DictionaryLookup::importFile(const QUrl &source)
{
    if (m_importing || !source.isValid()) {
        return;
    }
    m_importing = true;
    m_importError.clear();
    Q_EMIT importChanged();
    auto *watcher = new QFutureWatcher<ImportResult>(this);
    connect(watcher, &QFutureWatcher<ImportResult>::finished, this, [this, watcher] {
        const ImportResult result = watcher->result();
        watcher->deleteLater();
        m_importing = false;
        if (result.path.isEmpty()) {
            m_importError = i18n("Could not import dictionary: %1", result.error);
        } else {
            const bool samePath = dictionaryFile() == result.path;
            setDictionaryFile(result.path);
            if (samePath) {
                clear();
                Q_EMIT dictionaryFileChanged();
            }
        }
        Q_EMIT importChanged();
    });
    watcher->setFuture(QtConcurrent::run([source] {
        return importDictionary(source);
    }));
}
