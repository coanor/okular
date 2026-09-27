/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "aireadingassistant.h"

#include "aimarkdownview.h"
#include "gui/pagepainter.h"
#include "core/document.h"
#include "core/generator.h"
#include "core/page.h"

#include <KLocalizedString>

#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>
#include <QUuid>
#include <QUrl>
#include <QVBoxLayout>
#include <utility>

namespace
{
bool editProfile(AiProfile &profile, QWidget *parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(profile.id.isEmpty() ? i18n("Add AI model") : i18n("Edit AI model"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *name = new QLineEdit(profile.name, &dialog);
    auto *kind = new QComboBox(&dialog);
    kind->addItem(i18n("OpenAI-compatible Chat Completions"), static_cast<int>(AiProfile::Kind::OpenAiChat));
    kind->addItem(i18n("OpenAI Responses with server conversation"), static_cast<int>(AiProfile::Kind::OpenAiResponses));
    kind->addItem(i18n("Anthropic-compatible Messages"), static_cast<int>(AiProfile::Kind::Anthropic));
    kind->addItem(i18n("Local Codex CLI"), static_cast<int>(AiProfile::Kind::Codex));
    kind->setCurrentIndex(static_cast<int>(profile.kind));
    auto *endpoint = new QLineEdit(profile.endpoint, &dialog);
    endpoint->setPlaceholderText(i18n("Base URL, for example https://api.openai.com/v1"));
    auto *model = new QLineEdit(profile.model, &dialog);
    model->setPlaceholderText(i18n("Model name; leave empty for Codex default"));
    auto *key = new QLineEdit(&dialog);
    key->setEchoMode(QLineEdit::Password);
    key->setPlaceholderText(profile.apiKey.isEmpty() ? i18n("API key") : i18n("Leave blank to keep the current key"));
    auto *vision = new QCheckBox(i18n("This model accepts page images"), &dialog);
    vision->setChecked(profile.vision);
    form->addRow(i18n("Name:"), name);
    form->addRow(i18n("Protocol:"), kind);
    form->addRow(i18n("Base URL:"), endpoint);
    form->addRow(i18n("Model:"), model);
    form->addRow(i18n("API key:"), key);
    form->addRow(QString(), vision);
    const auto updateFields = [kind, endpoint, key] {
        const bool codex = kind->currentData().toInt() == static_cast<int>(AiProfile::Kind::Codex);
        endpoint->setEnabled(!codex);
        key->setEnabled(!codex);
    };
    QObject::connect(kind, &QComboBox::currentIndexChanged, &dialog, updateFields);
    updateFields();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    if (name->text().trimmed().isEmpty()) {
        QMessageBox::warning(parent, i18n("AI model"), i18n("Give this model a name."));
        return false;
    }
    if (profile.id.isEmpty()) {
        profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    profile.name = name->text().trimmed();
    profile.kind = static_cast<AiProfile::Kind>(kind->currentData().toInt());
    profile.endpoint = endpoint->text().trimmed();
    profile.model = model->text().trimmed();
    profile.vision = vision->isChecked();
    if (!key->text().isEmpty()) {
        profile.apiKey = key->text();
    }
    return true;
}
}

AiReadingAssistant::AiReadingAssistant(Okular::Document *document, QWidget *parent)
    : QWidget(parent)
    , m_document(document)
    , m_provider(this)
{
    m_profiles = AiStore::loadProfiles(winId());
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    auto *top = new QHBoxLayout;
    m_profileCombo = new QComboBox(this);
    m_profileCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    top->addWidget(m_profileCombo);
    auto *profilesButton = new QPushButton(i18n("Models…"), this);
    top->addWidget(profilesButton);
    layout->addLayout(top);
    for (const AiProfile &profile : std::as_const(m_profiles)) {
        m_profileCombo->addItem(profile.name, profile.id);
    }
    connect(profilesButton, &QPushButton::clicked, this, &AiReadingAssistant::editProfiles);
    connect(m_profileCombo, &QComboBox::currentIndexChanged, this, &AiReadingAssistant::loadSelectedConversation);

    auto *newConversation = new QPushButton(i18n("Start new conversation"), this);
    layout->addWidget(newConversation);
    connect(newConversation, &QPushButton::clicked, this, &AiReadingAssistant::clearConversation);

    m_view = new AiMarkdownView(this);
    layout->addWidget(m_view, 1);
    connect(m_view, &AiMarkdownView::saveRequested, this, &AiReadingAssistant::saveMessage);

    m_selectionLabel = new QLabel(this);
    m_selectionLabel->setWordWrap(true);
    m_selectionLabel->hide();
    layout->addWidget(m_selectionLabel);
    m_prompt = new QTextEdit(this);
    m_prompt->setPlaceholderText(i18n("Ask about the current page, for example: Explain equation 1.2"));
    m_prompt->setMaximumHeight(100);
    layout->addWidget(m_prompt);
    auto *actions = new QHBoxLayout;
    m_send = new QPushButton(i18n("Ask"), this);
    m_cancel = new QPushButton(i18n("Cancel"), this);
    actions->addWidget(m_send);
    actions->addWidget(m_cancel);
    layout->addLayout(actions);
    connect(m_send, &QPushButton::clicked, this, &AiReadingAssistant::sendQuestion);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        m_provider.cancel();
        m_imageTimer->stop();
        m_pendingPage = -1;
        if (m_questionSubmitted) {
            m_conversation = m_beforeRequest;
            m_questionSubmitted = false;
            if (AiProfile *profile = currentProfile()) {
                AiStore::saveConversation(m_documentKey, profile->id, m_conversation);
            }
            renderConversation();
        }
        showStatus(i18n("Request canceled."));
        updateControls();
    });
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_imageTimer = new QTimer(this);
    m_imageTimer->setSingleShot(true);
    connect(m_imageTimer, &QTimer::timeout, this, [this] {
        m_pendingPage = -1;
        showStatus(i18n("Could not render the page image in time."));
        updateControls();
    });

    connect(&m_provider, &AiProvider::completed, this, [this](const QString &answer, const QString &sessionId) {
        m_questionSubmitted = false;
        m_conversation.sessionId = sessionId;
        m_conversation.messages.append(AiMessage{QStringLiteral("assistant"), answer, -1, {}, {}, {}});
        bool saved = false;
        if (AiProfile *profile = currentProfile()) {
            saved = AiStore::saveConversation(m_documentKey, profile->id, m_conversation);
        }
        m_prompt->clear();
        m_selection.clear();
        m_selectionLabel->hide();
        renderConversation();
        showStatus(saved ? QString() : i18n("Could not save this AI conversation locally."));
        updateControls();
    });
    connect(&m_provider, &AiProvider::failed, this, [this](const QString &error) {
        if (m_questionSubmitted) {
            m_conversation = m_beforeRequest;
            m_questionSubmitted = false;
            if (AiProfile *profile = currentProfile()) {
                AiStore::saveConversation(m_documentKey, profile->id, m_conversation);
            }
        }
        renderConversation();
        showStatus(error);
        updateControls();
    });
    connect(&m_provider, &AiProvider::stopped, this, &AiReadingAssistant::updateControls);

    m_document->addObserver(this);
    updateControls();
}

AiReadingAssistant::~AiReadingAssistant()
{
    m_provider.cancel();
    m_document->removeObserver(this);
}

void AiReadingAssistant::setDocumentUrl(const QUrl &url)
{
    m_provider.cancel();
    m_pendingPage = -1;
    m_questionSubmitted = false;
    m_documentKey = AiStore::documentKey(url);
    m_selection.clear();
    m_selectionLabel->hide();
    loadSelectedConversation();
}

void AiReadingAssistant::askAboutSelection(const QString &text)
{
    m_selection = text;
    m_selectionLabel->setText(i18n("Selected: %1", text.left(250)));
    m_selectionLabel->show();
    m_prompt->setFocus();
}

void AiReadingAssistant::showStatus(const QString &text)
{
    m_status->setText(text);
}

void AiReadingAssistant::notifyPageChanged(int page, int flags)
{
    if (page == m_pendingPage && (flags & Okular::DocumentObserver::Pixmap)) {
        const Okular::Page *documentPage = m_document->page(page);
        if (documentPage && documentPage->hasPixmap(this, m_imageWidth, m_imageHeight)) {
            pageImageReady();
        }
    }
}

void AiReadingAssistant::notifySetup(const QList<Okular::Page *> &, int setupFlags)
{
    if (setupFlags & Okular::DocumentObserver::DocumentChanged) {
        m_imageTimer->stop();
        m_pendingPage = -1;
    }
}

AiProfile *AiReadingAssistant::currentProfile()
{
    const QString id = m_profileCombo->currentData().toString();
    for (AiProfile &profile : m_profiles) {
        if (profile.id == id) {
            return &profile;
        }
    }
    return nullptr;
}

void AiReadingAssistant::editProfiles()
{
    if (m_provider.isBusy() || m_pendingPage >= 0) {
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(i18n("AI models"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *list = new QListWidget(&dialog);
    for (const AiProfile &profile : std::as_const(m_profiles)) {
        list->addItem(profile.name);
    }
    layout->addWidget(list);
    auto *row = new QHBoxLayout;
    auto *add = new QPushButton(i18n("Add…"), &dialog);
    auto *edit = new QPushButton(i18n("Edit…"), &dialog);
    auto *remove = new QPushButton(i18n("Remove"), &dialog);
    row->addWidget(add);
    row->addWidget(edit);
    row->addWidget(remove);
    layout->addLayout(row);
    connect(add, &QPushButton::clicked, &dialog, [this, list, &dialog] {
        AiProfile profile;
        if (editProfile(profile, &dialog)) {
            m_profiles.append(profile);
            list->addItem(profile.name);
        }
    });
    connect(edit, &QPushButton::clicked, &dialog, [this, list, &dialog] {
        const int index = list->currentRow();
        if (index >= 0 && editProfile(m_profiles[index], &dialog)) {
            list->item(index)->setText(m_profiles[index].name);
        }
    });
    connect(remove, &QPushButton::clicked, &dialog, [this, list] {
        const int index = list->currentRow();
        if (index >= 0) {
            m_profiles.removeAt(index);
            delete list->takeItem(index);
        }
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    dialog.exec();
    QString error;
    AiStore::saveProfiles(m_profiles, winId(), &error);
    const QString selected = m_profileCombo->currentData().toString();
    m_profileCombo->clear();
    for (const AiProfile &profile : std::as_const(m_profiles)) {
        m_profileCombo->addItem(profile.name, profile.id);
    }
    const int selectedIndex = m_profileCombo->findData(selected);
    if (selectedIndex >= 0) {
        m_profileCombo->setCurrentIndex(selectedIndex);
    }
    if (!error.isEmpty()) {
        showStatus(error);
    }
    loadSelectedConversation();
}

void AiReadingAssistant::loadSelectedConversation()
{
    m_provider.cancel();
    m_pendingPage = -1;
    m_questionSubmitted = false;
    if (AiProfile *profile = currentProfile()) {
        m_conversation = AiStore::loadConversation(m_documentKey, profile->id);
    } else {
        m_conversation = {};
    }
    renderConversation();
    updateControls();
}

void AiReadingAssistant::sendQuestion()
{
    AiProfile *profile = currentProfile();
    if (!profile || !m_document->isOpened() || m_documentKey.isEmpty()) {
        showStatus(i18n("Open a document and configure an AI model first."));
        return;
    }
    const QString question = m_prompt->toPlainText().trimmed();
    if (question.isEmpty()) {
        showStatus(i18n("Type a question first."));
        return;
    }
    if (!m_document->isAllowed(Okular::AllowCopy)) {
        showStatus(i18n("This document does not allow copying page content."));
        return;
    }
    if (!profile->vision && m_selection.isEmpty()) {
        showStatus(i18n("This model cannot see page images. Select text or switch to a vision model."));
        return;
    }
    if (profile->kind != AiProfile::Kind::Codex && profile->model.isEmpty()) {
        showStatus(i18n("Set a model name for this API profile."));
        return;
    }
    if (!profile->endpoint.isEmpty()) {
        const QUrl endpoint(profile->endpoint);
        if (!endpoint.isValid() || (endpoint.scheme() != QLatin1String("http") && endpoint.scheme() != QLatin1String("https")) || endpoint.host().isEmpty()) {
            showStatus(i18n("Enter a valid HTTP or HTTPS base URL for this model."));
            return;
        }
    }
    if (profile->kind != AiProfile::Kind::Codex && profile->apiKey.isEmpty()) {
        bool ok = false;
        profile->apiKey = QInputDialog::getText(this, i18n("API key"), i18n("Enter the API key for %1:", profile->name), QLineEdit::Password, QString(), &ok);
        if (!ok || profile->apiKey.isEmpty()) {
            return;
        }
        QString error;
        AiStore::saveProfiles(m_profiles, winId(), &error);
        if (!error.isEmpty()) {
            showStatus(error);
        }
    }
    const int page = static_cast<int>(m_document->currentPage());
    const Okular::Page *documentPage = m_document->page(page);
    if (!documentPage) {
        showStatus(i18n("The current page is unavailable."));
        return;
    }
    if (!documentPage->hasTextPage()) {
        m_document->requestTextPage(page);
    }
    m_pendingMessage = AiMessage{QStringLiteral("user"), question, page, documentPage->text(nullptr).left(30000), m_selection, {}};
    if (!profile->vision) {
        submitQuestion(QString());
        return;
    }
    const double scale = 1600.0 / qMax(documentPage->width(), documentPage->height());
    m_imageWidth = qMax(1, qRound(documentPage->width() * scale));
    m_imageHeight = qMax(1, qRound(documentPage->height() * scale));
    m_pendingPage = page;
    showStatus(i18n("Rendering page %1…", page + 1));
    updateControls();
    if (documentPage->hasPixmap(this, m_imageWidth, m_imageHeight)) {
        pageImageReady();
        return;
    }
    auto *request = new Okular::PixmapRequest(this, page, m_imageWidth, m_imageHeight, 1.0, 1, Okular::PixmapRequest::Asynchronous);
    m_imageTimer->start(20000);
    m_document->requestPixmaps({request}, Okular::Document::NoOption);
}

void AiReadingAssistant::pageImageReady()
{
    m_imageTimer->stop();
    const Okular::Page *documentPage = m_document->page(m_pendingPage);
    if (!documentPage) {
        m_pendingPage = -1;
        showStatus(i18n("The page is no longer available."));
        updateControls();
        return;
    }
    QImage image(m_imageWidth, m_imageHeight, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    PagePainter::paintPageOnPainter(&painter, documentPage, this, 0, m_imageWidth, m_imageHeight, QRect(0, 0, m_imageWidth, m_imageHeight));
    painter.end();
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "JPEG", 85)) {
        m_pendingPage = -1;
        showStatus(i18n("Could not encode the page image."));
        updateControls();
        return;
    }
    m_pendingPage = -1;
    submitQuestion(QString::fromLatin1(encoded.toBase64()));
}

void AiReadingAssistant::submitQuestion(const QString &pageImage)
{
    AiProfile *profile = currentProfile();
    if (!profile) {
        return;
    }
    m_beforeRequest = m_conversation;
    m_questionSubmitted = true;
    for (AiMessage &message : m_conversation.messages) {
        message.pageImage.clear();
    }
    m_pendingMessage.pageImage = pageImage;
    m_conversation.messages.append(m_pendingMessage);
    AiStore::saveConversation(m_documentKey, profile->id, m_conversation);
    renderConversation();
    showStatus(i18n("Waiting for %1…", profile->name));
    m_provider.send(*profile, m_conversation, m_pendingMessage);
    updateControls();
}

void AiReadingAssistant::clearConversation()
{
    AiProfile *profile = currentProfile();
    if (!profile || (m_conversation.messages.isEmpty() && m_conversation.sessionId.isEmpty())) {
        return;
    }
    if (QMessageBox::question(this, i18n("Start new conversation"), i18n("Remove the current conversation from Okular? Saved annotations will remain.")) != QMessageBox::Yes) {
        return;
    }
    m_provider.cancel();
    m_questionSubmitted = false;
    if (!AiStore::clearConversation(m_documentKey, profile->id)) {
        showStatus(i18n("Could not remove the local conversation record."));
        return;
    }
    m_conversation = {};
    renderConversation();
    updateControls();
}

void AiReadingAssistant::renderConversation()
{
    QJsonArray json;
    for (const AiMessage &message : std::as_const(m_conversation.messages)) {
        json.append(QJsonObject{{QStringLiteral("role"), message.role}, {QStringLiteral("content"), message.content}});
    }
    m_view->setMessages(json);
}

void AiReadingAssistant::saveMessage(int messageIndex)
{
    if (messageIndex < 1 || messageIndex >= m_conversation.messages.size() || m_conversation.messages[messageIndex].role != QLatin1String("assistant")) {
        return;
    }
    const QUrl url = m_document->currentDocument();
    if (!url.isLocalFile() || !url.toLocalFile().endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive) || !m_document->isAllowed(Okular::AllowNotes)) {
        showStatus(i18n("AI annotations are currently available only for local PDFs that allow notes."));
        return;
    }
    for (int index = messageIndex - 1; index >= 0; --index) {
        const AiMessage &question = m_conversation.messages[index];
        if (question.role == QLatin1String("user")) {
            Q_EMIT saveAnswerRequested(question.content, m_conversation.messages[messageIndex].content, question.page);
            showStatus(i18n("Click a point on page %1 to place the annotation.", question.page + 1));
            return;
        }
    }
}

void AiReadingAssistant::updateControls()
{
    const bool busy = m_provider.isBusy() || m_pendingPage >= 0;
    m_send->setEnabled(!busy && m_document->isOpened() && m_profileCombo->currentIndex() >= 0);
    m_cancel->setEnabled(busy);
    m_profileCombo->setEnabled(!busy);
}
