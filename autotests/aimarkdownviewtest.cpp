/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../part/aimarkdownview.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QWebEnginePage>

class AiMarkdownViewTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void rendersMathWithoutLoadingRemoteImages()
    {
        AiMarkdownView view;
        QSignalSpy loaded(&view, &QWebEngineView::loadFinished);
        if (loaded.isEmpty()) {
            QVERIFY(loaded.wait(15000));
        }
        QVERIFY(loaded.last().at(0).toBool());

        view.setMessages(QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                                                {QStringLiteral("content"), QStringLiteral("Equation $x^2$ and ![remote](https://example.org/picture.png)")}}});
        QVariant result;
        QTimer::singleShot(200, &view, [&view, &result] {
            view.page()->runJavaScript(QStringLiteral("({math: document.querySelectorAll('.katex').length, images: document.querySelectorAll('img').length, links: document.querySelectorAll('a[href^=\"https:\"]').length})"),
                                       [&result](const QVariant &value) { result = value; });
        });
        QTRY_VERIFY_WITH_TIMEOUT(result.isValid(), 10000);
        const QVariantMap values = result.toMap();
        QVERIFY(values.value(QStringLiteral("math")).toInt() > 0);
        QCOMPARE(values.value(QStringLiteral("images")).toInt(), 0);
        QCOMPARE(values.value(QStringLiteral("links")).toInt(), 1);
    }
};

QTEST_MAIN(AiMarkdownViewTest)
#include "aimarkdownviewtest.moc"
