// SPDX-License-Identifier: GPL-3.0-or-later

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
*
* Copyright (C) 2013 - 2024, nymea GmbH
* Copyright (C) 2024 - 2025, chargebyte austria GmbH
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

#include "energylogger.h"

#include <nymeasettings.h>

#include <QStandardPaths>
#include <QDir>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QSqlIndex>
#include <QSettings>
#include <QElapsedTimer>
#include <QPointer>
#include <QUuid>
#include <QTimeZone>

#include <QLoggingCategory>
Q_DECLARE_LOGGING_CATEGORY(dcEnergyExperience)

namespace {

struct MaintenanceConfig {
    EnergyLogs::SampleRate sampleRate = EnergyLogs::SampleRateAny;
    EnergyLogs::SampleRate baseSampleRate = EnergyLogs::SampleRateAny;
    uint maxSamples = 0;
};

struct BalanceTotals {
    double totalConsumption = 0;
    double totalProduction = 0;
    double totalAcquisition = 0;
    double totalReturn = 0;
};

struct ThingTotals {
    double totalConsumption = 0;
    double totalProduction = 0;
};

qint64 floorDiv(qint64 value, qint64 divisor)
{
    qint64 result = value / divisor;
    if (value % divisor != 0 && value < 0) {
        result--;
    }
    return result;
}

// Sample timestamps of the fixed length sample rates (1 min, 15 mins, 1 hour, 3 hours) are aligned to the
// local wall clock (e.g. 3 hour samples at 00:00, 03:00, 06:00... local time), but they are calculated on
// absolute time (msecs since epoch) and the UTC offset in effect. Calculating them with wall clock arithmetic
// (QDateTime(date, time).addMSecs()) is ambiguous in the hour repeated when DST ends: Qt 6 resolves it to the
// first occurrence, so the "next" sample ends up one hour in the past and the sampling loops never finish.

// Returns the first point in time strictly after msecs at which the local wall clock shows a multiple of stepMSecs.
qint64 nextAlignedMSecs(qint64 msecs, qint64 stepMSecs)
{
    const QTimeZone timeZone = QTimeZone::systemTimeZone();
    qint64 from = msecs + 1;
    forever {
        const QDateTime fromDateTime = QDateTime::fromMSecsSinceEpoch(from);
        const qint64 offset = timeZone.isValid() ? timeZone.offsetFromUtc(fromDateTime) * 1000LL : 0;
        const qint64 candidate = (floorDiv(from + offset - 1, stepMSecs) + 1) * stepMSecs - offset;
        if (!timeZone.isValid() || !timeZone.hasTransitions()) {
            return candidate;
        }
        // The offset is valid until the next transition. If the candidate is not before it, start over from there.
        const QDateTime transition = timeZone.nextTransition(fromDateTime).atUtc;
        if (!transition.isValid() || transition.toMSecsSinceEpoch() > candidate) {
            return candidate;
        }
        from = transition.toMSecsSinceEpoch();
    }
}

// Returns the last point in time strictly before msecs at which the local wall clock shows a multiple of stepMSecs.
qint64 previousAlignedMSecs(qint64 msecs, qint64 stepMSecs)
{
    const QTimeZone timeZone = QTimeZone::systemTimeZone();
    qint64 to = msecs - 1;
    forever {
        const QDateTime toDateTime = QDateTime::fromMSecsSinceEpoch(to);
        const qint64 offset = timeZone.isValid() ? timeZone.offsetFromUtc(toDateTime) * 1000LL : 0;
        const qint64 candidate = floorDiv(to + offset, stepMSecs) * stepMSecs - offset;
        if (!timeZone.isValid() || !timeZone.hasTransitions()) {
            return candidate;
        }
        // The offset is valid since the previous transition. If the candidate is before it, start over from there.
        const QDateTime transition = timeZone.previousTransition(QDateTime::fromMSecsSinceEpoch(to + 1)).atUtc;
        if (!transition.isValid() || transition.toMSecsSinceEpoch() <= candidate) {
            return candidate;
        }
        to = transition.toMSecsSinceEpoch() - 1;
    }
}

QDateTime maintenanceNextSampleTimestamp(EnergyLogs::SampleRate sampleRate, const QDateTime &dateTime)
{
    QDate date = dateTime.toLocalTime().date();
    switch (sampleRate) {
    case EnergyLogs::SampleRateAny:
        return QDateTime();
    case EnergyLogs::SampleRate1Min:
    case EnergyLogs::SampleRate15Mins:
    case EnergyLogs::SampleRate1Hour:
    case EnergyLogs::SampleRate3Hours:
        return QDateTime::fromMSecsSinceEpoch(nextAlignedMSecs(dateTime.toMSecsSinceEpoch(), sampleRate * 60 * 1000LL));
    case EnergyLogs::SampleRate1Day:
        break;
    case EnergyLogs::SampleRate1Week:
        date = date.addDays(-date.dayOfWeek() + 1);
        break;
    case EnergyLogs::SampleRate1Month:
        date = date.addDays(-date.day() + 1);
        break;
    case EnergyLogs::SampleRate1Year:
        date.setDate(date.year(), 1, 1);
        break;
    }

    // Days, weeks, months and years start at local midnight. Those days may be 23 or 25 hours long,
    // so step on the calendar and make sure we always move forward.
    QDateTime next;
    do {
        switch (sampleRate) {
        case EnergyLogs::SampleRate1Week:
            date = date.addDays(7);
            break;
        case EnergyLogs::SampleRate1Month:
            date = date.addMonths(1);
            break;
        case EnergyLogs::SampleRate1Year:
            date = date.addYears(1);
            break;
        default:
            date = date.addDays(1);
            break;
        }
        next = date.startOfDay();
    } while (next <= dateTime);

    return next;
}

QDateTime maintenanceCalculateSampleStart(const QDateTime &sampleEnd, EnergyLogs::SampleRate sampleRate, int sampleCount = 1)
{
    switch (sampleRate) {
    case EnergyLogs::SampleRateAny:
        return sampleEnd;
    case EnergyLogs::SampleRate1Day:
        return sampleEnd.toLocalTime().addDays(-sampleCount);
    case EnergyLogs::SampleRate1Week:
        return sampleEnd.toLocalTime().addDays(-7 * sampleCount);
    case EnergyLogs::SampleRate1Month:
        return sampleEnd.toLocalTime().addMonths(-sampleCount);
    case EnergyLogs::SampleRate1Year:
        return sampleEnd.toLocalTime().addYears(-sampleCount);
    default:
        break;
    }
    if (sampleCount == 1) {
        // A 3 hour sample spans 2 or 4 hours when the DST changes, start it at the previous aligned sample.
        return QDateTime::fromMSecsSinceEpoch(previousAlignedMSecs(sampleEnd.toMSecsSinceEpoch(), sampleRate * 60 * 1000LL));
    }
    return sampleEnd.addMSecs(-(qint64)sampleCount * sampleRate * 60 * 1000);
}

// Guard for the resampling loops: they must never get stuck if a sample timestamp does not move forward.
bool maintenanceSampleAdvances(EnergyLogs::SampleRate sampleRate, const QDateTime &current, const QDateTime &next)
{
    if (next.isValid() && next > current) {
        return true;
    }
    qCWarning(dcEnergyExperience()) << "Next sample timestamp" << next.toString(Qt::ISODate) << "for sample rate" << sampleRate
                                    << "does not advance past" << current.toString(Qt::ISODate) << ". Stopping resampling of this series.";
    return false;
}

// Older versions could write the same sample more than once (e.g. when the DST ended).
// Keep the first written row of each sample and drop the others. Returns the number of removed rows, or -1 on error.
int removeDuplicateSamples(QSqlDatabase &db)
{
    QSqlQuery checkQuery(db);
    if (!checkQuery.exec(QStringLiteral("SELECT 1 FROM powerBalance GROUP BY sampleRate, timestamp HAVING COUNT(*) > 1 LIMIT 1;"))) {
        qCWarning(dcEnergyExperience()) << "Failed to check powerBalance for duplicate samples:" << checkQuery.lastError();
        return -1;
    }
    bool hasDuplicates = checkQuery.next();
    if (!checkQuery.exec(QStringLiteral("SELECT 1 FROM thingPower GROUP BY thingId, sampleRate, timestamp HAVING COUNT(*) > 1 LIMIT 1;"))) {
        qCWarning(dcEnergyExperience()) << "Failed to check thingPower for duplicate samples:" << checkQuery.lastError();
        return -1;
    }
    hasDuplicates |= checkQuery.next();
    if (!hasDuplicates) {
        return 0;
    }

    db.transaction();
    QSqlQuery powerBalanceQuery(db);
    if (!powerBalanceQuery.exec(QStringLiteral("DELETE FROM powerBalance WHERE rowid NOT IN (SELECT MIN(rowid) FROM powerBalance GROUP BY sampleRate, timestamp);"))) {
        qCWarning(dcEnergyExperience()) << "Failed to remove duplicate powerBalance samples:" << powerBalanceQuery.lastError();
        db.rollback();
        return -1;
    }
    // Note: numRowsAffected() reports the last statement on the connection, read it right away.
    const int powerBalanceCount = powerBalanceQuery.numRowsAffected();
    QSqlQuery thingPowerQuery(db);
    if (!thingPowerQuery.exec(QStringLiteral("DELETE FROM thingPower WHERE rowid NOT IN (SELECT MIN(rowid) FROM thingPower GROUP BY thingId, sampleRate, timestamp);"))) {
        qCWarning(dcEnergyExperience()) << "Failed to remove duplicate thingPower samples:" << thingPowerQuery.lastError();
        db.rollback();
        return -1;
    }
    const int thingPowerCount = thingPowerQuery.numRowsAffected();
    if (!db.commit()) {
        qCWarning(dcEnergyExperience()) << "Failed to commit removal of duplicate samples:" << db.lastError();
        db.rollback();
        return -1;
    }

    qCWarning(dcEnergyExperience()).nospace() << "Removed " << powerBalanceCount << " duplicate powerBalance and "
                                              << thingPowerCount << " duplicate thingPower samples from the energy log.";
    return powerBalanceCount + thingPowerCount;
}

QList<ThingId> maintenanceLoggedThings(QSqlDatabase &db)
{
    QList<ThingId> ret;

    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT DISTINCT thingId FROM thingPower;"));
    if (!query.exec()) {
        qCWarning(dcEnergyExperience()) << "Failed to load existing things from logs:" << query.lastError();
        return ret;
    }
    while (query.next()) {
        ret.append(query.value("thingId").toUuid());
    }
    return ret;
}

QDateTime maintenanceGetOldestPowerBalanceSampleTimestamp(QSqlDatabase &db, EnergyLogs::SampleRate sampleRate)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT MIN(timestamp) AS oldestTimestamp FROM powerBalance WHERE sampleRate = ?;"));
    query.addBindValue(sampleRate);
    if (!query.exec()) {
        qCWarning(dcEnergyExperience()) << "Failed to query oldest powerBalance timestamp:" << query.lastError();
        return QDateTime();
    }
    if (query.next() && !query.value("oldestTimestamp").isNull()) {
        return QDateTime::fromMSecsSinceEpoch(query.value("oldestTimestamp").toLongLong());
    }
    return QDateTime();
}

QDateTime maintenanceGetNewestPowerBalanceSampleTimestamp(QSqlDatabase &db, EnergyLogs::SampleRate sampleRate)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT MAX(timestamp) AS newestTimestamp FROM powerBalance WHERE sampleRate = ?;"));
    query.addBindValue(sampleRate);
    if (!query.exec()) {
        qCWarning(dcEnergyExperience()) << "Failed to query newest powerBalance timestamp:" << query.lastError();
        return QDateTime();
    }
    if (query.next() && !query.value("newestTimestamp").isNull()) {
        return QDateTime::fromMSecsSinceEpoch(query.value("newestTimestamp").toLongLong());
    }
    return QDateTime();
}

QDateTime maintenanceGetOldestThingPowerSampleTimestamp(QSqlDatabase &db, const ThingId &thingId, EnergyLogs::SampleRate sampleRate)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT MIN(timestamp) AS oldestTimestamp FROM thingPower WHERE thingId = ? AND sampleRate = ?;"));
    query.addBindValue(thingId);
    query.addBindValue(sampleRate);
    if (!query.exec()) {
        qCWarning(dcEnergyExperience()) << "Failed to query oldest thingPower timestamp:" << thingId << query.lastError();
        return QDateTime();
    }
    if (query.next() && !query.value("oldestTimestamp").isNull()) {
        return QDateTime::fromMSecsSinceEpoch(query.value("oldestTimestamp").toLongLong());
    }
    return QDateTime();
}

QDateTime maintenanceGetNewestThingPowerSampleTimestamp(QSqlDatabase &db, const ThingId &thingId, EnergyLogs::SampleRate sampleRate)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT MAX(timestamp) AS newestTimestamp FROM thingPower WHERE thingId = ? AND sampleRate = ?;"));
    query.addBindValue(thingId);
    query.addBindValue(sampleRate);
    if (!query.exec()) {
        qCWarning(dcEnergyExperience()) << "Failed to query newest thingPower timestamp:" << thingId << query.lastError();
        return QDateTime();
    }
    if (query.next() && !query.value("newestTimestamp").isNull()) {
        return QDateTime::fromMSecsSinceEpoch(query.value("newestTimestamp").toLongLong());
    }
    return QDateTime();
}

BalanceTotals maintenanceLatestPowerBalanceTotals(QSqlDatabase &db, EnergyLogs::SampleRate sampleRate)
{
    BalanceTotals totals;
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT totalConsumption, totalProduction, totalAcquisition, totalReturn FROM powerBalance WHERE sampleRate = ? ORDER BY timestamp DESC LIMIT 1;"));
    query.addBindValue(sampleRate);
    if (!query.exec()) {
        qCWarning(dcEnergyExperience()) << "Failed to query latest powerBalance totals:" << sampleRate << query.lastError();
        return totals;
    }
    if (!query.next()) {
        return totals;
    }
    totals.totalConsumption = query.value("totalConsumption").toDouble();
    totals.totalProduction = query.value("totalProduction").toDouble();
    totals.totalAcquisition = query.value("totalAcquisition").toDouble();
    totals.totalReturn = query.value("totalReturn").toDouble();
    return totals;
}

ThingTotals maintenanceLatestThingTotals(QSqlDatabase &db, EnergyLogs::SampleRate sampleRate, const ThingId &thingId)
{
    ThingTotals totals;
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT totalConsumption, totalProduction FROM thingPower WHERE thingId = ? AND sampleRate = ? ORDER BY timestamp DESC LIMIT 1;"));
    query.addBindValue(thingId);
    query.addBindValue(sampleRate);
    if (!query.exec()) {
        qCWarning(dcEnergyExperience()) << "Failed to query latest thingPower totals:" << sampleRate << thingId << query.lastError();
        return totals;
    }
    if (!query.next()) {
        return totals;
    }
    totals.totalConsumption = query.value("totalConsumption").toDouble();
    totals.totalProduction = query.value("totalProduction").toDouble();
    return totals;
}

bool maintenanceInsertPowerBalance(QSqlDatabase &db, const QDateTime &timestamp, EnergyLogs::SampleRate sampleRate,
                                  double consumption, double production, double acquisition, double storage,
                                  double totalConsumption, double totalProduction, double totalAcquisition, double totalReturn)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("INSERT INTO powerBalance (timestamp, sampleRate, consumption, production, acquisition, storage, totalConsumption, totalProduction, totalAcquisition, totalReturn) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"));
    query.addBindValue(timestamp.toMSecsSinceEpoch());
    query.addBindValue(sampleRate);
    query.addBindValue(consumption);
    query.addBindValue(production);
    query.addBindValue(acquisition);
    query.addBindValue(storage);
    query.addBindValue(totalConsumption);
    query.addBindValue(totalProduction);
    query.addBindValue(totalAcquisition);
    query.addBindValue(totalReturn);
    if (!query.exec()) {
        qCWarning(dcEnergyExperience()) << "Error logging power balance sample:" << query.lastError() << query.executedQuery();
        return false;
    }
    return true;
}

bool maintenanceInsertThingPower(QSqlDatabase &db, const QDateTime &timestamp, EnergyLogs::SampleRate sampleRate, const ThingId &thingId,
                                 double currentPower, double totalConsumption, double totalProduction)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("INSERT INTO thingPower (timestamp, sampleRate, thingId, currentPower, totalConsumption, totalProduction) values (?, ?, ?, ?, ?, ?);"));
    query.addBindValue(timestamp.toMSecsSinceEpoch());
    query.addBindValue(sampleRate);
    query.addBindValue(thingId);
    query.addBindValue(currentPower);
    query.addBindValue(totalConsumption);
    query.addBindValue(totalProduction);
    if (!query.exec()) {
        qCWarning(dcEnergyExperience()) << "Error logging thing power sample:" << query.lastError() << query.executedQuery();
        return false;
    }
    return true;
}

bool maintenanceSamplePowerBalance(QSqlDatabase &db, EnergyLogs::SampleRate sampleRate, EnergyLogs::SampleRate baseSampleRate, const QDateTime &sampleEnd)
{
    QDateTime sampleStart = maintenanceCalculateSampleStart(sampleEnd, sampleRate);

    double medianConsumption = 0;
    double medianProduction = 0;
    double medianAcquisition = 0;
    double medianStorage = 0;
    double totalConsumption = 0;
    double totalProduction = 0;
    double totalAcquisition = 0;
    double totalReturn = 0;

    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT * FROM powerBalance WHERE sampleRate = ? AND timestamp > ? AND timestamp <= ? ORDER BY timestamp ASC;"));
    query.addBindValue(baseSampleRate);
    query.addBindValue(sampleStart.toMSecsSinceEpoch());
    query.addBindValue(sampleEnd.toMSecsSinceEpoch());
    if (!query.exec()) {
        qCWarning(dcEnergyExperience()) << "Error fetching power balance samples for" << baseSampleRate << "from" << sampleStart.toString() << "to" << sampleEnd.toString();
        qCWarning(dcEnergyExperience()) << "SQL error was:" << query.lastError() << "executed query:" << query.executedQuery();
        return false;
    }

    int resultCount = 0;
    while (query.next()) {
        if (QThread::currentThread()->isInterruptionRequested()) {
            return false;
        }
        resultCount++;
        medianConsumption += query.value("consumption").toDouble();
        medianProduction += query.value("production").toDouble();
        medianAcquisition += query.value("acquisition").toDouble();
        medianStorage += query.value("storage").toDouble();
        totalConsumption = query.value("totalConsumption").toDouble();
        totalProduction = query.value("totalProduction").toDouble();
        totalAcquisition = query.value("totalAcquisition").toDouble();
        totalReturn = query.value("totalReturn").toDouble();
    }
    if (resultCount > 0) {
        // Scale to the actual length of the sample: 3 hour samples, days and longer ones vary with DST and the calendar.
        const double sampleMinutes = sampleStart.msecsTo(sampleEnd) / 60000.0;
        medianConsumption = medianConsumption * baseSampleRate / sampleMinutes;
        medianProduction = medianProduction * baseSampleRate / sampleMinutes;
        medianAcquisition = medianAcquisition * baseSampleRate / sampleMinutes;
        medianStorage = medianStorage * baseSampleRate / sampleMinutes;
    } else {
        query = QSqlQuery(db);
        query.prepare(QStringLiteral("SELECT * FROM powerBalance WHERE sampleRate = ? ORDER BY timestamp DESC LIMIT 1;"));
        query.addBindValue(baseSampleRate);
        if (!query.exec()) {
            qCWarning(dcEnergyExperience()) << "Error fetching newest power balance sample for" << baseSampleRate;
            qCWarning(dcEnergyExperience()) << "SQL error was:" << query.lastError() << "executed query:" << query.executedQuery();
            return false;
        }
        if (query.next()) {
            totalConsumption = query.value("totalConsumption").toDouble();
            totalProduction = query.value("totalProduction").toDouble();
            totalAcquisition = query.value("totalAcquisition").toDouble();
            totalReturn = query.value("totalReturn").toDouble();
        }
    }

    return maintenanceInsertPowerBalance(db, sampleEnd, sampleRate, medianConsumption, medianProduction, medianAcquisition, medianStorage, totalConsumption, totalProduction, totalAcquisition, totalReturn);
}

bool maintenanceSampleThingPower(QSqlDatabase &db, const ThingId &thingId, EnergyLogs::SampleRate sampleRate, EnergyLogs::SampleRate baseSampleRate, const QDateTime &sampleEnd)
{
    QDateTime sampleStart = maintenanceCalculateSampleStart(sampleEnd, sampleRate);

    double medianCurrentPower = 0;
    double totalConsumption = 0;
    double totalProduction = 0;

    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT * FROM thingPower WHERE thingId = ? AND sampleRate = ? AND timestamp > ? AND timestamp <= ? ORDER BY timestamp ASC;"));
    query.addBindValue(thingId);
    query.addBindValue(baseSampleRate);
    query.addBindValue(sampleStart.toMSecsSinceEpoch());
    query.addBindValue(sampleEnd.toMSecsSinceEpoch());
    if (!query.exec()) {
        qCWarning(dcEnergyExperience()) << "Error fetching thing power samples for" << baseSampleRate << "from" << sampleStart.toString() << "to" << sampleEnd.toString();
        qCWarning(dcEnergyExperience()) << "SQL error was:" << query.lastError() << "executed query:" << query.executedQuery();
        return false;
    }

    int resultCount = 0;
    while (query.next()) {
        if (QThread::currentThread()->isInterruptionRequested()) {
            return false;
        }
        resultCount++;
        medianCurrentPower += query.value("currentPower").toDouble();
        totalConsumption = query.value("totalConsumption").toDouble();
        totalProduction = query.value("totalProduction").toDouble();
    }
    if (resultCount > 0) {
        // Scale to the actual length of the sample: 3 hour samples, days and longer ones vary with DST and the calendar.
        const double sampleMinutes = sampleStart.msecsTo(sampleEnd) / 60000.0;
        medianCurrentPower = medianCurrentPower * baseSampleRate / sampleMinutes;
    } else {
        query = QSqlQuery(db);
        query.prepare(QStringLiteral("SELECT * FROM thingPower WHERE thingId = ? AND sampleRate = ? ORDER BY timestamp DESC LIMIT 1;"));
        query.addBindValue(thingId);
        query.addBindValue(baseSampleRate);
        if (!query.exec()) {
            qCWarning(dcEnergyExperience()) << "Error fetching newest thing power sample for" << thingId.toString() << baseSampleRate;
            qCWarning(dcEnergyExperience()) << "SQL error was:" << query.lastError() << "executed query:" << query.executedQuery();
            return false;
        }
        if (query.next()) {
            totalConsumption = query.value("totalConsumption").toDouble();
            totalProduction = query.value("totalProduction").toDouble();
        }
    }

    return maintenanceInsertThingPower(db, sampleEnd, sampleRate, thingId, medianCurrentPower, totalConsumption, totalProduction);
}

void maintenanceRectifySamples(QSqlDatabase &db, EnergyLogs::SampleRate sampleRate, EnergyLogs::SampleRate baseSampleRate, uint maxSamples,
                               const QDateTime &nextScheduledSample, const QList<ThingId> &thingIds)
{
    QDateTime oldestBaseSample = maintenanceGetOldestPowerBalanceSampleTimestamp(db, baseSampleRate);
    QDateTime newestSample = maintenanceGetNewestPowerBalanceSampleTimestamp(db, sampleRate);

    if (QThread::currentThread()->isInterruptionRequested()) {
        return;
    }

    if (newestSample.isNull()) {
        newestSample = oldestBaseSample;
    }

    if (!newestSample.isNull() && maintenanceNextSampleTimestamp(sampleRate, newestSample) < nextScheduledSample) {
        QDateTime nextSample = maintenanceNextSampleTimestamp(sampleRate, newestSample.addMSecs(1000));
        if (!maintenanceSampleAdvances(sampleRate, newestSample, nextSample)) {
            return;
        }
        maintenanceSamplePowerBalance(db, sampleRate, baseSampleRate, nextSample);
        newestSample = nextSample;
    }

    BalanceTotals latest = maintenanceLatestPowerBalanceTotals(db, sampleRate);

    if (!newestSample.isNull()) {
        newestSample = qMax(newestSample, maintenanceCalculateSampleStart(nextScheduledSample, sampleRate, (int)maxSamples));
    }

    db.transaction();
    while (!newestSample.isNull() && maintenanceNextSampleTimestamp(sampleRate, newestSample) < nextScheduledSample) {
        if (QThread::currentThread()->isInterruptionRequested()) {
            db.rollback();
            return;
        }
        QDateTime nextSample = maintenanceNextSampleTimestamp(sampleRate, newestSample.addMSecs(1000));
        if (!maintenanceSampleAdvances(sampleRate, newestSample, nextSample)) {
            db.commit();
            return;
        }
        maintenanceInsertPowerBalance(db, nextSample, sampleRate, 0, 0, 0, 0, latest.totalConsumption, latest.totalProduction, latest.totalAcquisition, latest.totalReturn);
        newestSample = nextSample;
    }
    db.commit();

    foreach (const ThingId &thingId, thingIds) {
        if (QThread::currentThread()->isInterruptionRequested()) {
            return;
        }

        QDateTime oldestBaseSample = maintenanceGetOldestThingPowerSampleTimestamp(db, thingId, baseSampleRate);
        QDateTime newestSample = maintenanceGetNewestThingPowerSampleTimestamp(db, thingId, sampleRate);

        if (newestSample.isNull()) {
            if (oldestBaseSample.isNull()) {
                continue;
            }
            newestSample = oldestBaseSample;
        }

        if (!newestSample.isNull() && maintenanceNextSampleTimestamp(sampleRate, newestSample) < nextScheduledSample) {
            QDateTime nextSample = maintenanceNextSampleTimestamp(sampleRate, newestSample.addMSecs(1000));
            if (!maintenanceSampleAdvances(sampleRate, newestSample, nextSample)) {
                return;
            }
            maintenanceSampleThingPower(db, thingId, sampleRate, baseSampleRate, nextSample);
            newestSample = nextSample;
        }

        ThingTotals latest = maintenanceLatestThingTotals(db, sampleRate, thingId);

        newestSample = qMax(newestSample, maintenanceCalculateSampleStart(nextScheduledSample, sampleRate, (int)maxSamples));

        db.transaction();
        while (!newestSample.isNull() && maintenanceNextSampleTimestamp(sampleRate, newestSample) < nextScheduledSample) {
            if (QThread::currentThread()->isInterruptionRequested()) {
                db.rollback();
                return;
            }
            QDateTime nextSample = maintenanceNextSampleTimestamp(sampleRate, newestSample.addMSecs(1000));
            if (!maintenanceSampleAdvances(sampleRate, newestSample, nextSample)) {
                db.commit();
                return;
            }
            maintenanceInsertThingPower(db, nextSample, sampleRate, thingId, 0, latest.totalConsumption, latest.totalProduction);
            newestSample = nextSample;
        }
        db.commit();
    }
}

void maintenanceFillMissingMinuteSamples(QSqlDatabase &db, int maxMinuteSamples, const QDateTime &fillUntil)
{
    if (!fillUntil.isValid()) {
        return;
    }

    // Power balance
    QSqlQuery newestBalanceQuery(db);
    newestBalanceQuery.prepare(QStringLiteral("SELECT timestamp, totalConsumption, totalProduction, totalAcquisition, totalReturn FROM powerBalance WHERE sampleRate = ? ORDER BY timestamp DESC LIMIT 1;"));
    newestBalanceQuery.addBindValue(EnergyLogs::SampleRate1Min);
    if (!newestBalanceQuery.exec()) {
        qCWarning(dcEnergyExperience()) << "Failed to query newest power balance 1-min sample:" << newestBalanceQuery.lastError();
        return;
    }
    if (newestBalanceQuery.next() && !newestBalanceQuery.value("timestamp").isNull()) {
        QDateTime newestTimestamp = QDateTime::fromMSecsSinceEpoch(newestBalanceQuery.value("timestamp").toLongLong());
        if (newestTimestamp < fillUntil) {
            QDateTime oldestTimestamp = fillUntil.addMSecs(-(qint64)maxMinuteSamples * 60 * 1000);
            QDateTime timestamp = newestTimestamp;
            if (oldestTimestamp > newestTimestamp) {
                // Avoid writing huge numbers of 0-samples when we'd trim them anyway.
                timestamp = fillUntil.addMSecs(-60000);
            }

            BalanceTotals totals;
            totals.totalConsumption = newestBalanceQuery.value("totalConsumption").toDouble();
            totals.totalProduction = newestBalanceQuery.value("totalProduction").toDouble();
            totals.totalAcquisition = newestBalanceQuery.value("totalAcquisition").toDouble();
            totals.totalReturn = newestBalanceQuery.value("totalReturn").toDouble();

            db.transaction();
            while (timestamp < fillUntil) {
                if (QThread::currentThread()->isInterruptionRequested()) {
                    db.rollback();
                    return;
                }
                timestamp = timestamp.addMSecs(60000);
                maintenanceInsertPowerBalance(db, timestamp, EnergyLogs::SampleRate1Min, 0, 0, 0, 0, totals.totalConsumption, totals.totalProduction, totals.totalAcquisition, totals.totalReturn);
            }
            db.commit();
        }
    }

    // Things
    const QList<ThingId> thingIds = maintenanceLoggedThings(db);
    foreach (const ThingId &thingId, thingIds) {
        if (QThread::currentThread()->isInterruptionRequested()) {
            return;
        }

        QSqlQuery newestThingQuery(db);
        newestThingQuery.prepare(QStringLiteral("SELECT timestamp, totalConsumption, totalProduction FROM thingPower WHERE thingId = ? AND sampleRate = ? ORDER BY timestamp DESC LIMIT 1;"));
        newestThingQuery.addBindValue(thingId);
        newestThingQuery.addBindValue(EnergyLogs::SampleRate1Min);
        if (!newestThingQuery.exec()) {
            qCWarning(dcEnergyExperience()) << "Failed to query newest thing power 1-min sample:" << thingId << newestThingQuery.lastError();
            continue;
        }
        if (!newestThingQuery.next() || newestThingQuery.value("timestamp").isNull()) {
            continue;
        }

        QDateTime newestTimestamp = QDateTime::fromMSecsSinceEpoch(newestThingQuery.value("timestamp").toLongLong());
        if (newestTimestamp >= fillUntil) {
            continue;
        }

        QDateTime oldestTimestamp = fillUntil.addMSecs(-(qint64)maxMinuteSamples * 60 * 1000);
        QDateTime timestamp = newestTimestamp;
        if (oldestTimestamp > newestTimestamp) {
            // Avoid writing huge numbers of 0-samples when we'd trim them anyway.
            timestamp = fillUntil.addMSecs(-60000);
        }

        ThingTotals totals;
        totals.totalConsumption = newestThingQuery.value("totalConsumption").toDouble();
        totals.totalProduction = newestThingQuery.value("totalProduction").toDouble();

        db.transaction();
        while (timestamp < fillUntil) {
            if (QThread::currentThread()->isInterruptionRequested()) {
                db.rollback();
                return;
            }
            timestamp = timestamp.addMSecs(60000);
            maintenanceInsertThingPower(db, timestamp, EnergyLogs::SampleRate1Min, thingId, 0, totals.totalConsumption, totals.totalProduction);
        }
        db.commit();
    }
}

} // namespace

EnergyLogger::EnergyLogger(QObject *parent)
    : EnergyLogs(parent)
{
    if (!initDB()) {
        qCCritical(dcEnergyExperience()) << "Unable to open energy log. Energy logs will not be available.";
        return;
    }

    // Logging configuration
    // Note: SampleRate1Min is always sampled as it is the base series for others
    // Make sure your base series always has enough samples to build a full sample
    // of all series building on it.

    // Disk space considerations;
    // Each entry takes approx 50 bytes for powerBalance + 60 bytes for thingCurrentPower per thing of disk space
    // SQLite adds metadata and overhead of about 5%
    // The resulting database size can be estimated with (count being the sum of all numbers below):
    // (count * 50 bytes) + (count * things * 60 bytes) + 5%
    // ~40000 entries, with 5 energy things => ~15MB
    // Note: use sqlite3_analyzer to see the approx. size per entry in each table.

    // One day 1440 min, let's keep one week
    m_maxMinuteSamples = 10080;

    addConfig(SampleRate15Mins, SampleRate1Min, 16128); // 6 months
    addConfig(SampleRate1Hour, SampleRate15Mins, 8760); // 1 year
    addConfig(SampleRate3Hours, SampleRate15Mins, 2920); // 1 year
    addConfig(SampleRate1Day, SampleRate1Hour, 1095); // 3 years
    addConfig(SampleRate1Week, SampleRate1Day, 168); // 3 years
    addConfig(SampleRate1Month, SampleRate1Day, 240); // 20 years
    addConfig(SampleRate1Year, SampleRate1Month, 20); // 20 years

    // Load last values from thingsPower logs so we have at least one base sample available for sampling, even if a thing might not produce any logs for a while.
    foreach (const ThingId &thingId, loggedThings()) {
        m_thingsPowerLiveLogs[thingId].append(latestLogEntry(SampleRate1Min, thingId));
    }

    // Start the scheduling
    scheduleNextSample(SampleRate1Min);
    foreach (SampleRate sampleRate, m_configs.keys()) {
        scheduleNextSample(sampleRate);
    }

    // Now all the data is initialized. We can start with sampling.
    // First check if we missed any samplings (e.g. because the system was offline at the time when it should have created a sample).
    // This can take a long time (especially when filling long gaps) so run it in the background.
    startDbMaintenance("startup resampling", calculateSampleStart(m_nextSamples.value(SampleRate1Min), SampleRate1Min));

    // And start the sampler timer
    connect(&m_sampleTimer, &QTimer::timeout, this, &EnergyLogger::sample);
    m_sampleTimer.start(1000);
}

EnergyLogger::~EnergyLogger()
{
    QThread *thread = m_dbMaintenanceThread;
    m_dbMaintenanceThread = nullptr;
    if (!thread) {
        return;
    }

    thread->requestInterruption();
    thread->wait();
    delete thread;
}

void EnergyLogger::startDbMaintenance(const QString &reason, const QDateTime &fillMinuteSamplesUntil)
{
    if (m_dbMaintenanceThread && m_dbMaintenanceThread->isRunning()) {
        qCDebug(dcEnergyExperience()) << "Energy log DB maintenance thread already running. Skipping request:" << reason;
        return;
    }
    if (m_dbMaintenanceThread) {
        delete m_dbMaintenanceThread;
        m_dbMaintenanceThread = nullptr;
    }

    if (m_dbMaintenanceRunning) {
        qCDebug(dcEnergyExperience()) << "Energy log DB maintenance already running. Skipping request:" << reason;
        return;
    }
    if (m_dbFilePath.isEmpty()) {
        qCWarning(dcEnergyExperience()) << "Cannot start energy log DB maintenance without a database path.";
        return;
    }

    m_dbMaintenanceRunning = true;
    qCInfo(dcEnergyExperience()) << "Starting energy log DB maintenance in background:" << reason;

    QList<MaintenanceConfig> configs;
    for (auto it = m_configs.constBegin(); it != m_configs.constEnd(); ++it) {
        MaintenanceConfig cfg;
        cfg.sampleRate = it.key();
        cfg.baseSampleRate = it.value().baseSampleRate;
        cfg.maxSamples = it.value().maxSamples;
        configs.append(cfg);
    }
    const int maxMinuteSamples = m_maxMinuteSamples;
    const QString dbFilePath = m_dbFilePath;
    const QHash<SampleRate, QDateTime> nextSamples = m_nextSamples;

    QPointer<EnergyLogger> self(this);
    QThread *thread = QThread::create([self, reason, dbFilePath, configs, maxMinuteSamples, fillMinuteSamplesUntil, nextSamples]() {
        if (!self) {
            return;
        }

        QElapsedTimer timer;
        timer.start();

        const QString connectionName = QStringLiteral("energylogs_maintenance_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
            db.setDatabaseName(dbFilePath);
            if (!db.open()) {
                qCWarning(dcEnergyExperience()) << "Cannot open energy log DB for maintenance at" << dbFilePath << db.lastError();
            } else {
                QSqlQuery pragmaQuery(db);
                pragmaQuery.exec(QStringLiteral("PRAGMA journal_mode=WAL;"));
                pragmaQuery.exec(QStringLiteral("PRAGMA synchronous=NORMAL;"));

                if (fillMinuteSamplesUntil.isValid()) {
                    maintenanceFillMissingMinuteSamples(db, maxMinuteSamples, fillMinuteSamplesUntil);
                }

                const QList<ThingId> thingIds = maintenanceLoggedThings(db);
                foreach (const MaintenanceConfig &cfg, configs) {
                    if (QThread::currentThread()->isInterruptionRequested()) {
                        break;
                    }
                    QDateTime nextScheduledSample = nextSamples.value(cfg.sampleRate);
                    if (!nextScheduledSample.isValid()) {
                        nextScheduledSample = maintenanceNextSampleTimestamp(cfg.sampleRate, QDateTime::currentDateTime());
                    }
                    maintenanceRectifySamples(db, cfg.sampleRate, cfg.baseSampleRate, cfg.maxSamples, nextScheduledSample, thingIds);
                }
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);

        if (!self) {
            return;
        }
        const qint64 durationMs = timer.elapsed();
        QMetaObject::invokeMethod(self, [self, reason, durationMs]() {
            if (!self) {
                return;
            }
            qCInfo(dcEnergyExperience()) << "Energy log DB maintenance finished:" << reason << "in" << durationMs << "ms.";
            self->m_dbMaintenanceRunning = false;
            self->flushPendingDbWrites();
        }, Qt::QueuedConnection);
    });

    m_dbMaintenanceThread = thread;
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (m_dbMaintenanceThread == thread) {
            m_dbMaintenanceThread = nullptr;
        }
        delete thread;
    });
    thread->start();
}

void EnergyLogger::flushPendingDbWrites()
{
    if (m_dbMaintenanceRunning) {
        return;
    }

    // Remove queued thing logs first to avoid re-inserting data for removed things.
    foreach (const ThingId &thingId, m_pendingThingLogRemovals) {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral("DELETE FROM thingPower WHERE thingId = ?;"));
        query.addBindValue(thingId);
        if (!query.exec()) {
            qCWarning(dcEnergyExperience()) << "Error removing thing energy logs for thing id" << thingId << query.lastError() << query.executedQuery();
        }

        query = QSqlQuery(m_db);
        query.prepare(QStringLiteral("DELETE FROM thingCache WHERE thingId = ?;"));
        query.addBindValue(thingId);
        if (!query.exec()) {
            qCWarning(dcEnergyExperience()) << "Error removing thing cache entry for thing id" << thingId << query.lastError() << query.executedQuery();
        }
    }
    m_pendingThingLogRemovals.clear();

    // Flush cached thing state (deduplicated to latest value per thing).
    for (auto it = m_pendingThingCacheEntries.constBegin(); it != m_pendingThingCacheEntries.constEnd(); ++it) {
        const ThingId &thingId = it.key();
        const QPair<double, double> &totals = it.value();
        cacheThingEntry(thingId, totals.first, totals.second);
    }
    m_pendingThingCacheEntries.clear();

    // Flush queued samples.
    const int pendingPowerBalanceSamples = m_pendingPowerBalanceSamples.count();
    const int pendingThingPowerSamples = m_pendingThingPowerSamples.count();

    for (const PendingPowerBalanceSample &sample: m_pendingPowerBalanceSamples) {
        insertPowerBalance(sample.timestamp, sample.sampleRate,
                           sample.consumption, sample.production, sample.acquisition, sample.storage,
                           sample.totalConsumption, sample.totalProduction, sample.totalAcquisition, sample.totalReturn);
    }
    m_pendingPowerBalanceSamples.clear();

    for (const PendingThingPowerSample &sample: m_pendingThingPowerSamples) {
        insertThingPower(sample.timestamp, sample.sampleRate, sample.thingId,
                         sample.currentPower, sample.totalConsumption, sample.totalProduction);
    }
    m_pendingThingPowerSamples.clear();

    if (pendingPowerBalanceSamples > 0 || pendingThingPowerSamples > 0) {
        qCDebug(dcEnergyExperience()) << "Flushed queued samples after DB maintenance. Power balance:" << pendingPowerBalanceSamples << "Thing power:" << pendingThingPowerSamples;
    }
}

void EnergyLogger::logPowerBalance(double consumption, double production, double acquisition, double storage, double totalConsumption, double totalProduction, double totalAcquisition, double totalReturn)
{
    PowerBalanceLogEntry entry(QDateTime::currentDateTime(), consumption, production, acquisition, storage, totalConsumption, totalProduction, totalAcquisition, totalReturn);

    // Add everything to livelog, keep that for one day, in memory only
    m_balanceLiveLog.prepend(entry);
    while (m_balanceLiveLog.count() > 1 && m_balanceLiveLog.last().timestamp().addDays(1) < QDateTime::currentDateTime()) {
        qCDebug(dcEnergyExperience()) << "Discarding livelog entry from" << m_balanceLiveLog.last().timestamp().toString();
        m_balanceLiveLog.removeLast();
    }
}

void EnergyLogger::logThingPower(const ThingId &thingId, double currentPower, double totalConsumption, double totalProduction)
{
    ThingPowerLogEntry entry(QDateTime::currentDateTime(), thingId, currentPower, totalConsumption, totalProduction);

    m_thingsPowerLiveLogs[thingId].prepend(entry);
    while (m_thingsPowerLiveLogs[thingId].count() > 1 && m_thingsPowerLiveLogs[thingId].last().timestamp().addDays(1) < QDateTime::currentDateTime()) {
        qCDebug(dcEnergyExperience()) << "Discarding thing power livelog entry for thing" << thingId << "from" << m_thingsPowerLiveLogs[thingId].last().timestamp().toString();
        m_thingsPowerLiveLogs[thingId].removeLast();
    }
}

PowerBalanceLogEntries EnergyLogger::powerBalanceLogs(SampleRate sampleRate, const QDateTime &from, const QDateTime &to) const
{
    PowerBalanceLogEntries result;

    QSqlQuery query(m_db);
    QString queryString = "SELECT * FROM powerBalance WHERE sampleRate = ?";
    QVariantList bindValues;
    bindValues << sampleRate;
    qCDebug(dcEnergyExperience()) << "Fetching logs. Timestamp:" << from << from.isNull();
    if (!from.isNull()) {
        queryString += " AND timestamp >= ?";
        bindValues << from.toMSecsSinceEpoch();
    }
    if (!to.isNull()) {
        queryString += " AND timestamp <= ?";
        bindValues << to.toMSecsSinceEpoch();
    }
    queryString += " ORDER BY timestamp ASC";
    query.prepare(queryString);
    foreach (const QVariant &bindValue, bindValues) {
        query.addBindValue(bindValue);
    }

    qCDebug(dcEnergyExperience()) << "Executing" << queryString << bindValues;
    query.exec();
    if (query.lastError().isValid()) {
        qCWarning(dcEnergyExperience()) << "Error fetching power balance logs:" << query.lastError() << query.executedQuery();
        return result;
    }

    while (query.next()) {
        //        qCDebug(dcEnergyExperience()) << "Adding result";
        result.append(queryResultToBalanceLogEntry(query.record()));
    }
    return result;
}

ThingPowerLogEntries EnergyLogger::thingPowerLogs(SampleRate sampleRate, const QList<ThingId> &thingIds, const QDateTime &from, const QDateTime &to) const
{
    ThingPowerLogEntries result;

    QSqlQuery query(m_db);
    QString queryString = "SELECT * FROM thingPower WHERE sampleRate = ?";
    QVariantList bindValues;
    bindValues << sampleRate;

    qCDebug(dcEnergyExperience()) << "Fetching thing power logs for" << thingIds;

    QStringList thingsQuery;
    foreach (const ThingId &thingId, thingIds) {
        thingsQuery.append("thingId = ?");
        bindValues << thingId;
    }
    if (!thingsQuery.isEmpty()) {
        queryString += " AND (" + thingsQuery.join(" OR ") + " )";
    }

    if (!from.isNull()) {
        queryString += " AND timestamp >= ?";
        bindValues << from.toMSecsSinceEpoch();
    }
    if (!to.isNull()) {
        queryString += " AND timestamp <= ?";
        bindValues << to.toMSecsSinceEpoch();
    }
    queryString += " ORDER BY timestamp ASC";
    query.prepare(queryString);
    foreach (const QVariant &bindValue, bindValues) {
        query.addBindValue(bindValue);
    }
    query.exec();
    if (query.lastError().isValid()) {
        qCWarning(dcEnergyExperience()) << "Error fetching thing power logs:" << query.lastError() << query.executedQuery();
        return result;
    }

    while (query.next()) {
        result.append(ThingPowerLogEntry(
            QDateTime::fromMSecsSinceEpoch(query.value("timestamp").toLongLong()),
            query.value("thingId").toUuid(),
            query.value("currentPower").toDouble(),
            query.value("totalConsumption").toDouble(),
            query.value("totalProduction").toDouble()));
    }
    return result;

}

PowerBalanceLogEntry EnergyLogger::latestLogEntry(SampleRate sampleRate)
{
    if (sampleRate == SampleRateAny) {
        if (m_balanceLiveLog.count() > 0) {
            return m_balanceLiveLog.first();
        }
    }

    QSqlQuery query(m_db);
    QString queryString = "SELECT * FROM powerBalance";
    QVariantList bindValues;
    if (sampleRate != SampleRateAny) {
        queryString += " WHERE sampleRate = ?";
        bindValues.append(sampleRate);
    }
    queryString += " ORDER BY timestamp DESC";
    if (sampleRate == SampleRateAny) {
        queryString += ", sampleRate ASC";
    }
    queryString += " LIMIT 1;";
    query.prepare(queryString);
    foreach (const QVariant &value, bindValues) {
        query.addBindValue(value);
    }
    if (!query.exec()) {
        qCWarning(dcEnergyExperience()) << "Error obtaining latest log entry from DB:" << query.lastError() << query.executedQuery();
        return PowerBalanceLogEntry();
    }
    if (!query.next()) {
        qCDebug(dcEnergyExperience()) << "No power balance log entry in DB for sample rate:" << sampleRate;
        return PowerBalanceLogEntry();
    }
    return queryResultToBalanceLogEntry(query.record());
}

ThingPowerLogEntry EnergyLogger::latestLogEntry(SampleRate sampleRate, const ThingId &thingId)
{
    if (sampleRate == SampleRateAny) {
        if (m_thingsPowerLiveLogs.value(thingId).count() > 0) {
            return m_thingsPowerLiveLogs.value(thingId).first();
        }
    }

    QSqlQuery query(m_db);
    QString queryString = "SELECT * FROM thingPower WHERE thingId = ?";
    QVariantList bindValues;
    bindValues << thingId;
    if (sampleRate != SampleRateAny) {
        queryString += " AND sampleRate = ?";
        bindValues << sampleRate;
    }
    queryString += " ORDER BY timestamp DESC";
    if (sampleRate == SampleRateAny) {
        queryString += ", sampleRate ASC";
    }
    queryString += " LIMIT 1;";
    query.prepare(queryString);
    foreach (const QVariant &bindValue, bindValues) {
        query.addBindValue(bindValue);
    }
    if (!query.exec()) {
        qCWarning(dcEnergyExperience()) << "Error fetching latest thing log entry from DB:" << query.lastError() << query.executedQuery();
        return ThingPowerLogEntry();
    }
    if (!query.next()) {
        qCDebug(dcEnergyExperience()) << "No thing power log entry in DB for sample rate:" << sampleRate << "thingId:" << thingId;
        return ThingPowerLogEntry();
    }
    return queryResultToThingPowerLogEntry(query.record());

}

void EnergyLogger::removeThingLogs(const ThingId &thingId)
{
    m_thingsPowerLiveLogs.remove(thingId);

    if (m_dbMaintenanceRunning) {
        m_pendingThingLogRemovals.insert(thingId);
        m_pendingThingCacheEntries.remove(thingId);

        // Drop any queued samples for this thing.
        for (int i = m_pendingThingPowerSamples.count() - 1; i >= 0; i--) {
            if (m_pendingThingPowerSamples.at(i).thingId == thingId) {
                m_pendingThingPowerSamples.removeAt(i);
            }
        }
        return;
    }

    QSqlQuery query(m_db);
    query.prepare("DELETE FROM thingPower WHERE thingId = ?;");
    query.addBindValue(thingId);
    query.exec();
    if (query.lastError().isValid()) {
        qCWarning(dcEnergyExperience()) << "Error removing thing energy logs for thing id" << thingId << query.lastError() << query.executedQuery();
    }

    query = QSqlQuery(m_db);
    query.prepare("DELETE FROM thingCache WHERE thingId = ?;");
    query.addBindValue(thingId);
    query.exec();
    if (query.lastError().isValid()) {
        qCWarning(dcEnergyExperience()) << "Error removing thing cache entry for thing id" << thingId << query.lastError() << query.executedQuery();
    }
}

QList<ThingId> EnergyLogger::loggedThings() const
{
    QList<ThingId> ret;

    QSqlQuery query(m_db);
    query.prepare("SELECT DISTINCT thingId FROM thingPower;");
    query.exec();
    if (query.lastError().isValid()) {
        qCWarning(dcEnergyExperience()) << "Failed to load existing things from logs:" << query.lastError();
    } else {
        while (query.next()) {
            ret.append(query.value("thingId").toUuid());
        }
    }
    return ret;
}

void EnergyLogger::cacheThingEntry(const ThingId &thingId, double totalEnergyConsumed, double totalEnergyProduced)
{
    if (m_dbMaintenanceRunning) {
        m_pendingThingCacheEntries.insert(thingId, {totalEnergyConsumed, totalEnergyProduced});
        return;
    }

    QSqlQuery query(m_db);
    query.prepare("INSERT OR REPLACE INTO thingCache (thingId, totalEnergyConsumed, totalEnergyProduced) VALUES (?, ?, ?);");
    query.addBindValue(thingId);
    query.addBindValue(totalEnergyConsumed);
    query.addBindValue(totalEnergyProduced);
    query.exec();
    if (query.lastError().isValid()) {
        qCWarning(dcEnergyExperience()) << "Failed to store thing cache entry:" << query.lastError() << query.executedQuery();
    }
}

ThingPowerLogEntry EnergyLogger::cachedThingEntry(const ThingId &thingId)
{
    QSqlQuery query(m_db);
    query.prepare("SELECT * FROM thingCache WHERE thingId = ?;");
    query.addBindValue(thingId);
    query.exec();
    if (query.lastError().isValid()) {
        qCWarning(dcEnergyExperience()) << "Failed to retrieve thing cache entry:" << query.lastError() << query.executedQuery();
        return ThingPowerLogEntry();
    }
    if (!query.next()) {
        qCDebug(dcEnergyExperience()) << "No cached thing entry for" << thingId;
        return ThingPowerLogEntry();
    }
    return ThingPowerLogEntry(QDateTime(), thingId, 0, query.value("totalEnergyConsumed").toDouble(), query.value("totalEnergyProduced").toDouble());
}

void EnergyLogger::sample()
{
    QDateTime now = QDateTime::currentDateTime();
    bool deferDbWrites = m_dbMaintenanceRunning;

    if (now >= m_nextSamples.value(SampleRate1Min)) {
        QDateTime sampleEnd = m_nextSamples.value(SampleRate1Min);
        QDateTime sampleStart = sampleEnd.addMSecs(-60 * 1000);
        bool alreadyLogged = false;

        if (!deferDbWrites) {
            PowerBalanceLogEntry newestInDB = latestLogEntry(SampleRate1Min);
            qCDebug(dcEnergyExperience()) << "Sampling power balance for 1 min from" << sampleStart.toString() << sampleEnd.toString() << "newest in DB:" << newestInDB.timestamp().toString();

            if (newestInDB.timestamp().isValid() && newestInDB.timestamp() < sampleStart) {
                qCWarning(dcEnergyExperience()).nospace() << "Clock skew detected. Scheduling background recovery job.";
                startDbMaintenance("clock skew recovery", sampleStart);
                deferDbWrites = true;
            } else if (newestInDB.timestamp().isValid() && newestInDB.timestamp() >= sampleEnd) {
                qCWarning(dcEnergyExperience()) << "Power balance sample for" << sampleEnd.toString(Qt::ISODate) << "already logged. Skipping it.";
                alreadyLogged = true;
            }
        }

        double medianConsumption = 0;
        double medianProduction = 0;
        double medianAcquisition = 0;
        double medianStorage = 0;
        for (int i = 0; i < m_balanceLiveLog.count(); i++) {
            const PowerBalanceLogEntry &entry = m_balanceLiveLog.at(i);
            QDateTime frameStart = entry.timestamp();
            if (frameStart < sampleStart) {
                frameStart = sampleStart;
            } else if (frameStart > sampleEnd) {
                frameStart = sampleEnd;
            }

            QDateTime frameEnd = i == 0 ? sampleEnd : m_balanceLiveLog.at(i-1).timestamp();
            if (frameEnd < sampleStart) {
                frameEnd = sampleStart;
            } else if (frameEnd > sampleEnd) {
                frameEnd = sampleEnd;
            }
            qint64 frameDuration = frameStart.msecsTo(frameEnd);
            if (frameDuration < 0) {
                frameDuration = 0;
            }
            qCDebug(dcEnergyExperience()) << "Frame" << i << "duration:" << frameDuration << "value:" << entry.consumption() << "start" << frameStart.toString() << "end" << frameEnd.toString();

            medianConsumption += entry.consumption() * frameDuration;
            medianProduction += entry.production() * frameDuration;
            medianAcquisition += entry.acquisition() * frameDuration;
            medianStorage += entry.storage() * frameDuration;
            if (entry.timestamp() < sampleStart) {
                break;
            }
        }
        medianConsumption /= sampleStart.msecsTo(sampleEnd);
        medianProduction /= sampleStart.msecsTo(sampleEnd);
        medianAcquisition /= sampleStart.msecsTo(sampleEnd);
        medianStorage /= sampleStart.msecsTo(sampleEnd);

        PowerBalanceLogEntry newest = latestLogEntry(SampleRateAny);
        double totalConsumption = newest.totalConsumption();
        double totalProduction = newest.totalProduction();
        double totalAcquisition = newest.totalAcquisition();
        double totalReturn = newest.totalReturn();

        qCDebug(dcEnergyExperience()) << "Sampled power balance:" << SampleRate1Min << "🔥:" << medianConsumption << "🌞:" << medianProduction << "💵:" << medianAcquisition << "🔋:" << medianStorage << "Totals:" << "🔥:" << totalConsumption << "🌞:" << totalProduction << "💵↓:" << totalAcquisition << "💵↑:" << totalReturn;
        if (!alreadyLogged) {
            insertPowerBalance(sampleEnd, SampleRate1Min, medianConsumption, medianProduction, medianAcquisition, medianStorage, totalConsumption, totalProduction, totalAcquisition, totalReturn);
        }

        foreach (const ThingId &thingId, m_thingsPowerLiveLogs.keys()) {
            bool thingAlreadyLogged = false;

            if (!deferDbWrites) {
                ThingPowerLogEntry newestInDB = latestLogEntry(SampleRate1Min, thingId);
                qCDebug(dcEnergyExperience()) << "Sampling thing power for" << thingId.toString() << SampleRate1Min << "from" << sampleStart.toString() << "to" <<  sampleEnd.toString() << "newest in DB" << newestInDB.timestamp().toString();

                if (newestInDB.timestamp().isValid() && newestInDB.timestamp() < sampleStart) {
                    qCWarning(dcEnergyExperience()).nospace() << "Clock skew detected for thing power logs. Scheduling background recovery job.";
                    startDbMaintenance("clock skew recovery", sampleStart);
                    deferDbWrites = true;
                } else if (newestInDB.timestamp().isValid() && newestInDB.timestamp() >= sampleEnd) {
                    qCWarning(dcEnergyExperience()) << "Thing power sample for" << thingId.toString() << sampleEnd.toString(Qt::ISODate) << "already logged. Skipping it.";
                    thingAlreadyLogged = true;
                }
            }

            double medianPower = 0;
            ThingPowerLogEntries entries = m_thingsPowerLiveLogs.value(thingId);
            for (int i = 0; i < entries.count(); i++) {
                const ThingPowerLogEntry &entry = entries.at(i);
                QDateTime frameStart = entry.timestamp();
                if (frameStart < sampleStart) {
                    frameStart = sampleStart;
                } else if (frameStart > sampleEnd) {
                    frameStart = sampleEnd;
                }

                QDateTime frameEnd = i == 0 ? sampleEnd : entries.at(i-1).timestamp();
                if (frameEnd < sampleStart) {
                    frameEnd = sampleStart;
                } else if (frameEnd > sampleEnd) {
                    frameEnd = sampleEnd;
                }
                qint64 frameDuration = frameStart.msecsTo(frameEnd);
                if (frameDuration < 0) {
                    frameDuration = 0;
                }
                qCDebug(dcEnergyExperience()) << "Frame" << i << "duration:" << frameDuration << "value:" << entry.currentPower();
                medianPower += entry.currentPower() * frameDuration;
                if (entry.timestamp() < sampleStart) {
                    break;
                }
            }
            medianPower /= sampleStart.msecsTo(sampleEnd);

            ThingPowerLogEntry newest = latestLogEntry(SampleRateAny, thingId);
            double totalConsumption = newest.totalConsumption();
            double totalProduction = newest.totalProduction();

            qCDebug(dcEnergyExperience()) << "Sampled thing power for" << thingId << SampleRate1Min << "🔥/🌞:" << medianPower << "Totals:" << "🔥:" << totalConsumption << "🌞:" << totalProduction;
            if (!thingAlreadyLogged) {
                insertThingPower(sampleEnd, SampleRate1Min, thingId, medianPower, totalConsumption, totalProduction);
            }
        }
    }

    // A background maintenance job (resampling / gap filling) is running.
    // Keep 1-minute sampling going (in-memory) but do not touch the DB for lower sample rates or housekeeping.
    if (deferDbWrites) {
        if (now > m_nextSamples.value(SampleRate1Min)) {
            scheduleNextSample(SampleRate1Min);
        }
        return;
    }

    // First sample all the configs.
    foreach (SampleRate sampleRate, m_configs.keys()) {
        if (now >= m_nextSamples.value(sampleRate)) {
            QDateTime sampleTime = m_nextSamples.value(sampleRate);
            SampleRate baseSampleRate = m_configs.value(sampleRate).baseSampleRate;
            QDateTime sampleStart = calculateSampleStart(sampleTime, sampleRate);
            QDateTime newestInDB = getNewestPowerBalanceSampleTimestamp(sampleRate);

            if (newestInDB.isValid() && newestInDB < sampleStart) {
                qCWarning(dcEnergyExperience()) << "Clock skew detected. Scheduling background recovery job.";
                startDbMaintenance("clock skew recovery");
                if (now >= m_nextSamples.value(SampleRate1Min)) {
                    scheduleNextSample(SampleRate1Min);
                }
                foreach (SampleRate rate, m_configs.keys()) {
                    if (now >= m_nextSamples.value(rate)) {
                        scheduleNextSample(rate);
                    }
                }
                return;
            }

            if (newestInDB.isValid() && newestInDB >= sampleTime) {
                qCWarning(dcEnergyExperience()) << "Sample for" << sampleRate << sampleTime.toString(Qt::ISODate) << "already logged. Skipping it.";
                continue;
            }

            samplePowerBalance(sampleRate, baseSampleRate, sampleTime);
            sampleThingsPower(sampleRate, baseSampleRate, sampleTime);
        }
    }

    // and then trim them
    if (now > m_nextSamples.value(SampleRate1Min)) {
        QDateTime sampleTime = m_nextSamples.value(SampleRate1Min);
        QDateTime oldestTimestamp = sampleTime.addMSecs(-(qint64)m_maxMinuteSamples * 60 * 1000);
        trimPowerBalance(SampleRate1Min, oldestTimestamp);
        foreach (const ThingId &thingId, m_thingsPowerLiveLogs.keys()) {
            trimThingPower(thingId, SampleRate1Min, oldestTimestamp);
        }
    }
    foreach (SampleRate sampleRate, m_configs.keys()) {
        if (now >= m_nextSamples.value(sampleRate)) {
            uint maxSamples = m_configs.value(sampleRate).maxSamples;
            QDateTime sampleTime = m_nextSamples.value(sampleRate);
            QDateTime oldestTimestamp = calculateSampleStart(sampleTime, sampleRate, maxSamples);
            trimPowerBalance(sampleRate, oldestTimestamp);
            foreach (const ThingId &thingId, m_thingsPowerLiveLogs.keys()) {
                trimThingPower(thingId, sampleRate, oldestTimestamp);
            }
        }
    }

    // Lastly we reschedule the next sample for each config
    // Note: keep this at the end as the previous stuff uses the schedule to work
    if (now > m_nextSamples.value(SampleRate1Min)) {
        scheduleNextSample(SampleRate1Min);
    }
    foreach (SampleRate sampleRate, m_configs.keys()) {
        if (now >= m_nextSamples.value(sampleRate)) {
            scheduleNextSample(sampleRate);
        }
    }
}

bool EnergyLogger::initDB()
{
    m_db.close();

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), "energylogs");
    m_db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    QDir path = QDir(NymeaSettings::storagePath());
    if (!path.exists())
        path.mkpath(path.path());

    m_dbFilePath = path.filePath("energylogs.sqlite");
    m_db.setDatabaseName(m_dbFilePath);

    if (!m_db.isValid()) {
        qCWarning(dcEnergyExperience()) << "The energy database is not valid" << m_db.databaseName();
        return false;
    }

    qCDebug(dcEnergyExperience()) << "Opening energy database" << m_db.databaseName();
    if (!m_db.open()) {
        qCWarning(dcEnergyExperience()) << "Cannot open energy log DB at" << m_db.databaseName() << m_db.lastError();
        return false;
    }

    // Improve concurrency between readers (main thread) and the background maintenance job.
    // NOTE: journal_mode is persisted per database, running this is cheap.
    QSqlQuery pragmaQuery(m_db);
    pragmaQuery.exec(QStringLiteral("PRAGMA journal_mode=WAL;"));
    pragmaQuery.exec(QStringLiteral("PRAGMA synchronous=NORMAL;"));

    if (!m_db.tables().contains("metadata")) {
        qCDebug(dcEnergyExperience()) << "No \"metadata\" table in database. Creating it.";

        QString queryString = "CREATE TABLE IF NOT EXISTS metadata (version INT);";
        QSqlQuery createTableMetadataVersionQuery(queryString, m_db);

        if (!createTableMetadataVersionQuery.exec()) {
            qCWarning(dcEnergyExperience()) << "Error creating metadata table in energy log database. Query:" << queryString << createTableMetadataVersionQuery.lastError().text() << "Driver error:" << m_db.lastError().driverText() << "Database error:" << m_db.lastError().databaseText();
            return false;
        }

        queryString = "INSERT INTO metadata (version) VALUES (1);";
        QSqlQuery writeVersionQuery(queryString, m_db);
        if (!writeVersionQuery.exec()) {
            qCWarning(dcEnergyExperience()) << "Error writing metadata table in energy log database. Query:" << queryString << writeVersionQuery.lastError().text() << "Driver error:" << m_db.lastError().driverText() << "Database error:" << m_db.lastError().databaseText();
            return false;
        }
    }

    if (!m_db.tables().contains("powerBalance")) {
        qCDebug(dcEnergyExperience()) << "No \"powerBalance\" table in database. Creating it.";
        QString query("CREATE TABLE IF NOT EXISTS powerBalance "
                      "("
                      "timestamp BIGINT,"
                      "sampleRate INT,"
                      "consumption FLOAT,"
                      "production FLOAT,"
                      "acquisition FLOAT,"
                      "storage FLOAT,"
                      "totalConsumption FLOAT,"
                      "totalProduction FLOAT,"
                      "totalAcquisition FLOAT,"
                      "totalReturn FLOAT"
                      ");");

        QSqlQuery createTableQuery(query, m_db);
        if (!createTableQuery.exec()) {
            qCWarning(dcEnergyExperience()) << "Error creating powerBalance table in energy log database." << query << createTableQuery.lastError().text() << "Driver error:" << m_db.lastError().driverText() << "Database error:" << m_db.lastError().databaseText();
            return false;
        }

    }

    QSqlQuery createPowerBalanceIndexQuery("CREATE INDEX IF NOT EXISTS idx_powerBalance ON powerBalance(sampleRate, timestamp);", m_db);
    if (!createPowerBalanceIndexQuery.exec()) {
        qCWarning(dcEnergyExperience()) << "Error creating powerBalance table index in energy log database. Query:" << createPowerBalanceIndexQuery.lastQuery() << createPowerBalanceIndexQuery.lastError().text() << "Driver error:" << m_db.lastError().driverText() << "Database error:" << m_db.lastError().databaseText();
        return false;
    }

    if (!m_db.tables().contains("thingPower")) {
        qCDebug(dcEnergyExperience()) << "No \"thingPower\" table in database. Creating it.";
        QString query("CREATE TABLE IF NOT EXISTS thingPower "
                      "("
                      "timestamp BIGINT,"
                      "sampleRate INT,"
                      "thingId VARCHAR(38),"
                      "currentPower FLOAT,"
                      "totalConsumption FLOAT,"
                      "totalProduction FLOAT"
                      ");");

        QSqlQuery createThingPowerTableQuery(query, m_db);
        if (!createThingPowerTableQuery.exec()) {
            qCWarning(dcEnergyExperience()) << "Error creating thingPower table in energy log database. Query:" << query << createThingPowerTableQuery.lastError().text() << "Driver error:" << m_db.lastError().driverText() << "Database error:" << m_db.lastError().databaseText();
            return false;
        }
    }

    QSqlQuery createThingPowerIndexQuery("CREATE INDEX IF NOT EXISTS idx_thingPower ON thingPower(thingId, sampleRate, timestamp);", m_db);
    if (!createThingPowerIndexQuery.exec()) {
        qCWarning(dcEnergyExperience()) << "Error creating thingPower table index in energy log database. Query:" << createThingPowerIndexQuery.lastQuery() << createThingPowerIndexQuery.lastError().text() << "Driver error:" << m_db.lastError().driverText() << "Database error:" << m_db.lastError().databaseText();
        return false;
    }

    if (!m_db.tables().contains("thingCache")) {
        qCDebug(dcEnergyExperience()) << "No \"thingCache\" table in database. Creating it.";
        QString query("CREATE TABLE IF NOT EXISTS thingCache "
                      "("
                      "thingId VARCHAR(38) PRIMARY KEY,"
                      "totalEnergyConsumed FLOAT,"
                      "totalEnergyProduced FLOAT"
                      ");");

        QSqlQuery createThingCacheTableQuery(query, m_db);
        if (!createThingCacheTableQuery.exec()) {
            qCWarning(dcEnergyExperience()) << "Error creating thingCache table in energy log database. Query:" << query << createThingCacheTableQuery.lastError().text() << "Driver error:" << m_db.lastError().driverText() << "Database error:" << m_db.lastError().databaseText();
            return false;
        }
    }

    removeDuplicateSamples(m_db);

    qCDebug(dcEnergyExperience()) << "Initialized logging DB successfully." << m_db.databaseName();
    return true;
}

void EnergyLogger::addConfig(SampleRate sampleRate, SampleRate baseSampleRate, int maxSamples)
{
    SampleConfig config;
    config.baseSampleRate = baseSampleRate;
    config.maxSamples = maxSamples;
    m_configs.insert(sampleRate, config);
}

QDateTime EnergyLogger::getOldestPowerBalanceSampleTimestamp(SampleRate sampleRate)
{
    QSqlQuery query(m_db);
    query.prepare("SELECT MIN(timestamp) AS oldestTimestamp FROM powerBalance WHERE sampleRate = ?;");
    query.addBindValue(sampleRate);
    query.exec();
    if (query.next() && !query.value("oldestTimestamp").isNull()) {
        return QDateTime::fromMSecsSinceEpoch(query.value("oldestTimestamp").toLongLong());
    }
    return QDateTime();
}

QDateTime EnergyLogger::getNewestPowerBalanceSampleTimestamp(SampleRate sampleRate)
{
    QSqlQuery query(m_db);
    query.prepare("SELECT MAX(timestamp) AS latestTimestamp FROM powerBalance WHERE sampleRate = ?;");
    query.addBindValue(sampleRate);
    query.exec();
    if (query.next() && !query.value("latestTimestamp").isNull()) {
        return QDateTime::fromMSecsSinceEpoch(query.value("latestTimestamp").toLongLong());
    }
    return QDateTime();
}

QDateTime EnergyLogger::getOldestThingPowerSampleTimestamp(const ThingId &thingId, SampleRate sampleRate)
{
    QSqlQuery query(m_db);
    query.prepare("SELECT MIN(timestamp) AS oldestTimestamp FROM thingPower WHERE thingId = ? AND sampleRate = ?;");
    query.addBindValue(thingId);
    query.addBindValue(sampleRate);
    query.exec();
    if (query.next() && !query.value("oldestTimestamp").isNull()) {
        return QDateTime::fromMSecsSinceEpoch(query.value("oldestTimestamp").toLongLong());
    }
    return QDateTime();
}

QDateTime EnergyLogger::getNewestThingPowerSampleTimestamp(const ThingId &thingId, SampleRate sampleRate)
{
    QSqlQuery query(m_db);
    query.prepare("SELECT MAX(timestamp) AS newestTimestamp FROM thingPower WHERE thingId = ? AND sampleRate = ?;");
    query.addBindValue(thingId);
    query.addBindValue(sampleRate);
    query.exec();
    if (query.next() && !query.value("newestTimestamp").isNull()) {
        return QDateTime::fromMSecsSinceEpoch(query.value("newestTimestamp").toLongLong());
    }
    return QDateTime();
}

void EnergyLogger::scheduleNextSample(SampleRate sampleRate)
{
    // Advance relative to the previously scheduled sample to avoid skipping samples when we're behind.
    // If we don't have a schedule yet, align based on "now".
    QDateTime base = m_nextSamples.value(sampleRate);
    if (!base.isValid()) {
        base = QDateTime::currentDateTime();
    } else {
        base = base.addMSecs(1000);
    }

    QDateTime next = nextSampleTimestamp(sampleRate, base);
    m_nextSamples.insert(sampleRate, next);
    qCDebug(dcEnergyExperience()) << "Next sample for" << sampleRate << "scheduled at" << next.toString();
}

QDateTime EnergyLogger::calculateSampleStart(const QDateTime &sampleEnd, SampleRate sampleRate, int sampleCount)
{
    return maintenanceCalculateSampleStart(sampleEnd, sampleRate, sampleCount);
}

void EnergyLogger::rectifySamples(SampleRate sampleRate, SampleRate baseSampleRate)
{
    // Normally we'd need to find the newest available sample of a series and catch up from there.
    // However, it could happen a series does not have any samples at all yet. For example if we're logging since january,
    // and at new years the system was off, we missed the new years yearly sample and don't have any earlier. For those cases
    // we need to start resampling from the oldest timestamp we find in the DB for the base sampleRate.
    QDateTime oldestBaseSample = getOldestPowerBalanceSampleTimestamp(baseSampleRate);
    QDateTime newestSample = getNewestPowerBalanceSampleTimestamp(sampleRate);

    QDateTime startTime = QDateTime::currentDateTime();
    qCDebug(dcEnergyExperience()) << "Checking for missing power balance samples for" << sampleRate;
    qCDebug(dcEnergyExperience()) << "Newest sample:" << newestSample.toString() << "Oldest base sample:" << oldestBaseSample.toString();
    if (newestSample.isNull()) {
        qCDebug(dcEnergyExperience()) << "No sample at all so far. Using base as starting point.";
        newestSample = oldestBaseSample;
    }

    //    qCDebug(dcEnergyExperience()) << "next sample after last in series:" << nextSampleTimestamp(sampleRate, newestSample).toString();
    //    qCDebug(dcEnergyExperience()) << "next scheduled sample:" << m_nextSamples.value(sampleRate).toString();
    if (!newestSample.isNull() && nextSampleTimestamp(sampleRate, newestSample) < m_nextSamples[sampleRate]) {
        // regularly sample one sample as there may be some valid samples in the base series
        QDateTime nextSample = nextSampleTimestamp(sampleRate, newestSample.addMSecs(1000));
        if (!maintenanceSampleAdvances(sampleRate, newestSample, nextSample)) {
            return;
        }
        samplePowerBalance(sampleRate, baseSampleRate, nextSample);
        newestSample = nextSample;
    }
    // Now retrieve the last sample and just carry over totals
    PowerBalanceLogEntry latest = latestLogEntry(sampleRate);
    // We only need to rectify up to maxSamples, so don't bother with older ones
    if (!newestSample.isNull()) {
        newestSample = qMax(newestSample, calculateSampleStart(m_nextSamples[sampleRate], sampleRate, m_configs.value(sampleRate).maxSamples));
    }

    m_db.transaction();
    int count = 0;
    while (!newestSample.isNull() && nextSampleTimestamp(sampleRate, newestSample) < m_nextSamples[sampleRate]) {
        QDateTime nextSample = nextSampleTimestamp(sampleRate, newestSample.addMSecs(1000));
        if (!maintenanceSampleAdvances(sampleRate, newestSample, nextSample)) {
            m_db.commit();
            return;
        }
        insertPowerBalance(nextSample, sampleRate, 0, 0, 0, 0, latest.totalConsumption(), latest.totalProduction(), latest.totalAcquisition(), latest.totalReturn());
        newestSample = nextSample;
        count++;
    }
    m_db.commit();
    qCDebug(dcEnergyExperience()) << "Done rectifying" << count << "power balance samples for" << sampleRate << "in" << startTime.msecsTo(QDateTime::currentDateTime()) << "ms";

    foreach (const ThingId &thingId, m_thingsPowerLiveLogs.keys()) {
        QDateTime oldestBaseSample = getOldestThingPowerSampleTimestamp(thingId, baseSampleRate);
        QDateTime newestSample = getNewestThingPowerSampleTimestamp(thingId, sampleRate);

        QDateTime startTime = QDateTime::currentDateTime();
        qCDebug(dcEnergyExperience()) << "Checking for missing thing samples for" << sampleRate << "for thing" << thingId;
        qCDebug(dcEnergyExperience()) << "Newest sample:" << newestSample.toString() << "Oldest base sample:" << oldestBaseSample.toString();
        if (newestSample.isNull()) {
            qCDebug(dcEnergyExperience()) << "No sample at all so far. Using base as starting point.";
            if (oldestBaseSample.isNull()) {
                qCDebug(dcEnergyExperience()) << "Base series doesn't have any samples either. Skipping resampling for" << sampleRate << "for" << thingId;
                continue;
            }
            newestSample = oldestBaseSample;
        }
        //        qCDebug(dcEnergyExperience()) << "T next sample after last in series:" << nextSampleTimestamp(sampleRate, newestSample).toString();
        //        qCDebug(dcEnergyExperience()) << "T next scheduled sample:" << m_nextSamples.value(sampleRate).toString();
        if (!newestSample.isNull() && nextSampleTimestamp(sampleRate, newestSample) < m_nextSamples[sampleRate]) {
            QDateTime nextSample = nextSampleTimestamp(sampleRate, newestSample.addMSecs(1000));
            if (!maintenanceSampleAdvances(sampleRate, newestSample, nextSample)) {
                return;
            }
            sampleThingPower(thingId, sampleRate, baseSampleRate, nextSample);
            newestSample = nextSample;
        }
        // Now retrieve the last sample and just carry over totals
        ThingPowerLogEntry latest = latestLogEntry(sampleRate, thingId);
        // We only need to rectify up to maxSamples, so don't bother with older ones
        newestSample = qMax(newestSample, calculateSampleStart(m_nextSamples[sampleRate], sampleRate, m_configs.value(sampleRate).maxSamples));

        m_db.transaction();
        int count = 0;
        while (!newestSample.isNull() && nextSampleTimestamp(sampleRate, newestSample) < m_nextSamples[sampleRate]) {
            QDateTime nextSample = nextSampleTimestamp(sampleRate, newestSample.addMSecs(1000));
            if (!maintenanceSampleAdvances(sampleRate, newestSample, nextSample)) {
                m_db.commit();
                return;
            }
            insertThingPower(nextSample, sampleRate, thingId, 0, latest.totalConsumption(), latest.totalProduction());
            newestSample = nextSample;
            count++;
        }
        m_db.commit();
        qCDebug(dcEnergyExperience()) << "Done rectifying" << count << "thing power samples for" << sampleRate << " for" << thingId << "in" << startTime.msecsTo(QDateTime::currentDateTime()) << "ms";
    }
}

QDateTime EnergyLogger::nextSampleTimestamp(SampleRate sampleRate, const QDateTime &dateTime)
{
    if (sampleRate == SampleRateAny) {
        qCWarning(dcEnergyExperience()) << "Cannot calculate next sample timestamp without a sample rate";
        return QDateTime();
    }
    return maintenanceNextSampleTimestamp(sampleRate, dateTime);
}

bool EnergyLogger::samplePowerBalance(SampleRate sampleRate, SampleRate baseSampleRate, const QDateTime &sampleEnd)
{
    QDateTime sampleStart = calculateSampleStart(sampleEnd, sampleRate);

    qCDebug(dcEnergyExperience()) << "Sampling power balance" << sampleRate << "from" << sampleStart << "to" << sampleEnd;

    double medianConsumption = 0;
    double medianProduction = 0;
    double medianAcquisition = 0;
    double medianStorage = 0;
    double totalConsumption = 0;
    double totalProduction = 0;
    double totalAcquisition = 0;
    double totalReturn = 0;

    QSqlQuery query(m_db);
    query.prepare("SELECT * FROM powerBalance WHERE sampleRate = ? AND timestamp > ? AND timestamp <= ? ORDER BY timestamp ASC;");
    query.addBindValue(baseSampleRate);
    query.addBindValue(sampleStart.toMSecsSinceEpoch());
    query.addBindValue(sampleEnd.toMSecsSinceEpoch());
    query.exec();

    if (query.lastError().isValid()) {
        qCWarning(dcEnergyExperience()) << "Error fetching power balance samples for" << baseSampleRate << "from" << sampleStart.toString() << "to" << sampleEnd.toString();
        qCWarning(dcEnergyExperience()) << "SQL error was:" << query.lastError() << "executed query:" << query.executedQuery();
        return false;
    }

    int resultCount = 0;
    while (query.next()) {
        resultCount++;
        qCDebug(dcEnergyExperience()) << "Frame:" << QDateTime::fromMSecsSinceEpoch(query.value("timestamp").toLongLong()).toString() << query.value("consumption").toDouble() << query.value("production").toDouble() << query.value("acquisition").toDouble() << query.value("storage").toDouble() << query.value("totalConsumption").toDouble() << query.value("totalProduction").toDouble() << query.value("totalAcquisition").toDouble() << query.value("totalReturn").toDouble();
        medianConsumption += query.value("consumption").toDouble();
        medianProduction += query.value("production").toDouble();
        medianAcquisition += query.value("acquisition").toDouble();
        medianStorage += query.value("storage").toDouble();
        totalConsumption = query.value("totalConsumption").toDouble();
        totalProduction = query.value("totalProduction").toDouble();
        totalAcquisition = query.value("totalAcquisition").toDouble();
        totalReturn = query.value("totalReturn").toDouble();
    }
    if (resultCount > 0) {
        // Scale to the actual length of the sample: 3 hour samples, days and longer ones vary with DST and the calendar.
        const double sampleMinutes = sampleStart.msecsTo(sampleEnd) / 60000.0;
        medianConsumption = medianConsumption * baseSampleRate / sampleMinutes;
        medianProduction = medianProduction * baseSampleRate / sampleMinutes;
        medianAcquisition = medianAcquisition * baseSampleRate / sampleMinutes;
        medianStorage = medianStorage * baseSampleRate / sampleMinutes;

    } else {
        // If there are no base samples for the given time frame at all, let's try to find the last existing one in the base
        // to at least copy the totals from where we left off.

        query = QSqlQuery(m_db);
        query.prepare("SELECT * FROM powerBalance WHERE sampleRate = ? ORDER BY timestamp DESC LIMIT 1;");
        query.addBindValue(baseSampleRate);
        query.exec();
        if (query.lastError().isValid()) {
            qCWarning(dcEnergyExperience()) << "Error fetching newest power balance sample for" << baseSampleRate;
            qCWarning(dcEnergyExperience()) << "SQL error was:" << query.lastError() << "executed query:" << query.executedQuery();
            return false;
        }

        if (query.next()) {
            totalConsumption = query.value("totalConsumption").toDouble();
            totalProduction = query.value("totalProduction").toDouble();
            totalAcquisition = query.value("totalAcquisition").toDouble();
            totalReturn = query.value("totalReturn").toDouble();
        }
    }


    qCDebug(dcEnergyExperience()) << "Sampled:" << "🔥:" << medianConsumption << "🌞:" << medianProduction << "💵:" << medianAcquisition << "🔋:" << medianStorage << "Totals:" << "🔥:" << totalConsumption << "🌞:" << totalProduction << "💵↓:" << totalAcquisition << "💵↑:" << totalReturn;
    return insertPowerBalance(sampleEnd, sampleRate, medianConsumption, medianProduction, medianAcquisition, medianStorage, totalConsumption, totalProduction, totalAcquisition, totalReturn);
}

bool EnergyLogger::insertPowerBalance(const QDateTime &timestamp, SampleRate sampleRate, double consumption, double production, double acquisition, double storage, double totalConsumption, double totalProduction, double totalAcquisition, double totalReturn)
{
    if (m_dbMaintenanceRunning) {
        PendingPowerBalanceSample pending;
        pending.timestamp = timestamp;
        pending.sampleRate = sampleRate;
        pending.consumption = consumption;
        pending.production = production;
        pending.acquisition = acquisition;
        pending.storage = storage;
        pending.totalConsumption = totalConsumption;
        pending.totalProduction = totalProduction;
        pending.totalAcquisition = totalAcquisition;
        pending.totalReturn = totalReturn;
        m_pendingPowerBalanceSamples.append(pending);
        return true;
    }

    QSqlQuery query = QSqlQuery(m_db);
    query.prepare("INSERT INTO powerBalance (timestamp, sampleRate, consumption, production, acquisition, storage, totalConsumption, totalProduction, totalAcquisition, totalReturn) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    query.addBindValue(timestamp.toMSecsSinceEpoch());
    query.addBindValue(sampleRate);
    query.addBindValue(consumption);
    query.addBindValue(production);
    query.addBindValue(acquisition);
    query.addBindValue(storage);
    query.addBindValue(totalConsumption);
    query.addBindValue(totalProduction);
    query.addBindValue(totalAcquisition);
    query.addBindValue(totalReturn);
    query.exec();
    if (query.lastError().isValid()) {
        qCWarning(dcEnergyExperience()) << "Error logging consumption sample:" << query.lastError() << query.executedQuery();
        return false;
    }
    emit powerBalanceEntryAdded(sampleRate, PowerBalanceLogEntry(timestamp, consumption, production, acquisition, storage, totalConsumption, totalProduction, totalAcquisition, totalReturn));
    return true;
}

bool EnergyLogger::sampleThingsPower(SampleRate sampleRate, SampleRate baseSampleRate, const QDateTime &sampleEnd)
{
    bool ret = true;
    foreach (const ThingId &thingId, m_thingsPowerLiveLogs.keys()) {
        ret &= sampleThingPower(thingId, sampleRate, baseSampleRate, sampleEnd);
    }
    return ret;
}

bool EnergyLogger::sampleThingPower(const ThingId &thingId, SampleRate sampleRate, SampleRate baseSampleRate, const QDateTime &sampleEnd)
{
    QDateTime sampleStart = calculateSampleStart(sampleEnd, sampleRate);
    qCDebug(dcEnergyExperience()) << "Sampling thing power for" << thingId.toString() << sampleRate << "from" << sampleStart.toString() << "to" << sampleEnd.toString();

    double medianCurrentPower = 0;
    double totalConsumption = 0;
    double totalProduction = 0;

    QSqlQuery query(m_db);
    query.prepare("SELECT * FROM thingPower WHERE thingId = ? AND sampleRate = ? AND timestamp > ? AND timestamp <= ? ORDER BY timestamp ASC;");
    query.addBindValue(thingId);
    query.addBindValue(baseSampleRate);
    query.addBindValue(sampleStart.toMSecsSinceEpoch());
    query.addBindValue(sampleEnd.toMSecsSinceEpoch());
    query.exec();

    if (query.lastError().isValid()) {
        qCWarning(dcEnergyExperience()) << "Error fetching thing power samples for" << baseSampleRate << "from" << sampleStart.toString() << "to" << sampleEnd.toString();
        qCWarning(dcEnergyExperience()) << "SQL error was:" << query.lastError() << "executed query:" << query.executedQuery();
        return false;
    }

    qCDebug(dcEnergyExperience()) << "Query:" << query.executedQuery();
    qCDebug(dcEnergyExperience()) << "Results:" << query.size();

    int resultCount = 0;
    while (query.next()) {
        resultCount++;
        qCDebug(dcEnergyExperience()) << "Frame:" << query.value("currentPower").toDouble() << QDateTime::fromMSecsSinceEpoch(query.value("timestamp").toLongLong()).toString();
        medianCurrentPower += query.value("currentPower").toDouble();
        totalConsumption = query.value("totalConsumption").toDouble();
        totalProduction = query.value("totalProduction").toDouble();
    }
    if (resultCount > 0) {
        // Scale to the actual length of the sample: 3 hour samples, days and longer ones vary with DST and the calendar.
        const double sampleMinutes = sampleStart.msecsTo(sampleEnd) / 60000.0;
        medianCurrentPower = medianCurrentPower * baseSampleRate / sampleMinutes;

    } else {
        // If there are no base samples for the given time frame at all, let's try to find the last existing one in the base
        // to at least copy the totals from where we left off.

        query = QSqlQuery(m_db);
        query.prepare("SELECT * FROM thingPower WHERE thingId = ? AND sampleRate = ? ORDER BY timestamp DESC LIMIT 1;");
        query.addBindValue(thingId);
        query.addBindValue(baseSampleRate);
        query.exec();
        if (query.lastError().isValid()) {
            qCWarning(dcEnergyExperience()) << "Error fetching newest thing power sample for" << thingId.toString() << baseSampleRate;
            qCWarning(dcEnergyExperience()) << "SQL error was:" << query.lastError() << "executed query:" << query.executedQuery();
            return false;
        }

        if (query.next()) {
            totalConsumption = query.value("totalConsumption").toDouble();
            totalProduction = query.value("totalProduction").toDouble();
        }
    }


    qCDebug(dcEnergyExperience()) << "Sampled:" << thingId.toString() << sampleRate << "median currentPower:" << medianCurrentPower << "total consumption:" << totalConsumption << "total production:" << totalProduction;
    return insertThingPower(sampleEnd, sampleRate, thingId, medianCurrentPower, totalConsumption, totalProduction);
}

bool EnergyLogger::insertThingPower(const QDateTime &timestamp, SampleRate sampleRate, const ThingId &thingId, double currentPower, double totalConsumption, double totalProduction)
{
    if (m_dbMaintenanceRunning) {
        PendingThingPowerSample pending;
        pending.timestamp = timestamp;
        pending.sampleRate = sampleRate;
        pending.thingId = thingId;
        pending.currentPower = currentPower;
        pending.totalConsumption = totalConsumption;
        pending.totalProduction = totalProduction;
        m_pendingThingPowerSamples.append(pending);
        return true;
    }

    QSqlQuery query = QSqlQuery(m_db);
    query.prepare("INSERT INTO thingPower (timestamp, sampleRate, thingId, currentPower, totalConsumption, totalProduction) values (?, ?, ?, ?, ?, ?);");
    query.addBindValue(timestamp.toMSecsSinceEpoch());
    query.addBindValue(sampleRate);
    query.addBindValue(thingId);
    query.addBindValue(currentPower);
    query.addBindValue(totalConsumption);
    query.addBindValue(totalProduction);
    query.exec();
    if (query.lastError().isValid()) {
        qCWarning(dcEnergyExperience()) << "Error logging thing power sample:" << query.lastError() << query.executedQuery();
        return false;
    }
    emit thingPowerEntryAdded(sampleRate, ThingPowerLogEntry(timestamp, thingId, currentPower, totalConsumption, totalProduction));
    return true;
}

void EnergyLogger::trimPowerBalance(SampleRate sampleRate, const QDateTime &beforeTime)
{
    if (m_dbMaintenanceRunning) {
        return;
    }

    QSqlQuery query(m_db);
    query.prepare("DELETE FROM powerBalance WHERE sampleRate = ? AND timestamp < ?;");
    query.addBindValue(sampleRate);
    query.addBindValue(beforeTime.toMSecsSinceEpoch());
    query.exec();
    if (query.numRowsAffected() > 0) {
        qCDebug(dcEnergyExperience()).nospace() << "Trimmed " << query.numRowsAffected() << " from power balance series: " << sampleRate << " (Older than: " << beforeTime.toString() << ")";
    }
}

void EnergyLogger::trimThingPower(const ThingId &thingId, SampleRate sampleRate, const QDateTime &beforeTime)
{
    if (m_dbMaintenanceRunning) {
        return;
    }

    QSqlQuery query(m_db);
    query.prepare("DELETE FROM thingPower WHERE thingId = ? AND sampleRate = ? AND timestamp < ?;");
    query.addBindValue(thingId);
    query.addBindValue(sampleRate);
    query.addBindValue(beforeTime.toMSecsSinceEpoch());
    query.exec();
    if (query.numRowsAffected() > 0) {
        qCDebug(dcEnergyExperience()).nospace() << "Trimmed " << query.numRowsAffected() << " from thing power series for: " << thingId << sampleRate << " (Older than: " << beforeTime.toString() << ")";
    }
}

PowerBalanceLogEntry EnergyLogger::queryResultToBalanceLogEntry(const QSqlRecord &record) const
{
    return PowerBalanceLogEntry(QDateTime::fromMSecsSinceEpoch(record.value("timestamp").toLongLong()),
                                record.value("consumption").toDouble(),
                                record.value("production").toDouble(),
                                record.value("acquisition").toDouble(),
                                record.value("storage").toDouble(),
                                record.value("totalConsumption").toDouble(),
                                record.value("totalProduction").toDouble(),
                                record.value("totalAcquisition").toDouble(),
                                record.value("totalReturn").toDouble());

}

ThingPowerLogEntry EnergyLogger::queryResultToThingPowerLogEntry(const QSqlRecord &record) const
{
    return ThingPowerLogEntry(QDateTime::fromMSecsSinceEpoch(record.value("timestamp").toULongLong()),
                              record.value("thingId").toUuid(),
                              record.value("currentPower").toDouble(),
                              record.value("totalConsumption").toDouble(),
                              record.value("totalProduction").toDouble());
}
