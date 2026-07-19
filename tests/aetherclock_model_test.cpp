// AetherClockModel unit test — the thin, Q_PROPERTY-covered mirror of the
// AetherClock engine (PRD-A §8 row 4). Exercises defaults, the guard-compare
// "emit once" setter/slot idiom, the state machine + name maps, and the
// QUEUED engine→model wiring (attachEngine must not double-connect).
//
// The model's engine-facing handlers are PRIVATE slots by design; per the WS-2
// spec we drive them through the meta-object with Qt::QueuedConnection +
// processEvents (invokable regardless of access) and read the resulting state
// back through the Q_PROPERTY system to prove the property wiring.

#include "models/AetherClockModel.h"
#include "core/AetherClockEngine.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QSignalSpy>
#include <QString>
#include <QTime>
#include <QTimeZone>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

// Deliver everything queued so far (the engine→model and meta-invoked slots
// are all Qt::QueuedConnection).
static void pump()
{
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // Enum args cross a queued connection (engine→model) and are passed to the
    // private slots via Q_ARG — they must be known to the metatype system.
    qRegisterMetaType<AetherSDR::ClockLockState>("AetherSDR::ClockLockState");
    qRegisterMetaType<AetherSDR::ClockStation>("AetherSDR::ClockStation");

    // ---- 1. Defaults ----
    {
        AetherClockModel m;
        CHECK(m.state() == int(ClockLockState::NoSignal));       // 0
        CHECK(m.lockState() == ClockLockState::NoSignal);
        CHECK(m.stateName() == QStringLiteral("NoSignal"));
        CHECK(m.station() == int(ClockStation::Unknown));        // 0
        CHECK(m.stationName() == QStringLiteral("Unknown"));
        CHECK(m.lockQuality() == 0);
        CHECK(m.sliceId() == -1);
        CHECK(m.gpsTimeAvailable() == false);
        CHECK(!m.decodedUtc().isValid());
        CHECK(m.offsetMs() == 0.0);
        // Same facts, but through the Q_PROPERTY seam (protocol/QML surface).
        CHECK(m.property("state").toInt() == 0);
        CHECK(m.property("stateName").toString() == QStringLiteral("NoSignal"));
        CHECK(m.property("station").toInt() == 0);
        CHECK(m.property("sliceId").toInt() == -1);
        CHECK(m.property("gpsTimeAvailable").toBool() == false);
        CHECK(!m.property("decodedUtc").toDateTime().isValid());
    }

    // ---- 2. Change-signal-once (guard-compare, emit exactly once) ----
    {
        AetherClockModel m;

        // Public writable properties — direct calls, invoked twice / same value.
        QSignalSpy sliceSpy(&m, &AetherClockModel::sliceIdChanged);
        m.setSliceId(5);
        m.setSliceId(5);
        CHECK(sliceSpy.count() == 1);
        CHECK(m.sliceId() == 5);

        QSignalSpy gpsSpy(&m, &AetherClockModel::gpsTimeAvailableChanged);
        m.setGpsTimeAvailable(true);
        m.setGpsTimeAvailable(true);
        CHECK(gpsSpy.count() == 1);
        CHECK(m.gpsTimeAvailable() == true);

        // Private engine-facing slots — driven queued, same value twice.
        QSignalSpy stateSpy(&m, &AetherClockModel::stateChanged);
        QMetaObject::invokeMethod(&m, "onLockStateChanged", Qt::QueuedConnection,
                                  Q_ARG(AetherSDR::ClockLockState, ClockLockState::Locked));
        QMetaObject::invokeMethod(&m, "onLockStateChanged", Qt::QueuedConnection,
                                  Q_ARG(AetherSDR::ClockLockState, ClockLockState::Locked));
        pump();
        CHECK(stateSpy.count() == 1);
        CHECK(m.state() == int(ClockLockState::Locked));

        QSignalSpy stationSpy(&m, &AetherClockModel::stationChanged);
        QMetaObject::invokeMethod(&m, "onStationDetected", Qt::QueuedConnection,
                                  Q_ARG(AetherSDR::ClockStation, ClockStation::Wwv));
        QMetaObject::invokeMethod(&m, "onStationDetected", Qt::QueuedConnection,
                                  Q_ARG(AetherSDR::ClockStation, ClockStation::Wwv));
        pump();
        CHECK(stationSpy.count() == 1);
        CHECK(m.station() == int(ClockStation::Wwv));

        const QDateTime utc(QDate(2026, 7, 19), QTime(12, 34, 56), QTimeZone::utc());
        QSignalSpy utcSpy(&m, &AetherClockModel::decodedUtcChanged);
        QSignalSpy offSpy(&m, &AetherClockModel::offsetMsChanged);
        QSignalSpy qualSpy(&m, &AetherClockModel::lockQualityChanged);
        QMetaObject::invokeMethod(&m, "onTimeDecoded", Qt::QueuedConnection,
                                  Q_ARG(QDateTime, utc), Q_ARG(double, 100.0), Q_ARG(int, 50));
        QMetaObject::invokeMethod(&m, "onTimeDecoded", Qt::QueuedConnection,
                                  Q_ARG(QDateTime, utc), Q_ARG(double, 100.0), Q_ARG(int, 50));
        pump();
        CHECK(utcSpy.count() == 1);
        CHECK(offSpy.count() == 1);
        CHECK(qualSpy.count() == 1);
    }

    // ---- 3. onTimeDecoded updates the three fields; readable via Q_PROPERTY ----
    {
        AetherClockModel m;
        QSignalSpy utcSpy(&m, &AetherClockModel::decodedUtcChanged);
        QSignalSpy offSpy(&m, &AetherClockModel::offsetMsChanged);
        QSignalSpy qualSpy(&m, &AetherClockModel::lockQualityChanged);

        const QDateTime utc(QDate(2026, 7, 19), QTime(0, 0, 30), QTimeZone::utc());
        QMetaObject::invokeMethod(&m, "onTimeDecoded", Qt::QueuedConnection,
                                  Q_ARG(QDateTime, utc), Q_ARG(double, 123.0), Q_ARG(int, 87));
        pump();

        CHECK(utcSpy.count() == 1 && offSpy.count() == 1 && qualSpy.count() == 1);
        // Prove the Q_PROPERTY wiring (bridge `get clock` / QML surface).
        CHECK(m.property("offsetMs").toDouble() == 123.0);
        CHECK(m.property("lockQuality").toInt() == 87);
        CHECK(m.property("decodedUtc").toDateTime() == utc);
        // And the plain accessors agree.
        CHECK(m.offsetMs() == 123.0);
        CHECK(m.lockQuality() == 87);
        CHECK(m.decodedUtc() == utc);
    }

    // ---- 4. State machine: NoSignal → Acquiring → Locked → NoSignal ----
    {
        AetherClockModel m;
        QSignalSpy stateSpy(&m, &AetherClockModel::stateChanged);

        QMetaObject::invokeMethod(&m, "onLockStateChanged", Qt::QueuedConnection,
                                  Q_ARG(AetherSDR::ClockLockState, ClockLockState::Acquiring));
        pump();
        CHECK(stateSpy.count() == 1);
        CHECK(m.state() == int(ClockLockState::Acquiring));
        CHECK(m.stateName() == QStringLiteral("Acquiring"));

        QMetaObject::invokeMethod(&m, "onLockStateChanged", Qt::QueuedConnection,
                                  Q_ARG(AetherSDR::ClockLockState, ClockLockState::Locked));
        pump();
        CHECK(stateSpy.count() == 2);
        CHECK(m.state() == int(ClockLockState::Locked));
        CHECK(m.stateName() == QStringLiteral("Locked"));

        QMetaObject::invokeMethod(&m, "onLockStateChanged", Qt::QueuedConnection,
                                  Q_ARG(AetherSDR::ClockLockState, ClockLockState::NoSignal));
        pump();
        CHECK(stateSpy.count() == 3);
        CHECK(m.state() == int(ClockLockState::NoSignal));
        CHECK(m.stateName() == QStringLiteral("NoSignal"));
    }

    // ---- 5. stationDetected(Wwvh) → station()==2, stationName "WWVH" ----
    {
        AetherClockModel m;
        QSignalSpy stationSpy(&m, &AetherClockModel::stationChanged);
        QMetaObject::invokeMethod(&m, "onStationDetected", Qt::QueuedConnection,
                                  Q_ARG(AetherSDR::ClockStation, ClockStation::Wwvh));
        pump();
        CHECK(stationSpy.count() == 1);
        CHECK(m.station() == 2);
        CHECK(m.stationName() == QStringLiteral("WWVH"));
        // Round out the name map through the same seam.
        QMetaObject::invokeMethod(&m, "onStationDetected", Qt::QueuedConnection,
                                  Q_ARG(AetherSDR::ClockStation, ClockStation::Wwvb));
        pump();
        CHECK(m.station() == 3);
        CHECK(m.stationName() == QStringLiteral("WWVB"));
    }

    // ---- 6. attachEngine + queued semantics: no double-connect on re-attach ----
    // A real engine is enough (no PanadapterStream needed): we drive the
    // engine's lockStateChanged signal through the meta-object and count how
    // many times the model reacts. attachEngine wires QUEUED, so we pump.
    // Attaching the SAME engine twice must detach the prior connection first —
    // a double-connect would deliver the second transition twice.
    {
        AetherClockEngine engine;
        AetherClockModel m;
        m.attachEngine(&engine);

        QSignalSpy stateSpy(&m, &AetherClockModel::stateChanged);

        // First engine transition → exactly one model reaction.
        QMetaObject::invokeMethod(&engine, "lockStateChanged", Qt::DirectConnection,
                                  Q_ARG(AetherSDR::ClockLockState, ClockLockState::Acquiring));
        pump();
        CHECK(stateSpy.count() == 1);
        CHECK(m.state() == int(ClockLockState::Acquiring));

        // Re-attach the same engine, then a new transition. If attachEngine
        // failed to detach, this would bump the count by 2 (→ 3), not 1.
        m.attachEngine(&engine);
        QMetaObject::invokeMethod(&engine, "lockStateChanged", Qt::DirectConnection,
                                  Q_ARG(AetherSDR::ClockLockState, ClockLockState::Locked));
        pump();
        CHECK(stateSpy.count() == 2);            // single delivery, not doubled
        CHECK(m.state() == int(ClockLockState::Locked));
    }

    if (g_failures == 0) {
        std::printf("aetherclock_model_test: all checks passed\n");
        return 0;
    }
    std::printf("aetherclock_model_test: %d failure(s)\n", g_failures);
    return 1;
}
