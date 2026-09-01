// CW controls hold their value on a backend that never echoes them (#5256).
//
// WHY THIS TEST EXISTS. The CW group's settings reach the radio as wire text
// ("cw mode 1"), and on a FLEX the firmware echoes `iambic_mode=1` back in the
// next transmit status, which TransmitModel::apply() folds into local state.
// That echo is the ONLY thing that made the local value correct, and it made it
// correct by accident: `iambic` and `iambic_mode` are parsed in exactly one
// place in the tree, FlexBackend::decodeTransmitState. Hermes-Lite 2 has no
// such echo, so on HL2 a control whose only action is to send wire text leaves
// the model at its previous value forever — the dialog reseeds from the model
// and shows the old setting, and MainWindow_Session's syncLocalKeyerToRadio
// never runs because phoneStateChanged never fires, so the keyer keeps the old
// mode too. That is #5256 as reported: Mode B "does not stick" AND does not key.
//
// The setters below already do the right thing — they update local state, emit
// phoneStateChanged, and THEN emit the wire command — and TransmitModel::
// setCwIambic even carries a comment saying why. This file pins that property
// so it cannot be refactored away as redundant: on any backend without an echo,
// removing the optimistic local update silently restores #5256 with no test
// failure anywhere else in the suite.
//
// Both halves are asserted on every row, because they protect different radios:
//   - local state + phoneStateChanged  -> HL2 and any other non-echoing backend
//   - the exact wire command still sent -> FLEX, whose firmware remains
//     authoritative and whose echo supersedes the optimistic value (Principle II)
//
// Socket-free: TransmitModel is constructed directly, no radio, no connection.

#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = QString())
{
    std::printf("%s %-62s %s\n", ok ? "[ OK ]" : "[FAIL]", name, qPrintable(detail));
    if (!ok)
        ++g_failed;
}

// One CW control, exercised the way the Radio Setup dialog exercises it.
// `apply` performs the model call; the row then asserts the three things that
// have to be simultaneously true for both radio families.
struct Row {
    const char* name;
    void (*apply)(TransmitModel&);
    bool (*readBack)(const TransmitModel&);   // did local state actually move?
    const char* expectedCommand;              // what must still reach a FLEX
};

const Row kRows[] = {
    {"iambic mode B",
     [](TransmitModel& t) { t.setCwIambicMode(1); },
     [](const TransmitModel& t) { return t.cwIambicMode() == 1; },
     "cw mode 1"},

    {"iambic mode A",
     [](TransmitModel& t) { t.setCwIambicMode(0); },
     [](const TransmitModel& t) { return t.cwIambicMode() == 0; },
     "cw mode 0"},

    {"iambic disabled",
     [](TransmitModel& t) { t.setCwIambic(false); },
     [](const TransmitModel& t) { return t.cwIambic() == false; },
     "cw iambic 0"},

    {"swap paddles",
     [](TransmitModel& t) { t.setCwSwapPaddles(true); },
     [](const TransmitModel& t) { return t.cwSwapPaddles() == true; },
     "cw swap 1"},

    {"CWL enabled",
     [](TransmitModel& t) { t.setCwlEnabled(true); },
     [](const TransmitModel& t) { return t.cwlEnabled() == true; },
     "cw cwl_enabled 1"},
};

// Each row starts from a model whose value is the OPPOSITE of what the row
// sets, so "local state moved" is a real transition and not a value the
// default already happened to hold.
void seedOpposite(TransmitModel& t, const char* name)
{
    const QString n = QString::fromLatin1(name);
    if (n == "iambic mode B")      t.setCwIambicMode(0);
    else if (n == "iambic mode A") t.setCwIambicMode(1);
    else if (n == "iambic disabled") t.setCwIambic(true);
    else if (n == "swap paddles")  t.setCwSwapPaddles(false);
    else if (n == "CWL enabled")   t.setCwlEnabled(false);
}

void runRow(const Row& row)
{
    TransmitModel tx;
    seedOpposite(tx, row.name);

    // The seed uses the same setter under test, so an inert setter would leave
    // the model at a default that some rows happen to want — and the row would
    // pass having proved nothing. Assert the seed actually established the
    // OPPOSITE state before the row does its own work.
    if (row.readBack(tx)) {
        report(QStringLiteral("%1: seed established the opposite state")
                   .arg(QString::fromLatin1(row.name)).toUtf8().constData(),
               false,
               QStringLiteral("already at the target value before the row ran"));
        return;
    }

    int phoneChanges = 0;
    QObject::connect(&tx, &TransmitModel::phoneStateChanged,
                     [&phoneChanges] { ++phoneChanges; });
    QStringList commands;
    QObject::connect(&tx, &TransmitModel::commandReady,
                     [&commands](const QString& c) { commands << c; });

    row.apply(tx);

    report(QStringLiteral("%1: local state holds without an echo")
               .arg(QString::fromLatin1(row.name)).toUtf8().constData(),
           row.readBack(tx),
           QStringLiteral("model did not move; on HL2 nothing else ever moves it"));

    report(QStringLiteral("%1: phoneStateChanged fires (drives the local keyer)")
               .arg(QString::fromLatin1(row.name)).toUtf8().constData(),
           phoneChanges == 1,
           QStringLiteral("emitted %1 times, expected 1").arg(phoneChanges));

    report(QStringLiteral("%1: wire command still sent (FLEX stays authoritative)")
               .arg(QString::fromLatin1(row.name)).toUtf8().constData(),
           commands.contains(QString::fromLatin1(row.expectedCommand)),
           QStringLiteral("expected \"%1\", got [%2]")
               .arg(QString::fromLatin1(row.expectedCommand), commands.join(QStringLiteral(", "))));
}

// A repeat of the value already held must still re-send the command (the radio
// may have missed it) but must NOT claim a state change, or the keyer resyncs
// on every redundant click.
void runIdempotentRow()
{
    TransmitModel tx;
    tx.setCwIambicMode(1);

    int phoneChanges = 0;
    QObject::connect(&tx, &TransmitModel::phoneStateChanged,
                     [&phoneChanges] { ++phoneChanges; });
    QStringList commands;
    QObject::connect(&tx, &TransmitModel::commandReady,
                     [&commands](const QString& c) { commands << c; });

    tx.setCwIambicMode(1);

    report("repeat set: no spurious phoneStateChanged",
           phoneChanges == 0,
           QStringLiteral("emitted %1 times, expected 0").arg(phoneChanges));
    report("repeat set: command re-sent anyway",
           commands.contains(QStringLiteral("cw mode 1")),
           QStringLiteral("got [%1]").arg(commands.join(QStringLiteral(", "))));
}

// The mode is a two-value enum on the wire; anything else would be sent
// verbatim to the radio without the clamp.
void runClampRow()
{
    TransmitModel tx;
    QStringList commands;
    QObject::connect(&tx, &TransmitModel::commandReady,
                     [&commands](const QString& c) { commands << c; });

    tx.setCwIambicMode(7);
    report("out-of-range mode clamps to B",
           tx.cwIambicMode() == 1 && commands.contains(QStringLiteral("cw mode 1")),
           QStringLiteral("mode=%1 commands=[%2]")
               .arg(tx.cwIambicMode()).arg(commands.join(QStringLiteral(", "))));

    TransmitModel tx2;
    QStringList commands2;
    QObject::connect(&tx2, &TransmitModel::commandReady,
                     [&commands2](const QString& c) { commands2 << c; });
    tx2.setCwIambicMode(1);
    tx2.setCwIambicMode(-3);
    report("negative mode clamps to A",
           tx2.cwIambicMode() == 0 && commands2.contains(QStringLiteral("cw mode 0")),
           QStringLiteral("mode=%1").arg(tx2.cwIambicMode()));
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    for (const Row& row : kRows)
        runRow(row);
    runIdempotentRow();
    runClampRow();

    if (g_failed == 0)
        std::printf("\nAll CW local-state rows passed.\n");
    else
        std::printf("\n%d assertion(s) failed.\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
