/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "aimarkdownview.h"

#include <KLocalizedString>

#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>

namespace
{
class LocalOnlyInterceptor final : public QWebEngineUrlRequestInterceptor
{
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;

    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        if (info.requestUrl().scheme() != QLatin1String("qrc")) {
            info.block(true);
        }
    }
};
}

AiMarkdownPage::AiMarkdownPage(QWebEngineProfile *profile)
    : QWebEnginePage(profile, profile)
{
}

bool AiMarkdownPage::acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame)
{
    Q_UNUSED(type)
    Q_UNUSED(isMainFrame)

    if (url.scheme() == QLatin1String("okular-ai")) {
        const QString path = url.path();
        if (path.startsWith(QLatin1String("save/"))) {
            bool ok = false;
            const int index = path.mid(5).toInt(&ok);
            if (ok && index >= 0) {
                Q_EMIT saveRequested(index);
            }
        }
        return false;
    }
    if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) {
        QDesktopServices::openUrl(url);
        return false;
    }
    return url.scheme() == QLatin1String("qrc");
}

AiMarkdownView::AiMarkdownView(QWidget *parent)
    : QWebEngineView(parent)
{
    auto *profile = new QWebEngineProfile(this);
    profile->setUrlRequestInterceptor(new LocalOnlyInterceptor(profile));
    auto *markdownPage = new AiMarkdownPage(profile);
    setPage(markdownPage);
    connect(markdownPage, &AiMarkdownPage::saveRequested, this, &AiMarkdownView::saveRequested);
    connect(this, &QWebEngineView::loadFinished, this, [this](bool ok) {
        m_ready = ok;
        if (ok) {
            updatePage();
        }
    });
    load(QUrl(QStringLiteral("qrc:/okular/ai/renderer.html")));
}

AiMarkdownView::~AiMarkdownView()
{
    delete page();
}

void AiMarkdownView::setMessages(const QJsonArray &messages)
{
    m_messages = messages;
    updatePage();
}

void AiMarkdownView::updatePage()
{
    if (!m_ready) {
        return;
    }
    const QString json = QString::fromUtf8(QJsonDocument(m_messages).toJson(QJsonDocument::Compact));
    const QJsonObject labels{{QStringLiteral("image"), i18n("Image: ")},
                             {QStringLiteral("question"), i18n("Question")},
                             {QStringLiteral("answer"), i18n("AI answer")},
                             {QStringLiteral("save"), i18n("Save as annotation")},
                             {QStringLiteral("source"), i18n("View source")}};
    const QString translated = QString::fromUtf8(QJsonDocument(labels).toJson(QJsonDocument::Compact));
    page()->runJavaScript(QStringLiteral("setMessages(%1, %2)").arg(json, translated));
}
