// doubleClick / doubleClickAt verbs (#5068).
//
// The bridge could synthesize every other pointer gesture but never built a
// MouseButtonDblClick, so a control that opens on double-click — the VFO DIG
// offset inline editor, the TX filter cut readouts — could not be driven at
// all. Two clickAt calls do NOT substitute: Qt does not promote a pair of
// synthetic press/release sequences into a double-click, and a widget that
// overrides mouseDoubleClickEvent never hears one. That is the assertion this
// file exists for.

// AutomationServer's inline QPointer setters require these QObject-derived
// types to be complete before its header is parsed.
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "models/RadioModel.h"
#include "core/AutomationServer.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>
#include <QWidget>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = QString())
{
    std::printf("%s %-52s %s\n", ok ? "[ OK ]" : "[FAIL]", name, qPrintable(detail));
    if (!ok)
        ++g_failed;
}

struct Recorded {
    QEvent::Type type{QEvent::None};
    QPoint position;
};

// Counts double-clicks separately, because that is the whole point: a widget
// only ever learns about one through mouseDoubleClickEvent.
class RecordingWidget final : public QWidget
{
public:
    using QWidget::QWidget;

    QVector<Recorded> events;
    int doubleClicks{0};

protected:
    void mousePressEvent(QMouseEvent* e) override       { record(e); e->accept(); }
    void mouseReleaseEvent(QMouseEvent* e) override     { record(e); e->accept(); }
    void mouseDoubleClickEvent(QMouseEvent* e) override { ++doubleClicks; record(e); e->accept(); }

private:
    void record(const QMouseEvent* e)
    {
        events.append({e->type(), e->position().toPoint()});
    }
};

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

// The bridge defers the synthetic events onto the GUI loop, so let them land.
void settle()
{
    for (int i = 0; i < 8; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

bool isDoubleClickSequence(const QVector<Recorded>& e, QPoint at)
{
    // Qt's own order for a double-click: Press, Release, DblClick, Release.
    // The window system sends the DblClick INSTEAD of the second Press.
    const QVector<QEvent::Type> want{QEvent::MouseButtonPress,
                                     QEvent::MouseButtonRelease,
                                     QEvent::MouseButtonDblClick,
                                     QEvent::MouseButtonRelease};
    if (e.size() != want.size())
        return false;
    for (qsizetype i = 0; i < want.size(); ++i) {
        if (e.at(i).type != want.at(i) || e.at(i).position != at)
            return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QTemporaryDir testRoot;
    if (!testRoot.isValid()) {
        std::printf("[FAIL] create temporary test root\n");
        return 1;
    }
    const QByteArray root = testRoot.path().toUtf8();
    qputenv("HOME", root);
    qputenv("XDG_CONFIG_HOME", root);
    qputenv("TMPDIR", root);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);

    RecordingWidget target;
    target.setObjectName(QStringLiteral("automationDoubleClickTarget"));
    target.resize(100, 80);
    target.show();
    QCoreApplication::processEvents();

    AutomationServer server;
    const QString serverName = QStringLiteral("aethersdr-dblclick-test-%1")
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

    // ── doubleClick <target> — centre by default ─────────────────────────
    target.events.clear();
    target.doubleClicks = 0;
    const QJsonObject centre =
        request(socket, QByteArrayLiteral("doubleClick automationDoubleClickTarget"));
    settle();
    report("doubleClick <target> is accepted",
           centre.value(QStringLiteral("ok")).toBool(),
           QString::fromUtf8(QJsonDocument(centre).toJson(QJsonDocument::Compact)));
    report("doubleClick lands on the widget centre",
           isDoubleClickSequence(target.events, target.rect().center()),
           QStringLiteral("events=%1").arg(target.events.size()));

    // THE row this verb exists for. A widget only ever learns about a
    // double-click through mouseDoubleClickEvent, and no amount of clickAt
    // produces one.
    report("mouseDoubleClickEvent actually fires", target.doubleClicks == 1,
           QStringLiteral("count=%1").arg(target.doubleClicks));

    // ── doubleClickAt <target> <x> <y> — explicit point ──────────────────
    target.events.clear();
    target.doubleClicks = 0;
    const QJsonObject at = request(
        socket, QByteArrayLiteral("doubleClickAt automationDoubleClickTarget 10 12"));
    settle();
    report("doubleClickAt <target> <x> <y> is accepted",
           at.value(QStringLiteral("ok")).toBool(),
           QString::fromUtf8(QJsonDocument(at).toJson(QJsonDocument::Compact)));
    report("doubleClickAt lands on the named point",
           isDoubleClickSequence(target.events, QPoint(10, 12)),
           QStringLiteral("events=%1").arg(target.events.size()));
    report("doubleClickAt raises mouseDoubleClickEvent too",
           target.doubleClicks == 1,
           QStringLiteral("count=%1").arg(target.doubleClicks));

    // ── clickAt is unchanged: single click, no DblClick ──────────────────
    target.events.clear();
    target.doubleClicks = 0;
    request(socket, QByteArrayLiteral("clickAt automationDoubleClickTarget 10 12"));
    settle();
    report("clickAt still sends press+release only",
           target.events.size() == 2
               && target.events.at(0).type == QEvent::MouseButtonPress
               && target.events.at(1).type == QEvent::MouseButtonRelease,
           QStringLiteral("events=%1").arg(target.events.size()));
    report("clickAt raises no double-click", target.doubleClicks == 0,
           QStringLiteral("count=%1").arg(target.doubleClicks));

    // ── refusals ─────────────────────────────────────────────────────────
    const QJsonObject noTarget = request(socket, QByteArrayLiteral("doubleClick"));
    report("doubleClick without a target is refused",
           !noTarget.value(QStringLiteral("ok")).toBool());

    const QJsonObject missing =
        request(socket, QByteArrayLiteral("doubleClick automationNoSuchWidget"));
    report("doubleClick on an unknown widget is refused",
           !missing.value(QStringLiteral("ok")).toBool());

    // Disabled widgets drop input events, so a click there is a silent no-op
    // that must not be reported as ok — the guard is inherited from clickAt.
    target.setEnabled(false);
    target.events.clear();
    const QJsonObject disabled =
        request(socket, QByteArrayLiteral("doubleClick automationDoubleClickTarget"));
    settle();
    report("doubleClick on a disabled widget is refused",
           !disabled.value(QStringLiteral("ok")).toBool()
               && target.events.isEmpty(),
           QString::fromUtf8(QJsonDocument(disabled).toJson(QJsonDocument::Compact)));
    target.setEnabled(true);

    // ── the verb is discoverable ─────────────────────────────────────────
    const QJsonObject verbs = request(socket, QByteArrayLiteral("verbs"));
    const QString verbText =
        QString::fromUtf8(QJsonDocument(verbs).toJson(QJsonDocument::Compact));
    report("doubleClick is listed by the verbs registry",
           verbText.contains(QStringLiteral("doubleClick")));

    socket.disconnectFromServer();
    server.stop();

    std::printf("%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
