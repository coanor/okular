/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QJsonArray>
#include <QWebEnginePage>
#include <QWebEngineView>

class QWebEngineProfile;

class AiMarkdownPage : public QWebEnginePage
{
    Q_OBJECT
public:
    explicit AiMarkdownPage(QWebEngineProfile *profile);

Q_SIGNALS:
    void saveRequested(int messageIndex);

protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override;
};

class AiMarkdownView : public QWebEngineView
{
    Q_OBJECT
public:
    explicit AiMarkdownView(QWidget *parent = nullptr);
    ~AiMarkdownView() override;
    void setMessages(const QJsonArray &messages);

Q_SIGNALS:
    void saveRequested(int messageIndex);

private:
    void updatePage();

    QJsonArray m_messages;
    bool m_ready = false;
};
