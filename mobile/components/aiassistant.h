/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "chatgptconnection.h"
#include "core/observer.h"
#include "part/aiprovider.h"

#include <QFont>
#include <QSize>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <qqmlregistration.h>

class DocumentItem;

class AiAssistant : public QObject, public Okular::DocumentObserver
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Use DocumentItem.aiAssistant")

    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY profilesChanged)
    Q_PROPERTY(int currentProfile READ currentProfile WRITE setCurrentProfile NOTIFY currentProfileChanged)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY conversationChanged)
    Q_PROPERTY(QString question READ question WRITE setQuestion NOTIFY questionChanged)
    Q_PROPERTY(QString selection READ selection NOTIFY selectionChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(QObject *chatGpt READ chatGpt CONSTANT)

public:
    explicit AiAssistant(DocumentItem *document);
    ~AiAssistant() override;

    QVariantList profiles() const;
    int currentProfile() const;
    void setCurrentProfile(int index);
    QVariantList messages() const;
    QString question() const;
    void setQuestion(const QString &question);
    QString selection() const;
    QString status() const;
    bool busy() const;
    bool ready() const;
    QObject *chatGpt();

    Q_INVOKABLE void activate();
    Q_INVOKABLE QVariantMap profile(int index) const;
    Q_INVOKABLE bool saveProfile(int index, const QVariantMap &fields);
    Q_INVOKABLE bool saveChatGptProfile(int index, const QVariantMap &fields);
    Q_INVOKABLE void removeProfile(int index);
    Q_INVOKABLE void setSelection(const QString &text, int page = -1);
    Q_INVOKABLE void ask();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clearConversation();
    Q_INVOKABLE QString renderMarkdown(const QString &text, const QFont &font) const;

    void resetDocument();
    void flushHistory();
    void notifyPageChanged(int page, int flags) override;

Q_SIGNALS:
    void profilesChanged();
    void currentProfileChanged();
    void conversationChanged();
    void questionChanged();
    void selectionChanged();
    void statusChanged();
    void busyChanged();
    void readyChanged();

private:
    void setStatus(const QString &status);
    void loadConversation();
    void persistConversation();
    void rollbackQuestion();
    void pageImageReady();
    void submitQuestion(const QString &image);
    bool storeProfile(int index, AiProfile profile);

    DocumentItem *m_document;
    AiProvider m_provider;
    ChatGptConnection m_chatGpt;
    QList<AiProfile> m_profiles;
    QList<AiProfile> m_allProfiles;
    int m_currentProfile = -1;
    AiConversation m_conversation;
    AiConversation m_beforeRequest;
    AiMessage m_pendingMessage;
    QString m_documentKey;
    QString m_question;
    QString m_selection;
    int m_selectionPage = -1;
    QString m_status;
    int m_documentGeneration = 0;
    int m_requestGeneration = 0;
    int m_historyGeneration = 0;
    bool m_loadingHistory = false;
    bool m_historyReady = false;
    QThreadPool m_historyPool;
    bool m_encoding = false;
    bool m_hashing = false;
    bool m_submitted = false;
    bool m_authPending = false;
    QString m_streamedAnswer;
    int m_pendingPage = -1;
    QSize m_imageSize;
    QTimer m_imageTimer;
};
