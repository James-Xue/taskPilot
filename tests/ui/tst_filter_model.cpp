// tst_filter_model.cpp — tag and text filtering preserves the daemon order

#include "ui/queue_model.h"
#include "ui/task_filter_model.h"

#include <QTest>

using taskpilot::ui::QueueModel;
using taskpilot::ui::TaskFilterModel;

namespace
{

QByteArray queuePayload()
{
    return R"({"now":1,"weights":{},"queue":[
        {"id":1,"uid":"a","title":"Alpha","notes":"first note","status":"open",
         "importance":3,"due_at":null,"blocks":0,"tags":["work"],
         "score":30,"score_parts":{},"urgency_factor":0,"age_days":1},
        {"id":2,"uid":"b","title":"Beta","notes":"","status":"open",
         "importance":3,"due_at":null,"blocks":0,"tags":["home"],
         "score":20,"score_parts":{},"urgency_factor":0,"age_days":1},
        {"id":3,"uid":"c","title":"Charlie","notes":"second note","status":"open",
         "importance":3,"due_at":null,"blocks":0,"tags":["work","home"],
         "score":10,"score_parts":{},"urgency_factor":0,"age_days":1}
    ]})";
}

} // namespace

class FilterModelTest : public QObject
{
    Q_OBJECT

  private slots:
    void filterPreservesOrder();
    void tagFilterUsesOrSemantics();
    void searchIsCaseInsensitiveAcrossTitleAndNotes();
    void clearingRestoresEverything();
};

void FilterModelTest::filterPreservesOrder()
{
    QueueModel source;
    QString error;
    QVERIFY2(source.applyQueueReply(queuePayload(), &error), qPrintable(error));

    TaskFilterModel filter;
    filter.setSourceModel(&source);
    QCOMPARE(filter.rowCount(), 3);
    QCOMPARE(filter.get(0).value(QStringLiteral("title")).toString(), QStringLiteral("Alpha"));
    QCOMPARE(filter.get(1).value(QStringLiteral("title")).toString(), QStringLiteral("Beta"));
    QCOMPARE(filter.get(2).value(QStringLiteral("title")).toString(), QStringLiteral("Charlie"));
}

void FilterModelTest::tagFilterUsesOrSemantics()
{
    QueueModel source;
    QString error;
    QVERIFY(source.applyQueueReply(queuePayload(), &error));
    TaskFilterModel filter;
    filter.setSourceModel(&source);

    filter.setTagFilter({ QStringLiteral("work") });
    QCOMPARE(filter.rowCount(), 2);
    QCOMPARE(filter.get(0).value(QStringLiteral("title")).toString(), QStringLiteral("Alpha"));
    QCOMPARE(filter.get(1).value(QStringLiteral("title")).toString(), QStringLiteral("Charlie"));

    filter.setTagFilter({ QStringLiteral("work"), QStringLiteral("home") });
    QCOMPARE(filter.rowCount(), 3);

    filter.setTagFilter({ QStringLiteral("home") });
    QCOMPARE(filter.rowCount(), 2);
    QCOMPARE(filter.get(0).value(QStringLiteral("title")).toString(), QStringLiteral("Beta"));
    QCOMPARE(filter.get(1).value(QStringLiteral("title")).toString(), QStringLiteral("Charlie"));
}

void FilterModelTest::searchIsCaseInsensitiveAcrossTitleAndNotes()
{
    QueueModel source;
    QString error;
    QVERIFY(source.applyQueueReply(queuePayload(), &error));
    TaskFilterModel filter;
    filter.setSourceModel(&source);

    filter.setSearchText(QStringLiteral("alpha"));
    QCOMPARE(filter.rowCount(), 1);
    QCOMPARE(filter.get(0).value(QStringLiteral("title")).toString(), QStringLiteral("Alpha"));

    filter.setSearchText(QStringLiteral("NOTE"));
    QCOMPARE(filter.rowCount(), 2);
    QCOMPARE(filter.get(0).value(QStringLiteral("title")).toString(), QStringLiteral("Alpha"));
    QCOMPARE(filter.get(1).value(QStringLiteral("title")).toString(), QStringLiteral("Charlie"));

    filter.setSearchText(QStringLiteral("nothing matches"));
    QCOMPARE(filter.rowCount(), 0);
}

void FilterModelTest::clearingRestoresEverything()
{
    QueueModel source;
    QString error;
    QVERIFY(source.applyQueueReply(queuePayload(), &error));
    TaskFilterModel filter;
    filter.setSourceModel(&source);

    filter.setTagFilter({ QStringLiteral("work") });
    filter.setSearchText(QStringLiteral("charlie"));
    QCOMPARE(filter.rowCount(), 1);

    filter.setTagFilter({});
    filter.setSearchText({});
    QCOMPARE(filter.rowCount(), 3);
}

QTEST_GUILESS_MAIN(FilterModelTest)

#include "tst_filter_model.moc"
