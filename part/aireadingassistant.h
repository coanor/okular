/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "aiprovider.h"
#include "core/observer.h"

#include <QPointer>
#include <QWidget>

class AiMarkdownView;
class AiPromptEdit;
class QAction;
class QComboBox;
class QLabel;
class QPushButton;
class QTimer;
class QToolButton;

namespace Okular
{
class Document;
}

class AiReadingAssistant : public QWidget, public Okular::DocumentObserver
{
    Q_OBJECT
public:
    explicit AiReadingAssistant(Okular::Document *document, QWidget *parent = nullptr);
    ~AiReadingAssistant() override;
    void setDocumentUrl(const QUrl &url);
    void askAboutSelection(const QString &text);
    void showStatus(const QString &text);

    void notifyPageChanged(int page, int flags) override;
    void notifySetup(const QList<Okular::Page *> &pages, int setupFlags) override;

Q_SIGNALS:
    void saveAnswerRequested(const QString &question, const QString &answer, int page);

private:
    void editProfiles();
    void loadSelectedConversation();
    void sendQuestion();
    void submitQuestion(const QString &pageImage);
    void cancelQuestion();
    void clearConversation();
    void pageImageReady();
    void renderConversation();
    void saveMessage(int messageIndex);
    AiProfile *currentProfile();
    void updateControls();

    Okular::Document *m_document;
    AiProvider m_provider;
    QList<AiProfile> m_profiles;
    AiConversation m_conversation;
    AiConversation m_beforeRequest;
    bool m_questionSubmitted = false;
    bool m_cancelling = false;
    QString m_documentKey;
    QString m_selection;
    AiMessage m_pendingMessage;
    int m_pendingPage = -1;
    int m_imageWidth = 0;
    int m_imageHeight = 0;
    QTimer *m_imageTimer;
    QComboBox *m_profileCombo;
    QToolButton *m_modelsButton;
    QAction *m_newConversationAction;
    QLabel *m_selectionLabel;
    QLabel *m_status;
    AiPromptEdit *m_prompt;
    QPushButton *m_actionButton;
    AiMarkdownView *m_view;
};
