/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "mobile/components/textselection.h"
#include "core/misc.h"
#include "core/textpage.h"
#include "part/mdxdictionary.h"

#include <QTest>

class TextSelectionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testTouchTolerance_data();
    void testTouchTolerance();
    void testOrdinaryLineBreak();
    void testSplitWord_data();
    void testSplitWord();
    void testWordBoundaries_data();
    void testWordBoundaries();
    void testRotatedSelectionText();
};

void TextSelectionTest::testTouchTolerance_data()
{
    QTest::addColumn<QPointF>("point");
    QTest::addColumn<QSizeF>("size");
    QTest::addColumn<QString>("expected");
    const QSizeF size(500, 1000);
    QTest::newRow("inside-word") << QPointF(100, 120) << size << QStringLiteral("hello");
    QTest::newRow("space-next-to-word") << QPointF(155, 120) << size << QStringLiteral("hello");
    QTest::newRow("below-baseline") << QPointF(100, 158) << size << QStringLiteral("hello");
    QTest::newRow("outside-left-edge") << QPointF(40, 120) << size << QStringLiteral("hello");
    QTest::newRow("nearest-second-word") << QPointF(170, 120) << size << QStringLiteral("world");
    QTest::newRow("far-from-text") << QPointF(10, 10) << size << QString();
    QTest::newRow("zoomed-in") << QPointF(310, 240) << QSizeF(1000, 2000) << QStringLiteral("hello");
    QTest::newRow("zoomed-out") << QPointF(80, 60) << QSizeF(250, 500) << QStringLiteral("hello");
}

void TextSelectionTest::testTouchTolerance()
{
    QFETCH(QPointF, point);
    QFETCH(QSizeF, size);
    QFETCH(QString, expected);
    const Okular::TextEntity::List entities = {
        Okular::TextEntity(QStringLiteral("hello"), Okular::NormalizedRect(0.1, 0.1, 0.3, 0.15)),
        Okular::TextEntity(QStringLiteral(" "), Okular::NormalizedRect(0.3, 0.1, 0.35, 0.15)),
        Okular::TextEntity(QStringLiteral("world"), Okular::NormalizedRect(0.35, 0.1, 0.55, 0.15)),
    };
    const auto nearest = MobileTextSelection::nearestTextPoint(entities, point, size, 22);
    if (expected.isEmpty()) {
        QVERIFY(!nearest);
        return;
    }
    QVERIFY(nearest);
    const Okular::TextPage page(entities);
    const auto area = page.wordAt(Okular::NormalizedPoint(nearest->x(), nearest->y()));
    QVERIFY(area);
    QCOMPARE(page.text(area.get(), Okular::TextPage::CentralPixelTextAreaInclusionBehaviour).trimmed(), expected);
}

void TextSelectionTest::testOrdinaryLineBreak()
{
    const Okular::TextEntity::List entities = {
        Okular::TextEntity(QStringLiteral("hello"), Okular::NormalizedRect(0.1, 0.1, 0.3, 0.12)),
        Okular::TextEntity(QStringLiteral("\n"), Okular::NormalizedRect(0.3, 0.1, 1.0, 0.12)),
        Okular::TextEntity(QStringLiteral("world"), Okular::NormalizedRect(0.1, 0.14, 0.3, 0.16)),
    };
    const Okular::TextPage page(entities);
    const auto area = page.wordAt(entities.first().area().center());
    QVERIFY(area);
    QCOMPARE(page.text(area.get(), Okular::TextPage::CentralPixelTextAreaInclusionBehaviour), QStringLiteral("hello"));
}

void TextSelectionTest::testSplitWord_data()
{
    QTest::addColumn<QString>("firstPart");
    QTest::addColumn<QString>("separator");
    QTest::addColumn<bool>("characters");
    QTest::newRow("separate-newline") << QStringLiteral("prin-") << QStringLiteral("\n") << false;
    QTest::newRow("embedded-newline") << QStringLiteral("prin-\n") << QString() << false;
    QTest::newRow("geometric-line-break") << QStringLiteral("prin-") << QString() << false;
    QTest::newRow("space-at-line-break") << QStringLiteral("prin-") << QStringLiteral(" ") << false;
    QTest::newRow("trailing-space") << QStringLiteral("prin- ") << QString() << false;
    QTest::newRow("soft-hyphen") << QStringLiteral("prin\u00ad") << QStringLiteral("\n") << false;
    QTest::newRow("individual-characters") << QStringLiteral("prin-") << QString() << true;
    QTest::newRow("individual-characters-with-space") << QStringLiteral("prin-") << QStringLiteral(" ") << true;
}

void TextSelectionTest::testSplitWord()
{
    QFETCH(QString, firstPart);
    QFETCH(QString, separator);
    QFETCH(bool, characters);
    const Okular::NormalizedRect first(0.6, 0.1, 0.9, 0.12);
    const Okular::NormalizedRect second(0.1, 0.14, 0.4, 0.16);
    Okular::TextEntity::List entities;
    const auto appendPart = [&entities, characters](const QString &text, const Okular::NormalizedRect &area) {
        if (!characters) {
            entities.append(Okular::TextEntity(text, area));
            return;
        }
        const double width = (area.right - area.left) / text.size();
        for (qsizetype i = 0; i < text.size(); ++i) {
            entities.append(Okular::TextEntity(QString(text[i]), Okular::NormalizedRect(area.left + i * width, area.top, area.left + (i + 1) * width, area.bottom)));
        }
    };
    appendPart(firstPart, first);
    if (!separator.isEmpty()) {
        entities.append(Okular::TextEntity(separator, Okular::NormalizedRect(0.9, 0.1, 1.0, 0.12)));
    }
    appendPart(QStringLiteral("ted"), second);
    entities.append(Okular::TextEntity(QStringLiteral(" "), Okular::NormalizedRect(0.4, 0.14, 0.45, 0.16)));
    const Okular::NormalizedRect following(0.45, 0.14, 0.7, 0.16);
    entities.append(Okular::TextEntity(QStringLiteral("words"), following));
    Okular::TextPage page(entities);
    for (const auto &point : {first.center(), second.center()}) {
        const auto nearest = MobileTextSelection::nearestTextPoint(entities, QPointF(point.x * 1000, point.y * 1000), QSizeF(1000, 1000), 22);
        QVERIFY(nearest);
        const auto area = page.wordAt(Okular::NormalizedPoint(nearest->x(), nearest->y()));
        QVERIFY(area);
        QVERIFY(area->contains(first.center().x, first.center().y));
        QVERIFY(area->contains(second.center().x, second.center().y));
        QVERIFY(!area->contains(following.center().x, following.center().y));
        const auto selected = page.words(area.get(), Okular::TextPage::CentralPixelTextAreaInclusionBehaviour);
        QCOMPARE(MdxDictionary::word(MobileTextSelection::selectionText(selected)), QStringLiteral("printed"));
    }
}

void TextSelectionTest::testWordBoundaries_data()
{
    QTest::addColumn<QString>("firstPart");
    QTest::addColumn<QString>("separator");
    QTest::addColumn<Okular::NormalizedRect>("secondArea");
    QTest::addColumn<QString>("expected");
    const Okular::NormalizedRect sameLine(0.3, 0.1, 0.4, 0.12);
    const Okular::NormalizedRect nextLine(0.1, 0.14, 0.2, 0.16);
    QTest::newRow("inline-hyphen") << QStringLiteral("well-") << QString() << sameLine << QStringLiteral("well-known");
    QTest::newRow("inline-dash-space") << QStringLiteral("well-") << QStringLiteral(" ") << sameLine << QStringLiteral("well");
    QTest::newRow("ordinary-new-line") << QStringLiteral("well") << QString() << nextLine << QStringLiteral("well");
    QTest::newRow("paragraph-break") << QStringLiteral("well-") << QStringLiteral("\n\n") << Okular::NormalizedRect(0.1, 0.3, 0.2, 0.32) << QStringLiteral("well");
}

void TextSelectionTest::testWordBoundaries()
{
    QFETCH(QString, firstPart);
    QFETCH(QString, separator);
    QFETCH(Okular::NormalizedRect, secondArea);
    QFETCH(QString, expected);
    const Okular::NormalizedRect first(0.1, 0.1, 0.25, 0.12);
    Okular::TextEntity::List entities = {Okular::TextEntity(firstPart, first)};
    if (!separator.isEmpty()) {
        entities.append(Okular::TextEntity(separator, Okular::NormalizedRect(0.25, 0.1, 0.3, 0.12)));
    }
    entities.append(Okular::TextEntity(QStringLiteral("known"), secondArea));
    const Okular::TextPage page(entities);
    const auto area = page.wordAt(first.center());
    QVERIFY(area);
    QCOMPARE(MdxDictionary::word(MobileTextSelection::selectionText(page.words(area.get(), Okular::TextPage::CentralPixelTextAreaInclusionBehaviour))), expected);
}

void TextSelectionTest::testRotatedSelectionText()
{
    const Okular::TextEntity::List entities = {
        Okular::TextEntity(QStringLiteral("prin-"), Okular::NormalizedRect(0.6, 0.1, 0.9, 0.12)),
        Okular::TextEntity(QStringLiteral("ted"), Okular::NormalizedRect(0.1, 0.14, 0.4, 0.16)),
    };
    for (int angle : {0, 90, 180, 270}) {
        QTransform rotation;
        rotation.rotate(angle);
        Okular::TextEntity::List rotated;
        for (const auto &entity : entities) {
            rotated.append(Okular::TextEntity(entity.text(), entity.transformedArea(rotation)));
        }
        QCOMPARE(MdxDictionary::word(MobileTextSelection::selectionText(rotated, rotation.inverted())), QStringLiteral("printed"));
    }
}

QTEST_GUILESS_MAIN(TextSelectionTest)
#include "textselectiontest.moc"
