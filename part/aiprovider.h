/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "aistore.h"

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <functional>

class QNetworkReply;
class QTemporaryFile;

class AiProvider : public QObject
{
    Q_OBJECT
public:
    explicit AiProvider(QObject *parent = nullptr);
    void send(const AiProfile &profile, const AiConversation &conversation, const AiMessage &message);
    void cancel();
    bool isBusy() const;

Q_SIGNALS:
    void completed(const QString &answer, const QString &sessionId);
    void failed(const QString &message);
    void stopped();
    void answerUpdated(const QString &answer);

private:
    void sendOpenAiChat();
    void createOpenAiConversation();
    void sendOpenAiResponse();
    void sendChatGptResponse();
    void processResponseStream();
    void sendAnthropic();
    void sendCodex();
    void finishHttp(QNetworkReply *reply, const std::function<void(const QJsonObject &)> &onSuccess);
    void processCodexOutput();
    QUrl endpoint(const QString &suffix) const;
    QString contextText(const AiMessage &message) const;
    QString conversationInstructions() const;

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QProcess *m_process = nullptr;
    QTemporaryFile *m_imageFile = nullptr;
    AiProfile m_profile;
    QJsonObject m_extraPayload;
    AiConversation m_conversation;
    AiMessage m_message;
    QString m_sessionId;
    QString m_lastAnswer;
    QByteArray m_outputBuffer;
    QByteArray m_errorBuffer;
    bool m_cancelled = false;
    bool m_streamCompleted = false;
    QString m_streamError;
};
