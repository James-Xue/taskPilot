// tst_queue_model.cpp — queue mapping and order, without a daemon

#include "ui/queue_model.h"

#include <QTest>

using taskpilot::ui::QueueModel;
using taskpilot::ui::QueueRow;

namespace
{

QByteArray queuePayload()
{
    return R"({
        "now": 1700000000,
        "weights": {"importance": 5, "urgency": 30, "age": 2, "blocks": 8,
                    "urgency_horizon_days": 7},
        "queue": [
            {"id": 3, "uid": "uid-c", "title": "C", "notes": "third",
             "status": "open", "importance": 2, "due_at": null, "blocks": 0,
             "tags": ["home"], "created_at": 1, "updated_at": 1,
             "completed_at": null, "score": 41.5,
             "score_parts": {"importance": 10, "urgency": 8.5, "age": 18,
                             "blocks": 0},
             "urgency_factor": 0.28, "age_days": 9},
            {"id": 1, "uid": "uid-a", "title": "A", "notes": "first",
             "status": "open", "importance": 5, "due_at": 1700086400,
             "blocks": 2, "tags": ["work", "urgent"], "created_at": 2,
             "updated_at": 2, "completed_at": null, "score": 77.0,
             "score_parts": {"importance": 25, "urgency": 30, "age": 6,
                             "blocks": 16},
             "urgency_factor": 1.0, "age_days": 3},
            {"id": 2, "uid": "uid-b", "title": "B", "notes": "second",
             "status": "in_progress", "importance": 4, "due_at": null,
             "blocks": 0, "tags": [], "created_at": 3, "updated_at": 3,
             "completed_at": null, "score": 47.71,
             "score_parts": {"importance": 20, "urgency": 25.71, "age": 2,
                             "blocks": 0},
             "urgency_factor": 0.857, "age_days": 1}
        ]
    })";
}

} // namespace

class QueueModelTest : public QObject
{
    Q_OBJECT

  private slots:
    void rowsKeepReplyOrder();
    void rolesMapTheDeliveredFields();
    void emptyQueueHasNoRows();
    void malformedReplyKeepsThePreviousRows();
};

void QueueModelTest::rowsKeepReplyOrder()
{
    QueueModel model;
    QString error;
    QVERIFY2(model.applyQueueReply(queuePayload(), &error), qPrintable(error));

    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.rows().at(0).title, QStringLiteral("C"));
    QCOMPARE(model.rows().at(1).title, QStringLiteral("A"));
    QCOMPARE(model.rows().at(2).title, QStringLiteral("B"));
}

void QueueModelTest::rolesMapTheDeliveredFields()
{
    QueueModel model;
    QString error;
    QVERIFY2(model.applyQueueReply(queuePayload(), &error), qPrintable(error));

    const QModelIndex first = model.index(1, 0);
    QCOMPARE(first.data(QueueModel::TaskIdRole).toLongLong(), 1LL);
    QCOMPARE(first.data(QueueModel::UidRole).toString(), QStringLiteral("uid-a"));
    QCOMPARE(first.data(QueueModel::TitleRole).toString(), QStringLiteral("A"));
    QCOMPARE(first.data(QueueModel::StatusRole).toString(), QStringLiteral("open"));
    QCOMPARE(first.data(QueueModel::ImportanceRole).toInt(), 5);
    QCOMPARE(first.data(QueueModel::BlocksRole).toInt(), 2);
    QCOMPARE(first.data(QueueModel::HasDueRole).toBool(), true);
    QCOMPARE(first.data(QueueModel::DueTextRole).toString(), QStringLiteral("2023-11-16"));
    // The delivered total is kept verbatim; the parts are evidence only.
    QCOMPARE(first.data(QueueModel::ScoreRole).toDouble(), 77.0);
    QCOMPARE(first.data(QueueModel::ScoreImportanceRole).toDouble(), 25.0);
    QCOMPARE(first.data(QueueModel::ScoreUrgencyRole).toDouble(), 30.0);
    QCOMPARE(first.data(QueueModel::ScoreAgeRole).toDouble(), 6.0);
    QCOMPARE(first.data(QueueModel::ScoreBlocksRole).toDouble(), 16.0);
    QCOMPARE(first.data(QueueModel::UrgencyFactorRole).toDouble(), 1.0);
    QCOMPARE(first.data(QueueModel::AgeDaysRole).toDouble(), 3.0);
    QCOMPARE(first.data(QueueModel::TagsRole).toStringList(),
             QStringList({ QStringLiteral("work"), QStringLiteral("urgent") }));

    const QVariantMap detail = model.get(1);
    QCOMPARE(detail.value(QStringLiteral("id")).toLongLong(), 1LL);
    QCOMPARE(detail.value(QStringLiteral("title")).toString(), QStringLiteral("A"));
    QCOMPARE(detail.value(QStringLiteral("score")).toDouble(), 77.0);
}

void QueueModelTest::emptyQueueHasNoRows()
{
    QueueModel model;
    QString error;
    QVERIFY2(model.applyQueueReply(QByteArrayLiteral(R"({"now":1,"weights":{},"queue":[]})"),
                                   &error),
             qPrintable(error));
    QCOMPARE(model.rowCount(), 0);
}

void QueueModelTest::malformedReplyKeepsThePreviousRows()
{
    QueueModel model;
    QString error;
    QVERIFY(model.applyQueueReply(queuePayload(), &error));
    QCOMPARE(model.rowCount(), 3);

    QVERIFY(!model.applyQueueReply(QByteArrayLiteral(R"({"queue": 5})"), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.rows().at(1).title, QStringLiteral("A"));
}

QTEST_GUILESS_MAIN(QueueModelTest)

#include "tst_queue_model.moc"
