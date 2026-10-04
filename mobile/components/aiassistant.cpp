/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "aiassistant.h"

#include "core/generator.h"
#include "core/page.h"
#include "documentitem.h"
#include "gui/pagepainter.h"

#include <KLocalizedString>
#include <QBuffer>
#include <QDesktopServices>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QPainter>
#include <QUuid>
#include <QtConcurrentRun>
#include <utility>

AiAssistant::AiAssistant(DocumentItem *document)
    : QObject(document)
    , m_document(document)
    , m_chatGpt(this)
{
    m_document->document()->addObserver(this);
    for (const AiProfile &profile : AiStore::loadProfiles(0)) {
        if (profile.kind >= AiProfile::Kind::OpenAiChat && profile.kind <= AiProfile::Kind::Anthropic) {
            m_profiles.append(profile);
        }
    }
    m_currentProfile = m_profiles.isEmpty() ? -1 : 0;
    m_imageTimer.setSingleShot(true);
    m_imageTimer.setInterval(20000);
    connect(&m_imageTimer, &QTimer::timeout, this, [this] {
        cancel();
        setStatus(i18n("The page image could not be rendered in time."));
    });
    connect(&m_provider, &AiProvider::completed, this, [this](const QString &answer, const QString &sessionId) {
        if (!m_submitted) {
            return;
        }
        m_submitted = false;
        m_streamedAnswer.clear();
        m_conversation.sessionId = sessionId;
        m_conversation.messages.append(AiMessage {QStringLiteral("assistant"), answer, m_pendingMessage.page, {}, {}, {}});
        persistConversation();
        Q_EMIT conversationChanged();
        Q_EMIT busyChanged();
    });
    connect(&m_provider, &AiProvider::failed, this, [this](const QString &error) {
        rollbackQuestion();
        setStatus(error);
        Q_EMIT busyChanged();
    });
    connect(&m_provider, &AiProvider::stopped, this, [this] {
        rollbackQuestion();
        Q_EMIT busyChanged();
    });
    connect(&m_provider, &AiProvider::answerUpdated, this, [this](const QString &answer) {
        m_streamedAnswer = answer;
        Q_EMIT conversationChanged();
    });
    connect(&m_chatGpt, &ChatGptConnection::authorizationRequested, this, [this](const QUrl &url) {
        if (!QDesktopServices::openUrl(url)) {
            m_chatGpt.cancel();
            setStatus(i18n("Could not open your browser for ChatGPT sign-in."));
        }
    });
}

AiAssistant::~AiAssistant()
{
    cancel();
    m_document->document()->removeObserver(this);
}

QVariantList AiAssistant::profiles() const
{
    QVariantList result;
    for (const AiProfile &profile : m_profiles) {
        result.append(QVariantMap {{QStringLiteral("name"), profile.name}});
    }
    return result;
}

int AiAssistant::currentProfile() const
{
    return m_currentProfile;
}

void AiAssistant::setCurrentProfile(int index)
{
    if (busy() || index < -1 || index >= m_profiles.size() || index == m_currentProfile) {
        return;
    }
    m_currentProfile = index;
    loadConversation();
    Q_EMIT currentProfileChanged();
}

QVariantList AiAssistant::messages() const
{
    QVariantList result;
    for (const AiMessage &message : m_conversation.messages) {
        result.append(QVariantMap {{QStringLiteral("role"), message.role}, {QStringLiteral("content"), message.content}, {QStringLiteral("page"), message.page + 1}});
    }
    if (!m_streamedAnswer.isEmpty()) {
        result.append(QVariantMap {{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("content"), m_streamedAnswer}, {QStringLiteral("page"), m_pendingMessage.page + 1}});
    }
    return result;
}

QString AiAssistant::question() const
{
    return m_question;
}

void AiAssistant::setQuestion(const QString &question)
{
    if (m_question != question) {
        m_question = question;
        Q_EMIT questionChanged();
    }
}

QString AiAssistant::selection() const
{
    return m_selection;
}

QString AiAssistant::status() const
{
    return m_status;
}

void AiAssistant::setStatus(const QString &status)
{
    m_status = status;
    Q_EMIT statusChanged();
}

bool AiAssistant::busy() const
{
    return m_hashing || m_encoding || m_authPending || m_pendingPage >= 0 || m_provider.isBusy();
}

bool AiAssistant::ready() const
{
    return !m_documentKey.isEmpty();
}

QObject *AiAssistant::chatGpt()
{
    return &m_chatGpt;
}

void AiAssistant::activate()
{
    if (ready() || m_hashing || !m_document->isOpened()) {
        return;
    }
    m_hashing = true;
    Q_EMIT busyChanged();
    const int generation = m_documentGeneration;
    const QUrl url = m_document->aiDocumentUrl();
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, generation] {
        const QString key = watcher->result();
        watcher->deleteLater();
        if (generation != m_documentGeneration) {
            return;
        }
        m_hashing = false;
        m_documentKey = key;
        loadConversation();
        Q_EMIT readyChanged();
        Q_EMIT busyChanged();
    });
    watcher->setFuture(QtConcurrent::run([url] { return AiStore::documentKey(url); }));
}

QVariantMap AiAssistant::profile(int index) const
{
    if (index < 0 || index >= m_profiles.size()) {
        return {};
    }
    const AiProfile &profile = m_profiles[index];
    return {{QStringLiteral("name"), profile.name},
            {QStringLiteral("kind"), static_cast<int>(profile.kind)},
            {QStringLiteral("endpoint"), profile.endpoint},
            {QStringLiteral("model"), profile.model},
            {QStringLiteral("extraArguments"), profile.extraArguments},
            {QStringLiteral("vision"), profile.vision},
            {QStringLiteral("chatGptAccountId"), profile.chatGptAccountId},
            {QStringLiteral("hasApiKey"), !profile.apiKey.isEmpty()}};
}

bool AiAssistant::saveProfile(int index, const QVariantMap &fields)
{
    if (busy() || index < -1 || index >= m_profiles.size()) {
        return false;
    }
    AiProfile profile = index < 0 ? AiProfile {} : m_profiles[index];
    if (!profile.chatGptAccountId.isEmpty()) {
        setStatus(i18n("Edit this model in ChatGPT settings."));
        return false;
    }
    const int kind = fields.value(QStringLiteral("kind")).toInt();
    profile.name = fields.value(QStringLiteral("name")).toString().trimmed();
    profile.endpoint = fields.value(QStringLiteral("endpoint")).toString().trimmed();
    profile.model = fields.value(QStringLiteral("model")).toString().trimmed();
    profile.extraArguments = fields.value(QStringLiteral("extraArguments")).toString().trimmed();
    profile.vision = fields.value(QStringLiteral("vision")).toBool();
    if (kind < 0 || kind > static_cast<int>(AiProfile::Kind::Anthropic) || profile.name.isEmpty() || profile.model.isEmpty()) {
        setStatus(i18n("Enter a profile name, protocol and model name."));
        return false;
    }
    profile.kind = static_cast<AiProfile::Kind>(kind);
    const QUrl endpoint(profile.endpoint);
    if (!profile.endpoint.isEmpty() && (!endpoint.isValid() || endpoint.host().isEmpty() || (endpoint.scheme() != QLatin1String("https") && endpoint.scheme() != QLatin1String("http")))) {
        setStatus(i18n("Enter a valid HTTP or HTTPS base URL."));
        return false;
    }
    if (!profile.extraArguments.isEmpty() && !QJsonDocument::fromJson(profile.extraArguments.toUtf8()).isObject()) {
        setStatus(i18n("Extra arguments must be a JSON object."));
        return false;
    }
    const QString key = fields.value(QStringLiteral("apiKey")).toString().trimmed();
    if (!key.isEmpty()) {
        profile.apiKey = key;
    }
    return storeProfile(index, profile);
}

bool AiAssistant::saveChatGptProfile(int index, const QVariantMap &fields)
{
    if (busy() || m_chatGpt.busy() || index < -1 || index >= m_profiles.size()) {
        return false;
    }
    const QString accountId = fields.value(QStringLiteral("accountId")).toString();
    const QString model = fields.value(QStringLiteral("model")).toString();
    bool validModel = false;
    for (const QVariant &value : m_chatGpt.models()) {
        validModel = validModel || value.toMap().value(QStringLiteral("id")).toString() == model;
    }
    if (!m_chatGpt.isConnected(accountId) || accountId != m_chatGpt.modelAccountId() || !validModel) {
        setStatus(i18n("Connect your ChatGPT account and choose an available model first."));
        return false;
    }
    AiProfile profile = index < 0 ? AiProfile {} : m_profiles[index];
    // Changing accounts starts a separate local history.
    if (profile.chatGptAccountId != accountId) {
        profile.id.clear();
    }
    profile.chatGptAccountId = accountId;
    profile.name = fields.value(QStringLiteral("name")).toString().trimmed();
    if (profile.name.isEmpty()) {
        profile.name = QStringLiteral("ChatGPT · ") + model;
    }
    profile.kind = AiProfile::Kind::OpenAiResponses;
    profile.endpoint = QStringLiteral("https://api.openai.com/v1");
    profile.model = model;
    profile.vision = fields.value(QStringLiteral("vision")).toBool();
    profile.apiKey.clear();
    profile.extraArguments.clear();
    return storeProfile(index, profile);
}

bool AiAssistant::storeProfile(int index, AiProfile profile)
{
    if (profile.id.isEmpty()) {
        profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (index < 0) {
        index = m_profiles.size();
        m_profiles.append(profile);
    } else {
        m_profiles[index] = profile;
    }
    QString error;
    AiStore::saveProfiles(m_profiles, 0, &error);
    m_currentProfile = index;
    loadConversation();
    setStatus(error);
    Q_EMIT profilesChanged();
    Q_EMIT currentProfileChanged();
    return true;
}

void AiAssistant::removeProfile(int index)
{
    if (busy() || index < 0 || index >= m_profiles.size()) {
        return;
    }
    m_profiles.removeAt(index);
    QString error;
    AiStore::saveProfiles(m_profiles, 0, &error);
    m_currentProfile = m_profiles.isEmpty() ? -1 : qMin(index, static_cast<int>(m_profiles.size()) - 1);
    loadConversation();
    setStatus(error);
    Q_EMIT profilesChanged();
    Q_EMIT currentProfileChanged();
}

void AiAssistant::setSelection(const QString &text, int page)
{
    if (busy()) {
        return;
    }
    m_selection = text;
    m_selectionPage = text.isEmpty() ? -1 : page;
    Q_EMIT selectionChanged();
}

void AiAssistant::resetDocument()
{
    cancel();
    ++m_documentGeneration;
    m_hashing = false;
    m_documentKey.clear();
    m_conversation = {};
    m_selection.clear();
    m_selectionPage = -1;
    setQuestion({});
    setStatus({});
    Q_EMIT selectionChanged();
    Q_EMIT conversationChanged();
    Q_EMIT readyChanged();
    Q_EMIT busyChanged();
}

void AiAssistant::loadConversation()
{
    m_conversation = m_currentProfile >= 0 ? AiStore::loadConversation(m_documentKey, m_profiles[m_currentProfile].id) : AiConversation {};
    Q_EMIT conversationChanged();
}

void AiAssistant::persistConversation()
{
    if (m_currentProfile >= 0) {
        setStatus(AiStore::saveConversation(m_documentKey, m_profiles[m_currentProfile].id, m_conversation) ? QString() : i18n("Could not save the local conversation."));
    }
}

void AiAssistant::ask()
{
    if (busy() || !ready() || !m_document->isOpened() || m_currentProfile < 0) {
        return;
    }
    if (m_question.trimmed().isEmpty()) {
        setStatus(i18n("Type a question first."));
        return;
    }
    if (!m_document->document()->isAllowed(Okular::AllowCopy)) {
        setStatus(i18n("This document does not allow copying page content."));
        return;
    }
    const AiProfile &profile = m_profiles[m_currentProfile];
    if (!profile.chatGptAccountId.isEmpty() && (!m_chatGpt.isConnected(profile.chatGptAccountId) || m_chatGpt.busy())) {
        setStatus(i18n("Connect this ChatGPT account in Models before asking a question."));
        return;
    }
    if (profile.apiKey.isEmpty() && profile.chatGptAccountId.isEmpty()) {
        setStatus(i18n("Edit this model and enter its API key."));
        return;
    }
    const int page = m_selectionPage >= 0 ? m_selectionPage : m_document->currentPage();
    const Okular::Page *documentPage = m_document->document()->page(page);
    if (!documentPage) {
        setStatus(i18n("The current page is unavailable."));
        return;
    }
    if (!documentPage->hasTextPage()) {
        m_document->document()->requestTextPage(page);
    }
    m_pendingMessage = AiMessage {QStringLiteral("user"), m_question.trimmed(), page, documentPage->text(nullptr).left(30000), m_selection, {}};
    if (!profile.vision) {
        if (m_selection.isEmpty() && m_pendingMessage.pageText.isEmpty()) {
            setStatus(i18n("Select text or switch to a model that accepts page images."));
            return;
        }
        submitQuestion({});
        return;
    }
    const double scale = 1600.0 / qMax(documentPage->width(), documentPage->height());
    m_imageSize = QSize(qMax(1, qRound(documentPage->width() * scale)), qMax(1, qRound(documentPage->height() * scale)));
    m_pendingPage = page;
    setStatus(i18n("Rendering page %1…", page + 1));
    Q_EMIT busyChanged();
    if (documentPage->hasPixmap(this, m_imageSize.width(), m_imageSize.height())) {
        pageImageReady();
    } else {
        m_imageTimer.start();
        m_document->document()->requestPixmaps({new Okular::PixmapRequest(this, page, m_imageSize.width(), m_imageSize.height(), 1.0, 1, Okular::PixmapRequest::Asynchronous)}, Okular::Document::NoOption);
    }
}

void AiAssistant::notifyPageChanged(int page, int flags)
{
    if (page == m_pendingPage && (flags & Okular::DocumentObserver::Pixmap) && m_document->document()->page(page)->hasPixmap(this, m_imageSize.width(), m_imageSize.height())) {
        pageImageReady();
    }
}

void AiAssistant::pageImageReady()
{
    m_imageTimer.stop();
    const Okular::Page *page = m_document->document()->page(m_pendingPage);
    QImage image(m_imageSize, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    PagePainter::paintPageOnPainter(&painter, page, this, 0, m_imageSize.width(), m_imageSize.height(), QRect(QPoint(), m_imageSize));
    painter.end();
    m_pendingPage = -1;
    // JPEG encoding runs away from the UI thread. A canceled or closed document
    // must never submit the result of an earlier render.
    const int generation = m_requestGeneration;
    m_encoding = true;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, generation] {
        const QString encoded = watcher->result();
        watcher->deleteLater();
        if (generation != m_requestGeneration || !m_encoding) {
            return;
        }
        m_encoding = false;
        if (encoded.isEmpty()) {
            setStatus(i18n("Could not encode the page image."));
            Q_EMIT busyChanged();
            return;
        }
        submitQuestion(encoded);
    });
    watcher->setFuture(QtConcurrent::run([image] {
        QByteArray encoded;
        QBuffer buffer(&encoded);
        buffer.open(QIODevice::WriteOnly);
        return image.save(&buffer, "JPEG", 85) ? QString::fromLatin1(encoded.toBase64()) : QString();
    }));
}

void AiAssistant::submitQuestion(const QString &image)
{
    m_beforeRequest = m_conversation;
    m_submitted = true;
    for (AiMessage &message : m_conversation.messages) {
        message.pageImage.clear();
    }
    m_pendingMessage.pageImage = image;
    m_conversation.messages.append(m_pendingMessage);
    persistConversation();
    setQuestion({});
    Q_EMIT conversationChanged();
    const AiProfile &profile = m_profiles[m_currentProfile];
    if (!profile.chatGptAccountId.isEmpty()) {
        m_authPending = true;
        setStatus(i18n("Connecting to ChatGPT…"));
        m_chatGpt.requestAccessToken(
            profile.chatGptAccountId,
            [this](const QString &token) {
                if (!m_authPending || !m_submitted || m_currentProfile < 0) {
                    return;
                }
                m_authPending = false;
                AiProfile authenticated = m_profiles[m_currentProfile];
                // OAuth credentials must never follow an editable API base URL.
                authenticated.endpoint = QStringLiteral("https://api.openai.com/v1");
                authenticated.apiKey = token;
                m_provider.send(authenticated, m_conversation, m_pendingMessage);
                Q_EMIT busyChanged();
            },
            [this](const QString &error) {
                if (m_authPending) {
                    m_authPending = false;
                    rollbackQuestion();
                    setStatus(error);
                    Q_EMIT busyChanged();
                }
            });
    } else {
        m_provider.send(profile, m_conversation, m_pendingMessage);
    }
    Q_EMIT busyChanged();
}

void AiAssistant::rollbackQuestion()
{
    if (!m_submitted) {
        return;
    }
    m_submitted = false;
    m_streamedAnswer.clear();
    m_conversation = m_beforeRequest;
    setQuestion(m_pendingMessage.content);
    persistConversation();
    Q_EMIT conversationChanged();
}

void AiAssistant::cancel()
{
    m_imageTimer.stop();
    m_pendingPage = -1;
    ++m_requestGeneration;
    m_encoding = false;
    if (m_authPending) {
        m_chatGpt.cancel();
        m_authPending = false;
    }
    m_provider.cancel();
    rollbackQuestion();
    setStatus(i18n("Request canceled."));
    Q_EMIT busyChanged();
}

void AiAssistant::clearConversation()
{
    if (busy() || !ready() || m_currentProfile < 0) {
        return;
    }
    if (!AiStore::clearConversation(m_documentKey, m_profiles[m_currentProfile].id)) {
        setStatus(i18n("Could not remove the local conversation record."));
        return;
    }
    m_conversation = {};
    setStatus({});
    Q_EMIT conversationChanged();
}

#include "moc_aiassistant.cpp"
