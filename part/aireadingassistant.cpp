/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "aireadingassistant.h"

#include "aimarkdownview.h"
#include "core/document.h"
#include "core/generator.h"
#include "core/page.h"
#include "gui/pagepainter.h"

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
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <utility>

class AiPromptEdit final : public QTextEdit
{
public:
    explicit AiPromptEdit(QWidget *parent)
        : QTextEdit(parent)
        , m_button(new QPushButton(this))
    {
        m_button->setObjectName(QStringLiteral("aiPromptAction"));
        m_button->setFocusPolicy(Qt::NoFocus);
    }

    QPushButton *actionButton() const
    {
        return m_button;
    }

    void setActionText(const QString &text)
    {
        if (m_button->text() == text) {
            return;
        }
        m_button->setText(text);
        m_button->adjustSize();
        setViewportMargins(0, 0, m_button->width() + 12, 0);
        positionButton();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QTextEdit::resizeEvent(event);
        positionButton();
    }

private:
    void positionButton()
    {
        m_button->move(width() - frameWidth() - m_button->width() - 4, height() - frameWidth() - m_button->height() - 4);
    }

    QPushButton *m_button;
};

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
    auto *extra = new QPlainTextEdit(profile.extraArguments, &dialog);
    extra->setMaximumHeight(72);
    auto *key = new QLineEdit(&dialog);
    key->setEchoMode(QLineEdit::Password);
    key->setPlaceholderText(profile.apiKey.isEmpty() ? i18n("API key") : i18n("Leave blank to keep the current key"));
    auto *vision = new QCheckBox(i18n("This model accepts page images"), &dialog);
    vision->setChecked(profile.vision);
    form->addRow(i18n("Name:"), name);
    form->addRow(i18n("Protocol:"), kind);
    form->addRow(i18n("Base URL:"), endpoint);
    form->addRow(i18n("Model:"), model);
    form->addRow(i18n("Extra arguments:"), extra);
    form->addRow(i18n("API key:"), key);
    form->addRow(QString(), vision);
    const auto updateFields = [kind, endpoint, key, extra] {
        const bool codex = kind->currentData().toInt() == static_cast<int>(AiProfile::Kind::Codex);
        endpoint->setEnabled(!codex);
        key->setEnabled(!codex);
        extra->setPlaceholderText(codex ? i18n("Codex CLI options, e.g. -c model_reasoning_effort=medium (default: low)") : i18n("JSON request fields, e.g. {\"temperature\":0.2}"));
    };
    QObject::connect(kind, &QComboBox::currentIndexChanged, &dialog, updateFields);
    updateFields();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (name->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, i18n("AI model"), i18n("Give this model a name."));
            return;
        }
        const QUrl parsedEndpoint(endpoint->text().trimmed());
        if (!parsedEndpoint.userName().isEmpty() || !parsedEndpoint.password().isEmpty()) {
            QMessageBox::warning(&dialog, i18n("AI model"), i18n("Keep API credentials in the API key field, not in the endpoint URL."));
            return;
        }
        if (kind->currentData().toInt() != static_cast<int>(AiProfile::Kind::Codex) && !extra->toPlainText().trimmed().isEmpty()) {
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(extra->toPlainText().toUtf8(), &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) {
                QMessageBox::warning(&dialog, i18n("AI model"), i18n("Extra arguments must be a JSON object for this provider."));
                return;
            }
        }
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    if (profile.id.isEmpty()) {
        profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    profile.name = name->text().trimmed();
    const AiProfile::Kind selectedKind = static_cast<AiProfile::Kind>(kind->currentData().toInt());
    const QString selectedEndpoint = endpoint->text().trimmed();
    if (profile.kind != selectedKind || profile.endpoint != selectedEndpoint) {
        profile.apiKey.clear();
    }
    profile.kind = selectedKind;
    profile.endpoint = selectedEndpoint;
    profile.model = model->text().trimmed();
    profile.extraArguments = extra->toPlainText().trimmed();
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
    m_modelsButton = new QToolButton(this);
    m_modelsButton->setObjectName(QStringLiteral("aiModelsButton"));
    m_modelsButton->setText(i18n("Models…"));
    m_modelsButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_modelsButton->setPopupMode(QToolButton::MenuButtonPopup);
    auto *modelsMenu = new QMenu(m_modelsButton);
    m_conversationInstructionsAction = modelsMenu->addAction(i18n("Conversation instructions…"));
    m_conversationInstructionsAction->setObjectName(QStringLiteral("aiConversationInstructions"));
    m_bookDefaultPromptAction = modelsMenu->addAction(i18n("Book default prompt…"));
    m_newConversationAction = modelsMenu->addAction(i18n("Start new conversation"));
    m_newConversationAction->setObjectName(QStringLiteral("aiNewConversation"));
    m_modelsButton->setMenu(modelsMenu);
    top->addWidget(m_modelsButton);
    layout->addLayout(top);
    for (const AiProfile &profile : std::as_const(m_profiles)) {
        m_profileCombo->addItem(profile.name, profile.id);
    }
    connect(m_modelsButton, &QToolButton::clicked, this, &AiReadingAssistant::editProfiles);
    connect(m_conversationInstructionsAction, &QAction::triggered, this, &AiReadingAssistant::editConversationInstructions);
    connect(m_bookDefaultPromptAction, &QAction::triggered, this, &AiReadingAssistant::editBookDefaultPrompt);
    connect(m_newConversationAction, &QAction::triggered, this, &AiReadingAssistant::clearConversation);
    connect(m_profileCombo, &QComboBox::currentIndexChanged, this, &AiReadingAssistant::loadSelectedConversation);
    connect(m_profileCombo, &QComboBox::activated, this, [this](int) {
        if (!AiStore::isManagedBook(m_documentKey)) {
            return;
        }
        AiBookSettings settings = AiStore::loadBookSettings(m_documentKey);
        settings.selectedProfileId = m_profileCombo->currentData().toString();
        if (!AiStore::saveBookSettings(m_documentKey, settings)) {
            showStatus(i18n("Could not save this book's model selection."));
        }
    });

    m_view = new AiMarkdownView(this);
    layout->addWidget(m_view, 1);
    connect(m_view, &AiMarkdownView::saveRequested, this, &AiReadingAssistant::saveMessage);

    m_selectionLabel = new QLabel(this);
    m_selectionLabel->setWordWrap(true);
    m_selectionLabel->hide();
    layout->addWidget(m_selectionLabel);
    m_prompt = new AiPromptEdit(this);
    m_prompt->setPlaceholderText(i18n("Ask about the current page, for example: Explain equation 1.2"));
    m_prompt->setMaximumHeight(100);
    layout->addWidget(m_prompt);
    m_actionButton = m_prompt->actionButton();
    connect(m_actionButton, &QPushButton::clicked, this, [this] {
        if (m_provider.isBusy() || m_pendingPage >= 0) {
            cancelQuestion();
        } else {
            sendQuestion();
        }
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
        m_conversation.messages.append(AiMessage {QStringLiteral("assistant"), answer, -1, {}, {}, {}});
        bool saved = false;
        if (AiProfile *profile = currentProfile()) {
            saved = AiStore::saveConversation(m_documentKey, profile->id, m_conversation);
        }
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
            m_prompt->setPlainText(m_pendingMessage.content);
            if (AiProfile *profile = currentProfile()) {
                AiStore::saveConversation(m_documentKey, profile->id, m_conversation);
            }
        }
        renderConversation();
        showStatus(error);
        updateControls();
    });
    connect(&m_provider, &AiProvider::stopped, this, [this] {
        m_cancelling = false;
        updateControls();
    });

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
    m_cancelling = m_provider.isBusy();
    m_provider.cancel();
    m_pendingPage = -1;
    m_questionSubmitted = false;
    m_documentKey = url.isEmpty() ? QString() : m_document->contentHash();
    if (!url.isEmpty() && m_documentKey.isEmpty()) {
        m_documentKey = AiStore::documentKey(url.isLocalFile() ? url : m_document->localSource());
    }
    if (AiStore::isManagedBook(m_documentKey)) {
        const QString selectedId = AiStore::loadBookSettings(m_documentKey).selectedProfileId;
        m_profileCombo->setCurrentIndex(selectedId.isEmpty() ? -1 : m_profileCombo->findData(selectedId));
    }
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

void AiReadingAssistant::editConversationInstructions()
{
    AiProfile *profile = currentProfile();
    if (!profile || !m_historyReady || m_documentKey.isEmpty() || m_provider.isBusy() || m_pendingPage >= 0) {
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(i18n("Conversation instructions"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(i18n("Set preferences for this conversation, such as answer language and tone. They apply to future answers."), &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *editor = new QPlainTextEdit(m_conversation.instructions, &dialog);
    editor->setObjectName(QStringLiteral("aiConversationInstructionsEdit"));
    editor->setPlaceholderText(i18n("For example: Answer in Chinese. Use a concise, patient tone."));
    layout->addWidget(editor);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(420, 260);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QString previous = m_conversation.instructions;
    m_conversation.instructions = editor->toPlainText().trimmed();
    if (!AiStore::saveConversation(m_documentKey, profile->id, m_conversation)) {
        m_conversation.instructions = previous;
        showStatus(i18n("Could not save these conversation instructions locally."));
    } else {
        showStatus(i18n("Conversation instructions saved."));
        updateControls();
    }
}

void AiReadingAssistant::editBookDefaultPrompt()
{
    if (!AiStore::isManagedBook(m_documentKey) || m_provider.isBusy()) {
        return;
    }
    AiBookSettings settings = AiStore::loadBookSettings(m_documentKey);
    QDialog dialog(this);
    dialog.setWindowTitle(i18n("Book default prompt"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(i18n("New conversations for this book start with this prompt. Existing conversations keep their own instructions."), &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *editor = new QPlainTextEdit(settings.defaultPrompt, &dialog);
    layout->addWidget(editor);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(420, 260);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    settings.defaultPrompt = editor->toPlainText().trimmed();
    showStatus(AiStore::saveBookSettings(m_documentKey, settings) ? i18n("Book default prompt saved.") : i18n("Could not save this book's default prompt."));
}

void AiReadingAssistant::loadSelectedConversation()
{
    m_cancelling = m_provider.isBusy();
    m_provider.cancel();
    m_pendingPage = -1;
    m_questionSubmitted = false;
    m_historyReady = false;
    m_conversation = {};
    QString error;
    if (AiProfile *profile = currentProfile(); profile && !m_documentKey.isEmpty()) {
        m_conversation = AiStore::loadConversation(m_documentKey, profile->id, &error);
        m_historyReady = error.isEmpty();
    }
    if (!error.isEmpty()) {
        showStatus(i18n("Could not load the local conversation: %1", error));
    }
    renderConversation();
    updateControls();
}

void AiReadingAssistant::sendQuestion()
{
    AiProfile *profile = currentProfile();
    if (!profile || !m_historyReady || !m_document->isOpened() || m_documentKey.isEmpty()) {
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
    m_pendingMessage = AiMessage {QStringLiteral("user"), question, page, documentPage->text(nullptr).left(30000), m_selection, {}};
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
    m_prompt->clear();
    renderConversation();
    showStatus(i18n("Waiting for %1…", profile->name));
    m_provider.send(*profile, m_conversation, m_pendingMessage);
    updateControls();
}

void AiReadingAssistant::cancelQuestion()
{
    m_cancelling = m_provider.isBusy();
    m_provider.cancel();
    m_imageTimer->stop();
    m_pendingPage = -1;
    if (m_questionSubmitted) {
        m_conversation = m_beforeRequest;
        m_questionSubmitted = false;
        m_prompt->setPlainText(m_pendingMessage.content);
        if (AiProfile *profile = currentProfile()) {
            AiStore::saveConversation(m_documentKey, profile->id, m_conversation);
        }
        renderConversation();
    }
    showStatus(i18n("Request canceled."));
    updateControls();
}

void AiReadingAssistant::clearConversation()
{
    AiProfile *profile = currentProfile();
    if (!profile || !m_historyReady || m_documentKey.isEmpty() || (m_conversation.messages.isEmpty() && m_conversation.sessionId.isEmpty() && m_conversation.instructions.isEmpty()) || m_provider.isBusy() || m_pendingPage >= 0) {
        return;
    }
    if (QMessageBox::question(this, i18n("Start new conversation"), i18n("Remove the current conversation from Okular? Saved annotations will remain.")) != QMessageBox::Yes) {
        return;
    }
    if (!AiStore::clearConversation(m_documentKey, profile->id)) {
        showStatus(i18n("Could not remove the local conversation record."));
        return;
    }
    m_conversation = {};
    m_conversation.instructions = AiStore::loadBookSettings(m_documentKey).defaultPrompt;
    renderConversation();
    showStatus(i18n("New conversation started."));
    updateControls();
}

void AiReadingAssistant::renderConversation()
{
    QJsonArray json;
    for (const AiMessage &message : std::as_const(m_conversation.messages)) {
        json.append(QJsonObject {{QStringLiteral("role"), message.role}, {QStringLiteral("content"), message.content}});
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
    m_prompt->setActionText(busy ? i18n("Cancel") : i18n("Ask"));
    m_actionButton->setEnabled(busy ? !m_cancelling : m_historyReady && !m_documentKey.isEmpty() && m_document->isOpened() && m_profileCombo->currentIndex() >= 0);
    m_profileCombo->setEnabled(!busy);
    m_modelsButton->setEnabled(!busy);
    m_conversationInstructionsAction->setEnabled(!busy && m_historyReady && !m_documentKey.isEmpty() && m_profileCombo->currentIndex() >= 0);
    m_bookDefaultPromptAction->setEnabled(!busy && AiStore::isManagedBook(m_documentKey));
    m_newConversationAction->setEnabled(!busy && m_historyReady && !m_documentKey.isEmpty() && (!m_conversation.messages.isEmpty() || !m_conversation.sessionId.isEmpty() || !m_conversation.instructions.isEmpty()));
    m_prompt->setReadOnly(busy);
}
