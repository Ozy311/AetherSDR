// FM repeater verbs: slice squelch / squelchlevel / tone / offset, and
// transmit rfpower / tunepower (#5102).
//
// The bridge could not operate an FM repeater at all: it could not hear one
// (no squelch control), open one (no CTCSS), transmit to one (no offset), or
// set its drive. Every setter already existed on SliceModel/TransmitModel —
// only the bridge could not reach them.
//
// What this file pins is the BOUNDARY behaviour, which is what a radio-less
// CI run can actually assert: a malformed request must be refused by the verb
// itself, BEFORE any slice is resolved (Principle VII). The distinction is
// visible precisely because this fixture has a RadioModel with no slices —
// a well-formed request gets "no slice available", a malformed one gets its
// own validation error. If validation ever drifts back behind slice
// resolution, the two collapse into the same message and these rows fail.
//
// The apply paths are proven against a live FLEX-6500 and a real repeater;
// see the PR. They are deliberately not faked here.

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
    std::printf("%s %-56s %s\n", ok ? "[ OK ]" : "[FAIL]", name, qPrintable(detail));
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
void reportRejectedAtBoundary(QLocalSocket& socket, const char* name,
                              const QByteArray& line, const QString& expectFragment)
{
    const QJsonObject r = request(socket, line);
    const QString e = errorOf(r);
    const bool refused = (r.value(QStringLiteral("ok")).toBool() == false);
    const bool ownError = e.contains(expectFragment, Qt::CaseInsensitive);
    const bool notRadioError = !e.contains(QStringLiteral("no slice available"));
    report(name, refused && ownError && notRadioError, e.isEmpty() ? QStringLiteral("(no error field)") : e);
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
    // Left deliberately unset: the transmit rows below assert the TX gate.
    qunsetenv("AETHER_AUTOMATION_ALLOW_TX");

    QGuiApplication app(argc, argv);

    RadioModel radio;   // no slices — see the header comment
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

    // ── the verbs exist at all ───────────────────────────────────────────
    {
        const QJsonObject verbs = request(socket, QByteArrayLiteral("verbs"));
        bool hasTransmit = false;
        for (const QJsonValue& v : verbs.value(QStringLiteral("verbs")).toArray())
            if (v.toObject().value(QStringLiteral("name")).toString()
                == QLatin1String("transmit"))
                hasTransmit = true;
        report("transmit verb is registered", hasTransmit);
    }

    // ── the control row: a WELL-FORMED request reaches the radio layer ───
    // Everything below is only meaningful against this. If this row ever
    // reports a validation error instead, the fixture has changed and the
    // boundary rows below are no longer proving what they claim.
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("slice squelch on 30"));
        report("well-formed squelch reaches the radio layer",
               errorOf(r).contains(QStringLiteral("no slice available")), errorOf(r));
    }

    // ── squelch: refused at the boundary ─────────────────────────────────
    reportRejectedAtBoundary(socket, "squelch with no argument is refused",
                             QByteArrayLiteral("slice squelch"), QStringLiteral("requires"));
    reportRejectedAtBoundary(socket, "squelch state 'maybe' is refused",
                             QByteArrayLiteral("slice squelch maybe"), QStringLiteral("on|off"));
    reportRejectedAtBoundary(socket, "squelch level 101 is refused",
                             QByteArrayLiteral("slice squelch on 101"), QStringLiteral("0..100"));
    reportRejectedAtBoundary(socket, "squelch level -1 is refused",
                             QByteArrayLiteral("slice squelch on -1"), QStringLiteral("0..100"));
    reportRejectedAtBoundary(socket, "squelchlevel with no argument is refused",
                             QByteArrayLiteral("slice squelchlevel"), QStringLiteral("requires"));
    reportRejectedAtBoundary(socket, "squelchlevel 'loud' is refused",
                             QByteArrayLiteral("slice squelchlevel loud"), QStringLiteral("0..100"));

    // ── tone ─────────────────────────────────────────────────────────────
    reportRejectedAtBoundary(socket, "tone with no argument is refused",
                             QByteArrayLiteral("slice tone"), QStringLiteral("requires"));
    reportRejectedAtBoundary(socket, "tone mode 'dcs' is refused",
                             QByteArrayLiteral("slice tone dcs"), QStringLiteral("off/ctcss_tx"));
    reportRejectedAtBoundary(socket, "tone freq 9999 is refused",
                             QByteArrayLiteral("slice tone ctcss_tx 9999"), QStringLiteral("CTCSS"));
    reportRejectedAtBoundary(socket, "tone freq 0 is refused",
                             QByteArrayLiteral("slice tone ctcss_tx 0"), QStringLiteral("CTCSS"));

    // ── offset ───────────────────────────────────────────────────────────
    reportRejectedAtBoundary(socket, "offset with no argument is refused",
                             QByteArrayLiteral("slice offset"), QStringLiteral("requires"));
    reportRejectedAtBoundary(socket, "offset direction 'sideways' is refused",
                             QByteArrayLiteral("slice offset sideways"),
                             QStringLiteral("simplex/up/down"));
    reportRejectedAtBoundary(socket, "offset magnitude 'far' is refused",
                             QByteArrayLiteral("slice offset up far"), QStringLiteral("MHz"));

    // ── transmit: TX-gated, and gated BEFORE the value is even parsed ────
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("transmit rfpower 40"));
        report("transmit rfpower is blocked without ALLOW_TX",
               r.value(QStringLiteral("ok")).toBool() == false
                   && errorOf(r).contains(QStringLiteral("ALLOW_TX")),
               errorOf(r));
    }
    {
        // Principle VI: the gate must not be bypassable by sending nonsense —
        // an out-of-range value on a TX-gated verb still reports the gate.
        const QJsonObject r = request(socket, QByteArrayLiteral("transmit rfpower 9999"));
        report("gate reports before value validation on a TX verb",
               errorOf(r).contains(QStringLiteral("ALLOW_TX")), errorOf(r));
    }
    reportRejectedAtBoundary(socket, "unknown transmit action is refused",
                             QByteArrayLiteral("transmit wattage 5"),
                             QStringLiteral("rfpower|tunepower"));
    reportRejectedAtBoundary(socket, "transmit with no action is refused",
                             QByteArrayLiteral("transmit"), QStringLiteral("requires an action"));

    // ── the action list a caller is told about must be true ──────────────
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("slice bogusaction"));
        const QString e = errorOf(r);
        report("slice error lists the new actions",
               e.contains(QStringLiteral("squelch")) && e.contains(QStringLiteral("tone"))
                   && e.contains(QStringLiteral("offset")),
               e);
        // These three worked long before this change and were missing from the
        // list, which is how a caller concludes a working action does not exist.
        report("slice error lists the previously-omitted actions",
               e.contains(QStringLiteral("filter")) && e.contains(QStringLiteral("agc"))
                   && e.contains(QStringLiteral("dsp")),
               e);
    }

    socket.disconnectFromServer();
    server.stop();

    std::printf("\n%s\n", g_failed == 0 ? "all rows passed" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
