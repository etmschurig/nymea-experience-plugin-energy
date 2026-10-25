# Tests for the energy logger sampling (DST changes, resampling loops, duplicate cleanup).
# Built separately from the package: qmake6 tests/tests.pro && make && make check
#
# ENERGYLOGGER_PLUGIN_DIR may point to another copy of plugin/ (e.g. an older release) to run the same
# tests against it. Add CONFIG+=energylogger_legacy if that copy predates the duplicate cleanup and the guard.

QT += testlib sql network
QT -= gui

CONFIG += testcase link_pkgconfig
PKGCONFIG += nymea

include(../../../config.pri)

TARGET = energylogger-test

isEmpty(ENERGYLOGGER_PLUGIN_DIR): ENERGYLOGGER_PLUGIN_DIR = $$top_srcdir/plugin
energylogger_legacy: DEFINES += ENERGYLOGGER_LEGACY

INCLUDEPATH += $$ENERGYLOGGER_PLUGIN_DIR $$top_srcdir/libnymea-energy

HEADERS += \
    $$ENERGYLOGGER_PLUGIN_DIR/energylogger.h \
    $$top_srcdir/libnymea-energy/energylogs.h

SOURCES += \
    testenergylogger.cpp \
    $$top_srcdir/libnymea-energy/energylogs.cpp
