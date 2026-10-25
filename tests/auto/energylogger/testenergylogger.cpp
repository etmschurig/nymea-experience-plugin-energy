// SPDX-License-Identifier: GPL-3.0-or-later

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
*
* Copyright (C) 2026, ETM-Schurig SARL
*
* This file is part of nymea-experience-plugin-energy.
*
* nymea-experience-plugin-energy is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* nymea-experience-plugin-energy is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with nymea-experience-plugin-energy. If not, see <https://www.gnu.org/licenses/>.
*
* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

// White box tests: the sampling helpers live in an anonymous namespace of energylogger.cpp,
// so the source file is included here directly.
#include "energylogger.cpp"

#include <QtTest>
#include <QTemporaryDir>
#include <QMutex>

#include <functional>

#include <time.h>

Q_LOGGING_CATEGORY(dcEnergyExperience, "EnergyExperience")

namespace {

const qint64 minuteMSecs = 60 * 1000;

// Autumn: 2026-10-25 03:00 CEST -> 02:00 CET. Spring: 2026-03-29 02:00 CET -> 03:00 CEST.
const QString autumnTransition = QStringLiteral("2026-10-25T01:00:00Z");
const QString springTransition = QStringLiteral("2026-03-29T01:00:00Z");

QDateTime utc(const QString &isoString)
{
    return QDateTime::fromString(isoString, Qt::ISODate).toLocalTime();
}

QString describe(const QDateTime &dateTime)
{
    return dateTime.toUTC().toString(Qt::ISODate) + " (" + dateTime.toLocalTime().toString(QStringLiteral("dd.MM. HH:mm t")) + ")";
}

#ifndef ENERGYLOGGER_LEGACY
QMutex s_messagesMutex;
QStringList s_warnings;
QtMessageHandler s_previousHandler = nullptr;

void recordingMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type == QtWarningMsg) {
        QMutexLocker locker(&s_messagesMutex);
        s_warnings.append(message);
    }
    s_previousHandler(type, context, message);
}

int countWarnings(const QString &pattern)
{
    QMutexLocker locker(&s_messagesMutex);
    return s_warnings.filter(pattern).count();
}
#endif

bool createSchema(QSqlDatabase &db)
{
    // Same tables as EnergyLogger::initDB()
    QSqlQuery query(db);
    return query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS metadata (version INT);"))
            && query.exec(QStringLiteral("INSERT INTO metadata (version) VALUES (1);"))
            && query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS powerBalance (timestamp BIGINT, sampleRate INT, consumption FLOAT, production FLOAT, acquisition FLOAT, storage FLOAT, totalConsumption FLOAT, totalProduction FLOAT, totalAcquisition FLOAT, totalReturn FLOAT);"))
            && query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_powerBalance ON powerBalance(sampleRate, timestamp);"))
            && query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS thingPower (timestamp BIGINT, sampleRate INT, thingId VARCHAR(38), currentPower FLOAT, totalConsumption FLOAT, totalProduction FLOAT);"))
            && query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_thingPower ON thingPower(thingId, sampleRate, timestamp);"))
            && query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS thingCache (thingId VARCHAR(38) PRIMARY KEY, totalEnergyConsumed FLOAT, totalEnergyProduced FLOAT);"));
}

void insertSeries(QSqlDatabase &db, EnergyLogs::SampleRate sampleRate, const QList<ThingId> &thingIds, const QList<QDateTime> &timestamps)
{
    db.transaction();
    for (int i = 0; i < timestamps.count(); i++) {
        maintenanceInsertPowerBalance(db, timestamps.at(i), sampleRate, 100, 50, 50, 0, i, i / 2.0, i / 2.0, 0);
        foreach (const ThingId &thingId, thingIds) {
            maintenanceInsertThingPower(db, timestamps.at(i), sampleRate, thingId, 100, i, 0);
        }
    }
    db.commit();
}

QList<QDateTime> everyStep(const QDateTime &from, const QDateTime &to, qint64 stepMSecs)
{
    QList<QDateTime> ret;
    for (QDateTime timestamp = from; timestamp <= to; timestamp = timestamp.addMSecs(stepMSecs)) {
        ret.append(timestamp);
    }
    return ret;
}

QList<QDateTime> timestamps(QSqlDatabase &db, const QString &table, EnergyLogs::SampleRate sampleRate, const QDateTime &after, const QString &thingId = QString())
{
    QList<QDateTime> ret;
    QSqlQuery query(db);
    query.prepare(QString("SELECT timestamp FROM %1 WHERE sampleRate = ? AND timestamp > ?%2 ORDER BY timestamp ASC;")
                  .arg(table, thingId.isEmpty() ? QString() : QStringLiteral(" AND thingId = ?")));
    query.addBindValue(sampleRate);
    query.addBindValue(after.toMSecsSinceEpoch());
    if (!thingId.isEmpty()) {
        query.addBindValue(thingId);
    }
    if (!query.exec()) {
        qWarning() << "Query failed:" << query.lastError();
        return ret;
    }
    while (query.next()) {
        ret.append(QDateTime::fromMSecsSinceEpoch(query.value(0).toLongLong()));
    }
    return ret;
}

int duplicateCount(QSqlDatabase &db)
{
    QSqlQuery query(db);
    query.exec(QStringLiteral("SELECT (SELECT COUNT(*) FROM powerBalance) - (SELECT COUNT(*) FROM (SELECT DISTINCT sampleRate, timestamp FROM powerBalance)) "
                              "+ (SELECT COUNT(*) FROM thingPower) - (SELECT COUNT(*) FROM (SELECT DISTINCT thingId, sampleRate, timestamp FROM thingPower));"));
    return query.next() ? query.value(0).toInt() : -1;
}

// Runs a job on its own DB connection in a separate thread, like EnergyLogger::startDbMaintenance().
// Returns false if it did not finish in time. A stuck job is interrupted.
bool runMaintenance(const QString &dbFilePath, std::function<void(QSqlDatabase &db)> job, int timeoutMs)
{
    QThread *thread = QThread::create([dbFilePath, job]() {
        const QString connectionName = QUuid::createUuid().toString();
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(dbFilePath);
            if (db.open()) {
                job(db);
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
    });
    thread->start();
    bool finished = thread->wait(timeoutMs);
    if (!finished) {
        thread->requestInterruption();
        thread->wait();
    }
    delete thread;
    return finished;
}

}

class TestEnergyLogger: public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void fixedRateNext_data();
    void fixedRateNext();

    void threeHoursNext_data();
    void threeHoursNext();

    void calendarNext_data();
    void calendarNext();
    void calendarNextAlwaysAdvances_data();
    void calendarNextAlwaysAdvances();

    void repeatedHour();

    void sampleStart_data();
    void sampleStart();

    void resumeAfterDstEnd();

    void guard();

    void removeDuplicates();
    void removeDuplicatesOnOpen();
};

void TestEnergyLogger::initTestCase()
{
    QCOMPARE(QTimeZone::systemTimeZone().id(), QByteArray("Europe/Paris"));
    QCOMPARE(utc(autumnTransition).addMSecs(-1).offsetFromUtc(), 7200);
    QCOMPARE(utc(autumnTransition).offsetFromUtc(), 3600);
    QCOMPARE(utc(springTransition).addMSecs(-1).offsetFromUtc(), 3600);
    QCOMPARE(utc(springTransition).offsetFromUtc(), 7200);
}

void TestEnergyLogger::fixedRateNext_data()
{
    QTest::addColumn<int>("sampleRate");
    QTest::addColumn<QString>("transition");

    QTest::newRow("1 min, autumn") << (int)EnergyLogs::SampleRate1Min << autumnTransition;
    QTest::newRow("15 min, autumn") << (int)EnergyLogs::SampleRate15Mins << autumnTransition;
    QTest::newRow("1 hour, autumn") << (int)EnergyLogs::SampleRate1Hour << autumnTransition;
    QTest::newRow("1 min, spring") << (int)EnergyLogs::SampleRate1Min << springTransition;
    QTest::newRow("15 min, spring") << (int)EnergyLogs::SampleRate15Mins << springTransition;
    QTest::newRow("1 hour, spring") << (int)EnergyLogs::SampleRate1Hour << springTransition;
}

void TestEnergyLogger::fixedRateNext()
{
    QFETCH(int, sampleRate);
    QFETCH(QString, transition);

    const EnergyLogs::SampleRate rate = static_cast<EnergyLogs::SampleRate>(sampleRate);
    const qint64 step = sampleRate * minuteMSecs;
    const QDateTime from = utc(transition).addSecs(-3 * 3600);
    const QDateTime to = utc(transition).addSecs(3 * 3600);

    // Like EnergyLogger::scheduleNextSample(): next = nextSampleTimestamp(previous + 1 s)
    QDateTime previous = maintenanceNextSampleTimestamp(rate, from);
    while (previous < to) {
        QDateTime next = maintenanceNextSampleTimestamp(rate, previous.addMSecs(1000));
        QVERIFY2(next > previous, qPrintable(describe(previous) + " -> " + describe(next)));
        QVERIFY2(previous.msecsTo(next) == step, qPrintable(describe(previous) + " -> " + describe(next)));
        previous = next;
    }

    // Any point in time: the next sample is in the future, at most one step ahead and aligned
    for (QDateTime dateTime = from; dateTime < to; dateTime = dateTime.addMSecs(7 * minuteMSecs + 13001)) {
        QDateTime next = maintenanceNextSampleTimestamp(rate, dateTime);
        QVERIFY2(next > dateTime, qPrintable(describe(dateTime) + " -> " + describe(next)));
        QVERIFY2(dateTime.msecsTo(next) <= step, qPrintable(describe(dateTime) + " -> " + describe(next)));
        QCOMPARE(next.toMSecsSinceEpoch() % step, 0);
    }
}

void TestEnergyLogger::threeHoursNext_data()
{
    QTest::addColumn<QString>("from");
    QTest::addColumn<QStringList>("expected");

    // Aligned to 00:00, 03:00, 06:00... local time. The sample across the DST change spans 4 hours in autumn and 2 hours in spring.
    QTest::newRow("autumn") << "2026-10-24T12:00:00Z"
                            << (QStringList() << "2026-10-24T13:00:00Z" << "2026-10-24T16:00:00Z" << "2026-10-24T19:00:00Z"
                                << "2026-10-24T22:00:00Z" << "2026-10-25T02:00:00Z" << "2026-10-25T05:00:00Z" << "2026-10-25T08:00:00Z");
    QTest::newRow("spring") << "2026-03-28T12:00:00Z"
                            << (QStringList() << "2026-03-28T14:00:00Z" << "2026-03-28T17:00:00Z" << "2026-03-28T20:00:00Z"
                                << "2026-03-28T23:00:00Z" << "2026-03-29T01:00:00Z" << "2026-03-29T04:00:00Z" << "2026-03-29T07:00:00Z");
}

void TestEnergyLogger::threeHoursNext()
{
    QFETCH(QString, from);
    QFETCH(QStringList, expected);

    QDateTime previous = utc(from);
    for (int i = 0; i < expected.count(); i++) {
        QDateTime next = maintenanceNextSampleTimestamp(EnergyLogs::SampleRate3Hours, i == 0 ? previous : previous.addMSecs(1000));
        QVERIFY2(next == utc(expected.at(i)), qPrintable(describe(previous) + " -> " + describe(next) + ", expected " + describe(utc(expected.at(i)))));
        QCOMPARE(next.toLocalTime().time().hour() % 3, 0);
        QCOMPARE(next.toLocalTime().time().minute(), 0);
        if (i > 0) {
            QVERIFY2(maintenanceCalculateSampleStart(next, EnergyLogs::SampleRate3Hours) == previous,
                     qPrintable("sample start of " + describe(next) + ": " + describe(maintenanceCalculateSampleStart(next, EnergyLogs::SampleRate3Hours))));
        }
        previous = next;
    }
}

void TestEnergyLogger::calendarNext_data()
{
    QTest::addColumn<int>("sampleRate");
    QTest::addColumn<QString>("from");
    QTest::addColumn<QString>("expected");

    QTest::newRow("day, autumn, 25 hours") << (int)EnergyLogs::SampleRate1Day << "2026-10-24T22:00:00Z" << "2026-10-25T23:00:00Z";
    QTest::newRow("day, autumn, 1st 02:30") << (int)EnergyLogs::SampleRate1Day << "2026-10-25T00:30:00Z" << "2026-10-25T23:00:00Z";
    QTest::newRow("day, autumn, 2nd 02:30") << (int)EnergyLogs::SampleRate1Day << "2026-10-25T01:30:00Z" << "2026-10-25T23:00:00Z";
    QTest::newRow("day, spring, 23 hours") << (int)EnergyLogs::SampleRate1Day << "2026-03-28T23:00:00Z" << "2026-03-29T22:00:00Z";
    QTest::newRow("week, autumn") << (int)EnergyLogs::SampleRate1Week << "2026-10-18T22:00:00Z" << "2026-10-25T23:00:00Z";
    QTest::newRow("week, autumn, 2nd 02:30") << (int)EnergyLogs::SampleRate1Week << "2026-10-25T01:30:00Z" << "2026-10-25T23:00:00Z";
    QTest::newRow("week, spring") << (int)EnergyLogs::SampleRate1Week << "2026-03-22T23:00:00Z" << "2026-03-29T22:00:00Z";
    QTest::newRow("month, autumn") << (int)EnergyLogs::SampleRate1Month << "2026-09-30T22:00:00Z" << "2026-10-31T23:00:00Z";
    QTest::newRow("month, autumn, 2nd 02:30") << (int)EnergyLogs::SampleRate1Month << "2026-10-25T01:30:00Z" << "2026-10-31T23:00:00Z";
    QTest::newRow("month, spring") << (int)EnergyLogs::SampleRate1Month << "2026-02-28T23:00:00Z" << "2026-03-31T22:00:00Z";
    QTest::newRow("year, autumn, 2nd 02:30") << (int)EnergyLogs::SampleRate1Year << "2026-10-25T01:30:00Z" << "2026-12-31T23:00:00Z";
    QTest::newRow("year, spring") << (int)EnergyLogs::SampleRate1Year << "2026-03-29T01:30:00Z" << "2026-12-31T23:00:00Z";
}

void TestEnergyLogger::calendarNext()
{
    QFETCH(int, sampleRate);
    QFETCH(QString, from);
    QFETCH(QString, expected);

    QDateTime next = maintenanceNextSampleTimestamp(static_cast<EnergyLogs::SampleRate>(sampleRate), utc(from));
    QVERIFY2(next == utc(expected), qPrintable(describe(utc(from)) + " -> " + describe(next) + ", expected " + describe(utc(expected))));
}

void TestEnergyLogger::calendarNextAlwaysAdvances_data()
{
    QTest::addColumn<int>("sampleRate");
    QTest::addColumn<QString>("transition");

    QList<EnergyLogs::SampleRate> rates = {EnergyLogs::SampleRate1Day, EnergyLogs::SampleRate1Week, EnergyLogs::SampleRate1Month, EnergyLogs::SampleRate1Year};
    foreach (EnergyLogs::SampleRate rate, rates) {
        QTest::newRow(qPrintable(QString("%1, autumn").arg(rate))) << (int)rate << autumnTransition;
        QTest::newRow(qPrintable(QString("%1, spring").arg(rate))) << (int)rate << springTransition;
    }
}

void TestEnergyLogger::calendarNextAlwaysAdvances()
{
    QFETCH(int, sampleRate);
    QFETCH(QString, transition);

    const EnergyLogs::SampleRate rate = static_cast<EnergyLogs::SampleRate>(sampleRate);
    for (QDateTime dateTime = utc(transition).addSecs(-30 * 3600); dateTime < utc(transition).addSecs(30 * 3600); dateTime = dateTime.addMSecs(17 * minuteMSecs)) {
        QDateTime next = maintenanceNextSampleTimestamp(rate, dateTime);
        QVERIFY2(next > dateTime, qPrintable(describe(dateTime) + " -> " + describe(next)));
        QCOMPARE(next.toLocalTime().time(), QTime(0, 0));
        // And the sample before it starts at local midnight as well
        QDateTime start = maintenanceCalculateSampleStart(next, rate);
        QVERIFY(start < next);
        QCOMPARE(start.toLocalTime().time(), QTime(0, 0));
    }
}

void TestEnergyLogger::repeatedHour()
{
    // What the logger computes in the hour that exists twice. 01:00:01Z is 02:00:01 CET, the second 02:00.
    QCOMPARE(maintenanceNextSampleTimestamp(EnergyLogs::SampleRate1Min, utc("2026-10-25T01:00:01Z")), utc("2026-10-25T01:01:00Z"));
    QCOMPARE(maintenanceNextSampleTimestamp(EnergyLogs::SampleRate15Mins, utc("2026-10-25T01:00:01Z")), utc("2026-10-25T01:15:00Z"));
    QCOMPARE(maintenanceNextSampleTimestamp(EnergyLogs::SampleRate1Hour, utc("2026-10-25T01:00:01Z")), utc("2026-10-25T02:00:00Z"));
    QCOMPARE(maintenanceNextSampleTimestamp(EnergyLogs::SampleRate3Hours, utc("2026-10-25T01:00:01Z")), utc("2026-10-25T02:00:00Z"));
    // The first 02:00 (CEST)
    QCOMPARE(maintenanceNextSampleTimestamp(EnergyLogs::SampleRate1Min, utc("2026-10-25T00:00:01Z")), utc("2026-10-25T00:01:00Z"));
    QCOMPARE(maintenanceNextSampleTimestamp(EnergyLogs::SampleRate15Mins, utc("2026-10-25T00:00:01Z")), utc("2026-10-25T00:15:00Z"));
    QCOMPARE(maintenanceNextSampleTimestamp(EnergyLogs::SampleRate1Hour, utc("2026-10-25T00:00:01Z")), utc("2026-10-25T01:00:00Z"));
    QCOMPARE(maintenanceNextSampleTimestamp(EnergyLogs::SampleRate3Hours, utc("2026-10-25T00:00:01Z")), utc("2026-10-25T02:00:00Z"));
}

void TestEnergyLogger::sampleStart_data()
{
    QTest::addColumn<int>("sampleRate");
    QTest::addColumn<QString>("sampleEnd");
    QTest::addColumn<QString>("expected");

    QTest::newRow("15 min, autumn") << (int)EnergyLogs::SampleRate15Mins << "2026-10-25T01:00:00Z" << "2026-10-25T00:45:00Z";
    QTest::newRow("1 hour, autumn, 1st 02:00") << (int)EnergyLogs::SampleRate1Hour << "2026-10-25T01:00:00Z" << "2026-10-25T00:00:00Z";
    QTest::newRow("1 hour, autumn, 2nd 02:00") << (int)EnergyLogs::SampleRate1Hour << "2026-10-25T02:00:00Z" << "2026-10-25T01:00:00Z";
    QTest::newRow("1 hour, spring") << (int)EnergyLogs::SampleRate1Hour << "2026-03-29T01:00:00Z" << "2026-03-29T00:00:00Z";
    QTest::newRow("3 hours, autumn, 4 hours") << (int)EnergyLogs::SampleRate3Hours << "2026-10-25T02:00:00Z" << "2026-10-24T22:00:00Z";
    QTest::newRow("3 hours, spring, 2 hours") << (int)EnergyLogs::SampleRate3Hours << "2026-03-29T01:00:00Z" << "2026-03-28T23:00:00Z";
    QTest::newRow("day, autumn, 25 hours") << (int)EnergyLogs::SampleRate1Day << "2026-10-25T23:00:00Z" << "2026-10-24T22:00:00Z";
    QTest::newRow("day, spring, 23 hours") << (int)EnergyLogs::SampleRate1Day << "2026-03-29T22:00:00Z" << "2026-03-28T23:00:00Z";
    QTest::newRow("week, autumn") << (int)EnergyLogs::SampleRate1Week << "2026-10-25T23:00:00Z" << "2026-10-18T22:00:00Z";
    QTest::newRow("week, spring") << (int)EnergyLogs::SampleRate1Week << "2026-03-29T22:00:00Z" << "2026-03-22T23:00:00Z";
    QTest::newRow("month, autumn") << (int)EnergyLogs::SampleRate1Month << "2026-10-31T23:00:00Z" << "2026-09-30T22:00:00Z";
    QTest::newRow("month, spring") << (int)EnergyLogs::SampleRate1Month << "2026-03-31T22:00:00Z" << "2026-02-28T23:00:00Z";
}

void TestEnergyLogger::sampleStart()
{
    QFETCH(int, sampleRate);
    QFETCH(QString, sampleEnd);
    QFETCH(QString, expected);

    QDateTime start = maintenanceCalculateSampleStart(utc(sampleEnd), static_cast<EnergyLogs::SampleRate>(sampleRate));
    QVERIFY2(start == utc(expected), qPrintable(describe(utc(sampleEnd)) + " starts at " + describe(start) + ", expected " + describe(utc(expected))));
}

void TestEnergyLogger::resumeAfterDstEnd()
{
    // A box whose newest samples were written at 2026-10-25 01:00Z (03:00 CEST, the DST change) and which
    // resumes at 06:31:30Z, like after the update. The resampling must finish and fill the gap once.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbFilePath = dir.filePath("energylogs.sqlite");
    const QList<ThingId> thingIds = {ThingId::createThingId(), ThingId::createThingId()};
    const QDateTime lastSample = utc("2026-10-25T01:00:00Z");
    const QDateTime historyStart = utc("2026-10-24T01:00:00Z");
    const QDateTime now = utc("2026-10-25T06:31:30Z");

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), "setup");
        db.setDatabaseName(dbFilePath);
        QVERIFY(db.open());
        QVERIFY(createSchema(db));
        insertSeries(db, EnergyLogs::SampleRate1Min, thingIds, everyStep(historyStart, lastSample, minuteMSecs));
        insertSeries(db, EnergyLogs::SampleRate15Mins, thingIds, everyStep(historyStart, lastSample, 15 * minuteMSecs));
        insertSeries(db, EnergyLogs::SampleRate1Hour, thingIds, everyStep(historyStart, lastSample, 60 * minuteMSecs));
        insertSeries(db, EnergyLogs::SampleRate3Hours, thingIds, everyStep(historyStart, utc("2026-10-24T22:00:00Z"), 180 * minuteMSecs));
        insertSeries(db, EnergyLogs::SampleRate1Day, thingIds, {utc("2026-10-23T22:00:00Z"), utc("2026-10-24T22:00:00Z")});
        insertSeries(db, EnergyLogs::SampleRate1Week, thingIds, {utc("2026-10-18T22:00:00Z")});
        insertSeries(db, EnergyLogs::SampleRate1Month, thingIds, {utc("2026-09-30T22:00:00Z")});
        insertSeries(db, EnergyLogs::SampleRate1Year, thingIds, {utc("2025-12-31T23:00:00Z")});
        db.close();
    }
    QSqlDatabase::removeDatabase("setup");

    // The configuration of EnergyLogger::EnergyLogger()
    struct Config { EnergyLogs::SampleRate sampleRate; EnergyLogs::SampleRate baseSampleRate; uint maxSamples; };
    const QList<Config> configs = {
        {EnergyLogs::SampleRate15Mins, EnergyLogs::SampleRate1Min, 16128},
        {EnergyLogs::SampleRate1Hour, EnergyLogs::SampleRate15Mins, 8760},
        {EnergyLogs::SampleRate3Hours, EnergyLogs::SampleRate15Mins, 2920},
        {EnergyLogs::SampleRate1Day, EnergyLogs::SampleRate1Hour, 1095},
        {EnergyLogs::SampleRate1Week, EnergyLogs::SampleRate1Day, 168},
        {EnergyLogs::SampleRate1Month, EnergyLogs::SampleRate1Day, 240},
        {EnergyLogs::SampleRate1Year, EnergyLogs::SampleRate1Month, 20}
    };

    QElapsedTimer timer;
    timer.start();
    bool finished = runMaintenance(dbFilePath, [=](QSqlDatabase &db) {
        // Like the "startup resampling" job
        QDateTime fillUntil = maintenanceCalculateSampleStart(maintenanceNextSampleTimestamp(EnergyLogs::SampleRate1Min, now), EnergyLogs::SampleRate1Min);
        maintenanceFillMissingMinuteSamples(db, 10080, fillUntil);
        foreach (const Config &config, configs) {
            if (QThread::currentThread()->isInterruptionRequested()) {
                return;
            }
            maintenanceRectifySamples(db, config.sampleRate, config.baseSampleRate, config.maxSamples, maintenanceNextSampleTimestamp(config.sampleRate, now), thingIds);
        }
    }, 20000);
    QVERIFY2(finished, "The resampling did not finish within 20 s");
    qDebug() << "Resampling finished in" << timer.elapsed() << "ms";

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), "check");
    db.setDatabaseName(dbFilePath);
    QVERIFY(db.open());

    QCOMPARE(duplicateCount(db), 0);

    // The fixed rates continue right after the last sample, without a gap, up to the sample before the next scheduled one.
    QList<QPair<EnergyLogs::SampleRate, QDateTime>> expectedLast = {
        {EnergyLogs::SampleRate1Min, utc("2026-10-25T06:31:00Z")},
        {EnergyLogs::SampleRate15Mins, utc("2026-10-25T06:30:00Z")},
        {EnergyLogs::SampleRate1Hour, utc("2026-10-25T06:00:00Z")}
    };
    for (int i = 0; i < expectedLast.count(); i++) {
        const EnergyLogs::SampleRate rate = expectedLast.at(i).first;
        QList<QDateTime> expected = everyStep(lastSample.addMSecs(rate * minuteMSecs), expectedLast.at(i).second, rate * minuteMSecs);
        QCOMPARE(timestamps(db, "powerBalance", rate, lastSample), expected);
        foreach (const ThingId &thingId, thingIds) {
            QCOMPARE(timestamps(db, "thingPower", rate, lastSample, thingId.toString()), expected);
        }
    }

    // 3 hours: 03:00 CET and 06:00 CET
    QList<QDateTime> expected3Hours = {utc("2026-10-25T02:00:00Z"), utc("2026-10-25T05:00:00Z")};
    QCOMPARE(timestamps(db, "powerBalance", EnergyLogs::SampleRate3Hours, utc("2026-10-24T22:00:00Z")), expected3Hours);
    foreach (const ThingId &thingId, thingIds) {
        QCOMPARE(timestamps(db, "thingPower", EnergyLogs::SampleRate3Hours, utc("2026-10-24T22:00:00Z"), thingId.toString()), expected3Hours);
    }

    // The day of the 25th ends at 26.10. 00:00 CET, not yet due
    QCOMPARE(timestamps(db, "powerBalance", EnergyLogs::SampleRate1Day, utc("2026-10-24T22:00:00Z")), QList<QDateTime>());

    db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase("check");
}

void TestEnergyLogger::guard()
{
#ifdef ENERGYLOGGER_LEGACY
    QSKIP("This version has no guard in the resampling loops.");
#else
    const QDateTime now = utc("2026-10-25T06:31:30Z");
    QVERIFY(maintenanceSampleAdvances(EnergyLogs::SampleRate1Min, now, now.addSecs(60)));

    s_warnings.clear();
    s_previousHandler = qInstallMessageHandler(recordingMessageHandler);

    QVERIFY(!maintenanceSampleAdvances(EnergyLogs::SampleRate1Min, now, now));
    QVERIFY(!maintenanceSampleAdvances(EnergyLogs::SampleRate1Min, now, now.addSecs(-3600)));
    QVERIFY(!maintenanceSampleAdvances(EnergyLogs::SampleRate1Min, now, QDateTime()));
    QCOMPARE(countWarnings("does not advance"), 3);

    // A resampling loop whose next sample does not advance (no timestamp can be computed for SampleRateAny)
    // must stop at once, with a single warning, and write nothing.
    s_warnings.clear();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbFilePath = dir.filePath("energylogs.sqlite");
    const QList<ThingId> thingIds = {ThingId::createThingId(), ThingId::createThingId()};
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), "setup");
        db.setDatabaseName(dbFilePath);
        QVERIFY(db.open());
        QVERIFY(createSchema(db));
        insertSeries(db, EnergyLogs::SampleRate1Min, thingIds, everyStep(now.addSecs(-3600), now.addSecs(-90), minuteMSecs));
        db.close();
    }
    QSqlDatabase::removeDatabase("setup");

    bool finished = runMaintenance(dbFilePath, [=](QSqlDatabase &db) {
        maintenanceRectifySamples(db, EnergyLogs::SampleRateAny, EnergyLogs::SampleRate1Min, 100, now, thingIds);
    }, 10000);
    qInstallMessageHandler(s_previousHandler);

    QVERIFY2(finished, "The resampling loop did not stop");
    QCOMPARE(countWarnings("does not advance"), 1);

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), "check");
    db.setDatabaseName(dbFilePath);
    QVERIFY(db.open());
    QSqlQuery query(db);
    QVERIFY(query.exec("SELECT (SELECT COUNT(*) FROM powerBalance WHERE sampleRate = 0) + (SELECT COUNT(*) FROM thingPower WHERE sampleRate = 0);"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 0);
    db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase("check");
#endif
}

void TestEnergyLogger::removeDuplicates()
{
#ifdef ENERGYLOGGER_LEGACY
    QSKIP("This version has no duplicate cleanup.");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), "dedup");
    db.setDatabaseName(dir.filePath("energylogs.sqlite"));
    QVERIFY(db.open());
    QVERIFY(createSchema(db));

    const ThingId thingA = ThingId::createThingId();
    const ThingId thingB = ThingId::createThingId();
    const QDateTime t1 = utc("2026-10-25T00:01:00Z");
    const QDateTime t2 = utc("2026-10-25T00:02:00Z");
    const QDateTime t3 = utc("2026-10-25T01:00:00Z");

    // Unique samples: same timestamp in different sample rates, and same timestamp for different things
    maintenanceInsertPowerBalance(db, t1, EnergyLogs::SampleRate1Min, 1, 0, 0, 0, 10, 0, 0, 0);
    maintenanceInsertPowerBalance(db, t3, EnergyLogs::SampleRate1Hour, 3, 0, 0, 0, 30, 0, 0, 0);
    maintenanceInsertPowerBalance(db, t3, EnergyLogs::SampleRate15Mins, 4, 0, 0, 0, 40, 0, 0, 0);
    maintenanceInsertThingPower(db, t1, EnergyLogs::SampleRate1Min, thingA, 1, 10, 0);
    maintenanceInsertThingPower(db, t1, EnergyLogs::SampleRate1Min, thingB, 2, 20, 0);
    // Duplicates, written later
    maintenanceInsertPowerBalance(db, t2, EnergyLogs::SampleRate1Min, 2, 0, 0, 0, 20, 0, 0, 0);
    maintenanceInsertPowerBalance(db, t2, EnergyLogs::SampleRate1Min, 99, 0, 0, 0, 99, 0, 0, 0);
    maintenanceInsertPowerBalance(db, t2, EnergyLogs::SampleRate1Min, 98, 0, 0, 0, 98, 0, 0, 0);
    maintenanceInsertPowerBalance(db, t3, EnergyLogs::SampleRate1Hour, 97, 0, 0, 0, 97, 0, 0, 0);
    maintenanceInsertThingPower(db, t1, EnergyLogs::SampleRate1Min, thingA, 99, 99, 0);
    maintenanceInsertThingPower(db, t1, EnergyLogs::SampleRate1Min, thingA, 98, 98, 0);

    auto dump = [&db](const QString &queryString) {
        QStringList ret;
        QSqlQuery query(db);
        query.exec(queryString);
        while (query.next()) {
            QStringList row;
            for (int i = 0; i < query.record().count(); i++) {
                row.append(query.value(i).toString());
            }
            ret.append(row.join(","));
        }
        return ret;
    };

    QCOMPARE(duplicateCount(db), 5);
    QCOMPARE(removeDuplicateSamples(db), 5);
    QCOMPARE(duplicateCount(db), 0);

    // The first written row of each sample is kept, all unique rows are untouched.
    QCOMPARE(dump("SELECT timestamp, sampleRate, consumption, totalConsumption FROM powerBalance ORDER BY rowid;"),
             QStringList() << QString("%1,1,1,10").arg(t1.toMSecsSinceEpoch()) << QString("%1,60,3,30").arg(t3.toMSecsSinceEpoch())
             << QString("%1,15,4,40").arg(t3.toMSecsSinceEpoch()) << QString("%1,1,2,20").arg(t2.toMSecsSinceEpoch()));
    QCOMPARE(dump("SELECT timestamp, sampleRate, thingId, currentPower FROM thingPower ORDER BY rowid;"),
             QStringList() << QString("%1,1,%2,1").arg(t1.toMSecsSinceEpoch()).arg(thingA.toString())
             << QString("%1,1,%2,2").arg(t1.toMSecsSinceEpoch()).arg(thingB.toString()));

    // Nothing left to do on the next run
    QCOMPARE(removeDuplicateSamples(db), 0);

    db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase("dedup");
#endif
}

void TestEnergyLogger::removeDuplicatesOnOpen()
{
    // Open a database with duplicates through EnergyLogger itself. NymeaSettings::storagePath() is
    // $HOME/.local/share/<organization name> for non-root users.
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QByteArray previousHome = qgetenv("HOME");
    qputenv("HOME", home.path().toUtf8());
    const QString dbFilePath = QDir(NymeaSettings::storagePath()).filePath("energylogs.sqlite");
    if (!dbFilePath.startsWith(home.path())) {
        // As root, the storage path is the system one: never touch it from a test.
        qputenv("HOME", previousHome);
        QSKIP("NymeaSettings::storagePath() is not under $HOME (running as root)");
    }
    QDir().mkpath(NymeaSettings::storagePath());

    const ThingId thingId = ThingId::createThingId();
    const QDateTime newest = maintenanceCalculateSampleStart(maintenanceNextSampleTimestamp(EnergyLogs::SampleRate1Min, QDateTime::currentDateTime()), EnergyLogs::SampleRate1Min);
    QList<QDateTime> samples;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), "setup");
        db.setDatabaseName(dbFilePath);
        QVERIFY(db.open());
        QVERIFY(createSchema(db));
        samples = everyStep(newest.addSecs(-600), newest, minuteMSecs);
        insertSeries(db, EnergyLogs::SampleRate1Min, {thingId}, samples);
        insertSeries(db, EnergyLogs::SampleRate1Min, {thingId}, samples.mid(2, 5));
        QCOMPARE(duplicateCount(db), 10);
        db.close();
    }
    QSqlDatabase::removeDatabase("setup");

    {
        EnergyLogger logger;
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), "check");
        db.setDatabaseName(dbFilePath);
        QVERIFY(db.open());
        QCOMPARE(duplicateCount(db), 0);
        QCOMPARE(timestamps(db, "powerBalance", EnergyLogs::SampleRate1Min, newest.addSecs(-601)).mid(0, samples.count()), samples);
        QCOMPARE(timestamps(db, "thingPower", EnergyLogs::SampleRate1Min, newest.addSecs(-601), thingId.toString()).mid(0, samples.count()), samples);
        db.close();
    }
    QSqlDatabase::removeDatabase("check");
    qputenv("HOME", previousHome);
}

int main(int argc, char *argv[])
{
    // The DST tests are written for Europe/Paris. Set it before anything reads the local time zone.
    qputenv("TZ", "Europe/Paris");
    tzset();

    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("energylogger-test");
    TestEnergyLogger test;
    return QTest::qExec(&test, argc, argv);
}

#include "testenergylogger.moc"
