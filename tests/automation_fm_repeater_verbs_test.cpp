// FM repeater verbs: slice tone / slice offset, transmit rfpower / tunepower,
// and the single shared slice-action list (#5102).
//
// #5102 was filed reporting squelch as unreachable from the bridge. It was not:
// `slice dsp squelch <on|off> [level]` had implemented it all along. The report
// happened because two hand-maintained copies of the slice-action list had
// drifted, and the one a caller actually hits — `unknown slice action:` — omitted
// filter, agc and dsp. So the list is now derived from one function, and the row
// below that pins both messages to it is the regression guard that matters most
// here: it is the defect that manufactured a false bug report.
//
// What this file pins is BOUNDARY behaviour, which is what a radio-less CI run
// can assert. Validation must happen before any slice is resolved (Principle
// VII), and the fixture makes that observable by carrying a RadioModel with NO
// slices: a well-formed request reports "no slice available", a malformed one
// reports its own error. If validation drifts back behind slice resolution the
// two collapse into one message and these rows fail.
//
// Apply paths are proven against a live FLEX-6500 and a real repeater; see the
// PR. They are deliberately not faked here.

#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "models/RadioModel.h"
#include "core/AutomationServer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = QString())
{
    std::printf("%s %-58s %s\n", ok ? "[ OK ]" : "[FAIL]", name, qPrintable(detail));
    if (!ok)
        ++g_failed;
}

QJsonObject request(QLocalSocket& socket, const QByteArray& line)
{
    socket.write(line + '\n');
    socket.flush();

    QByteArray response;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2000 && !response.contains('\n')) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        response.append(socket.readAll());
        if (!response.contains('\n'))
            QThread::msleep(1);
    }
    if (!response.contains('\n'))
        return QJsonObject{{QStringLiteral("testError"), QStringLiteral("timeout")}};

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(response.trimmed(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return QJsonObject{{QStringLiteral("testError"), error.errorString()}};
    return doc.object();
}

QString errorOf(const QJsonObject& o)
{
    return o.value(QStringLiteral("error")).toString();
}

// A rejection that came from the VERB, not from the radio layer behind it.
void rejectedAtBoundary(QLocalSocket& socket, const char* name,
                        const QByteArray& line, const QString& expectFragment)
{
    const QJsonObject r = request(socket, line);
    const QString e = errorOf(r);
    report(name,
           r.value(QStringLiteral("ok")).toBool() == false
               && e.contains(expectFragment, Qt::CaseInsensitive)
               && !e.contains(QStringLiteral("no slice available")),
           e.isEmpty() ? QStringLiteral("(no error field)") : e);
}

} // namespace

int main(int argc, char** argv)
{
    QTemporaryDir testRoot;
    if (!testRoot.isValid()) {
        std::printf("[FAIL] temporary HOME could not be created\n");
        return 1;
    }
    const QByteArray root = testRoot.path().toUtf8();
    qputenv("HOME", root);
    qputenv("XDG_CONFIG_HOME", root);
    qputenv("TMPDIR", root);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    // Unset: the gate rows below assert TX is refused by default.
    qunsetenv("AETHER_AUTOMATION_ALLOW_TX");
    // start() reads the ceiling unconditionally, so it is in force the moment
    // TX is later permitted at runtime.
    qputenv("AETHER_AUTOMATION_TX_MAX_POWER", "30");

    QGuiApplication app(argc, argv);

    RadioModel radio;   // deliberately no slices — see the header comment
    AutomationServer server;
    server.setRadioModel(&radio);

    const QString serverName = QStringLiteral("aethersdr-fmverbs-test-%1")
                                   .arg(QCoreApplication::applicationPid());
    const bool started = server.start(serverName);
    report("bridge starts", started, server.fullServerName());
    if (!started)
        return 1;

    QLocalSocket socket;
    socket.connectToServer(serverName);
    const bool connected = socket.waitForConnected(2000);
    report("probe connects", connected, socket.errorString());
    if (!connected) {
        server.stop();
        return 1;
    }
    QCoreApplication::processEvents();

    // ── the one shared action list ───────────────────────────────────────
    // THE row this file exists for. Two hand-maintained copies drifted and the
    // divergence caused a false bug report; both messages must now name the
    // same set.
    {
        const QString empty = errorOf(request(socket, QByteArrayLiteral("slice")));
        const QString unknown =
            errorOf(request(socket, QByteArrayLiteral("slice bogusaction")));
        bool sameSet = true;
        for (const char* a : {"add", "remove", "select", "tx", "mode", "filter", "agc",
                              "dsp", "tone", "offset", "diversity", "centerlock",
                              "link", "txant", "rxant", "rxsource", "fixture",
                              "clearfixture"}) {
            const QString act = QString::fromLatin1(a);
            if (empty.contains(act) != unknown.contains(act))
                sameSet = false;
        }
        report("both slice error messages name the same action set", sameSet);
        report("the previously-omitted actions are advertised",
               unknown.contains(QStringLiteral("filter"))
                   && unknown.contains(QStringLiteral("agc"))
                   && unknown.contains(QStringLiteral("dsp")),
               unknown);
        report("the new actions are advertised",
               unknown.contains(QStringLiteral("tone"))
                   && unknown.contains(QStringLiteral("offset")),
               unknown);
    }

    // ── the pre-existing squelch route still works ───────────────────────
    // #5102 reported squelch as missing; `slice dsp squelch` already did it.
    // Nothing here may break that path — reaching the radio layer is the proof
    // the action is still routed.
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("slice dsp squelch off"));
        report("slice dsp squelch still routes (the pre-existing path)",
               errorOf(r).contains(QStringLiteral("no slice available")), errorOf(r));
    }

    // ── control row: a WELL-FORMED request reaches the radio layer ───────
    // Everything below is only meaningful against this.
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("slice tone ctcss_tx 100.0"));
        report("well-formed tone reaches the radio layer",
               errorOf(r).contains(QStringLiteral("no slice available")), errorOf(r));
    }

    // ── tone ─────────────────────────────────────────────────────────────
    rejectedAtBoundary(socket, "tone with no argument is refused",
                       QByteArrayLiteral("slice tone"), QStringLiteral("requires"));
    rejectedAtBoundary(socket, "tone mode 'dcs' is refused",
                       QByteArrayLiteral("slice tone dcs"), QStringLiteral("off/ctcss_tx"));
    rejectedAtBoundary(socket, "tone freq 9999 is refused",
                       QByteArrayLiteral("slice tone ctcss_tx 9999"), QStringLiteral("CTCSS"));
    rejectedAtBoundary(socket, "tone freq 0 is refused",
                       QByteArrayLiteral("slice tone ctcss_tx 0"), QStringLiteral("CTCSS"));

    // ── offset ───────────────────────────────────────────────────────────
    rejectedAtBoundary(socket, "offset with no argument is refused",
                       QByteArrayLiteral("slice offset"), QStringLiteral("requires"));
    rejectedAtBoundary(socket, "offset direction 'sideways' is refused",
                       QByteArrayLiteral("slice offset sideways"),
                       QStringLiteral("simplex/up/down"));
    rejectedAtBoundary(socket, "offset magnitude 'far' is refused",
                       QByteArrayLiteral("slice offset up far"), QStringLiteral("MHz"));

    // ── transmit: gated, then clamped ────────────────────────────────────
    {
        const QJsonObject verbs = request(socket, QByteArrayLiteral("verbs"));
        bool hasTransmit = false;
        for (const QJsonValue& v : verbs.value(QStringLiteral("verbs")).toArray())
            if (v.toObject().value(QStringLiteral("name")).toString()
                == QLatin1String("transmit"))
                hasTransmit = true;
        report("transmit verb is registered", hasTransmit);
    }
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("transmit rfpower 40"));
        report("transmit rfpower is blocked without ALLOW_TX",
               r.value(QStringLiteral("ok")).toBool() == false
                   && errorOf(r).contains(QStringLiteral("ALLOW_TX")),
               errorOf(r));
    }
    {
        // Principle VI: the gate must not be probeable with nonsense.
        const QJsonObject r = request(socket, QByteArrayLiteral("transmit rfpower 9999"));
        report("gate reports before value validation on a TX verb",
               errorOf(r).contains(QStringLiteral("ALLOW_TX")), errorOf(r));
    }
    rejectedAtBoundary(socket, "unknown transmit action is refused",
                       QByteArrayLiteral("transmit wattage 5"),
                       QStringLiteral("rfpower|tunepower"));
    rejectedAtBoundary(socket, "transmit with no action is refused",
                       QByteArrayLiteral("transmit"), QStringLiteral("requires an action"));

    // Now permit TX and prove the ceiling still binds. The invoke() power rail
    // is widget-scoped (it keys off accessibleName in the setValue path), so a
    // verb reaching the model directly does NOT inherit it — this row is the
    // reason the clamp is written out explicitly in doTransmit().
    server.setTxAllowed(true);
    QCoreApplication::processEvents();
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("transmit rfpower 90"));
        report("transmit rfpower is accepted once TX is allowed",
               r.value(QStringLiteral("ok")).toBool(), errorOf(r));
        report("transmit rfpower is clamped to AETHER_AUTOMATION_TX_MAX_POWER",
               r.value(QStringLiteral("clampedTo")).toInt() == 30
                   && r.value(QStringLiteral("requested")).toInt() == 90,
               QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
        report("the clamp is reported, not silent",
               r.contains(QStringLiteral("requested"))
                   && r.contains(QStringLiteral("clampedTo")));
    }
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("transmit tunepower 90"));
        report("tunepower is clamped by the same ceiling",
               r.value(QStringLiteral("clampedTo")).toInt() == 30,
               QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    }
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("transmit rfpower 10"));
        report("a request under the ceiling is not annotated",
               r.value(QStringLiteral("ok")).toBool()
                   && !r.contains(QStringLiteral("clampedTo")),
               QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    }
    rejectedAtBoundary(socket, "rfpower 101 is refused even with TX allowed",
                       QByteArrayLiteral("transmit rfpower 101"), QStringLiteral("0..100"));

    socket.disconnectFromServer();
    server.stop();

    std::printf("\n%s\n", g_failed == 0 ? "all rows passed" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
