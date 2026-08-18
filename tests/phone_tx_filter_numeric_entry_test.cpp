// TX filter direct numeric entry (#3627).
//
// The PHONE applet's low/high cut readouts are ScrollableLabels. Before this
// they could only be stepped in 50 Hz snaps; now they can be typed into.
// These rows pin the two things that are easy to get wrong: the cross-bound
// clamp a typed value must obey (the same one the step buttons apply at their
// call site), and the fact that editing is OFF for every OTHER ScrollableLabel
// in the app — RIT/XIT, step size, RTTY mark/shift all share this class.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/GuardedSlider.h"
#include "gui/PhoneApplet.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QLineEdit>
#include <QSignalSpy>
#include <QCoreApplication>
#include <QTest>

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* label)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", label);
    if (!condition)
        ++failures;
}

ScrollableLabel* cutLabel(PhoneApplet& applet, const char* accessibleName)
{
    for (ScrollableLabel* l : applet.findChildren<ScrollableLabel*>()) {
        if (l->accessibleName() == QLatin1String(accessibleName))
            return l;
    }
    return nullptr;
}

// Open the editor and hand back the CURRENT one.
//
// Always drain deferred deletes first: findChild<QLineEdit*>() will otherwise
// return an editor a previous edit already closed but that has not been
// deleted yet, and every assertion then runs against a widget the label no
// longer owns. A running app gets this for free from the event loop between
// two operator actions.
QLineEdit* openEditor(ScrollableLabel* label)
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    label->beginEdit();
    return label->findChild<QLineEdit*>();
}

// Type into an open editor and commit with Return.
void typeAndCommit(ScrollableLabel* label, const QString& text)
{
    QLineEdit* ed = openEditor(label);
    if (!ed)
        return;
    ed->setText(text);
    QTest::keyClick(ed, Qt::Key_Return);
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("phone-tx-filter-numeric-entry-test"));
    QApplication app(argc, argv);
    AppSettings::instance().load();

    TransmitModel model;
    PhoneApplet applet;
    applet.setTransmitModel(&model);

    ScrollableLabel* low  = cutLabel(applet, "TX low cut frequency");
    ScrollableLabel* high = cutLabel(applet, "TX high cut frequency");
    check(low != nullptr,  "low cut readout exists");
    check(high != nullptr, "high cut readout exists");
    if (!low || !high)
        return 1;

    check(low->isEditable(),  "low cut readout is editable (#3627)");
    check(high->isEditable(), "high cut readout is editable (#3627)");

    // Without a styler the editor renders as a bare system line edit in the
    // middle of a dark panel. The rule must select QLineEdit — Qt matches
    // stylesheet selectors on widget class, so a QLabel rule styles nothing.
    check(low->hasEditorStyler() && high->hasEditorStyler(),
          "both readouts style their editor rather than leaving it unthemed");
    if (QLineEdit* ed = openEditor(low)) {
        check(ed->styleSheet().contains(QLatin1String("QLineEdit")),
              "the editor is themed with a QLineEdit rule");
        check(!ed->styleSheet().contains(QLatin1String("{{")),
              "theme tokens are resolved, not passed through raw");
        QTest::keyClick(ed, Qt::Key_Escape);
    }

    // ── The feature: an exact value, in one action ────────────────────────
    model.setTxFilter(50, 3300);
    typeAndCommit(low, QStringLiteral("237"));
    check(model.txFilterLow() == 237,
          "a typed low cut reaches the model exactly (not snapped to 50 Hz)");

    typeAndCommit(high, QStringLiteral("2843"));
    check(model.txFilterHigh() == 2843,
          "a typed high cut reaches the model exactly");

    // The issue's motivating example: 100-2900 in two actions, not dozens.
    model.setTxFilter(50, 3300);
    typeAndCommit(low, QStringLiteral("100"));
    typeAndCommit(high, QStringLiteral("2900"));
    check(model.txFilterLow() == 100 && model.txFilterHigh() == 2900,
          "100-2900 Hz is reachable by typing both bounds");

    // ── Cross-bound clamp: same rule the step buttons enforce ─────────────
    model.setTxFilter(50, 3300);
    typeAndCommit(low, QStringLiteral("9000"));
    check(model.txFilterLow() == 3250,
          "a low cut above high is clamped to high - 50, never crossed");
    // The reason that clamp lives at the call site: the model would resolve
    // the same crossed pair by keeping low and dragging high up to 9050.
    check(model.txFilterHigh() == 3300,
          "clamping a typed low cut leaves the untouched high cut where it was");

    model.setTxFilter(500, 3300);
    typeAndCommit(high, QStringLiteral("200"));
    check(model.txFilterHigh() == 550,
          "a high cut below low is clamped to low + 50, never crossed");

    // ── The stale-label trap ──────────────────────────────────────────────
    // Typing a value that clamps onto the CURRENT value changes nothing, so
    // no model signal fires and syncFromModel() would never run on its own.
    // Without the unconditional re-sync the label keeps showing the rejected
    // number while the radio is on a different one.
    model.setTxFilter(3250, 3300);
    typeAndCommit(low, QStringLiteral("9999"));
    check(model.txFilterLow() == 3250,
          "clamping onto the current value leaves the model alone");
    check(low->text() == QStringLiteral("3250"),
          "the label shows the model value, not the rejected keystrokes");

    // Out-of-range input must not deadlock Return. QIntValidator calls
    // 20000 Intermediate, and QLineEdit will not emit on Intermediate input,
    // so without the clamping fixup() Enter would do nothing at all.
    model.setTxFilter(50, 3300);
    typeAndCommit(low, QStringLiteral("20000"));
    check(!low->isEditing(), "Enter commits an out-of-range value instead of hanging");
    check(model.txFilterLow() == 3250 && model.txFilterHigh() == 3300,
          "an out-of-range typed value is clamped, not silently dropped");

    // ── Cancel routes ─────────────────────────────────────────────────────
    //
    // Esc is the row that shipped broken, so it is tested the way the APP
    // behaves, not the way a bare harness does. The main window carries
    // QShortcut(Qt::Key_Escape, window()) (CwxPanel, DvkPanel). Qt offers a
    // ShortcutOverride first and, if nothing accepts it, the shortcut fires
    // and the focused widget never receives a KeyPress — which is why the
    // original keyPressEvent handler never ran in the real app while passing
    // a test that delivered Esc straight to the widget.
    //
    // Accepting the ShortcutOverride is therefore the whole fix, and it is
    // asserted directly: without it this row fails and Esc stays dead.
    model.setTxFilter(150, 3300);
    if (QLineEdit* ed = openEditor(low)) {
        ed->setText(QStringLiteral("2000"));
        QKeyEvent probe(QEvent::ShortcutOverride, Qt::Key_Escape, Qt::NoModifier);
        probe.setAccepted(false);
        QApplication::sendEvent(ed, &probe);
        check(probe.isAccepted(),
              "Esc is claimed from the window shortcut (ShortcutOverride accepted)");
        QTest::keyClick(ed, Qt::Key_Escape);
    }
    check(!low->isEditing(), "Esc closes the editor");
    check(model.txFilterLow() == 150, "Esc abandons the edit without committing");
    check(low->text() == QStringLiteral("150"), "Esc leaves the displayed value intact");

    // Qt::Key_Cancel is the same gesture on platforms that send it; VfoWidget
    // treats the two together and so do we.
    model.setTxFilter(150, 3300);
    if (QLineEdit* ed = openEditor(low)) {
        ed->setText(QStringLiteral("2000"));
        QTest::keyClick(ed, Qt::Key_Cancel);
    }
    check(!low->isEditing() && model.txFilterLow() == 150,
          "Key_Cancel abandons the edit the same way Esc does");

    // ── Lock gate: a locked panel is being scrolled, not operated (#745) ──
    ControlsLock::setLocked(true);
    QTest::mouseDClick(low, Qt::LeftButton);
    check(!low->isEditing(), "a locked panel does not open the editor (#745)");
    ControlsLock::setLocked(false);
    QTest::mouseDClick(low, Qt::LeftButton);
    check(low->isEditing(), "unlocking restores double-click-to-type");
    // Flush again before reaching for the editor: the row above opened a NEW
    // one, but an earlier closed editor can still be pending deleteLater and
    // findChild() returns whichever it meets first.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QTest::keyClick(low->findChild<QLineEdit*>(), Qt::Key_Escape);
    check(!low->isEditing(), "the editor closes again after the lock test");

    // ── Blast radius: every OTHER ScrollableLabel is untouched ────────────
    // RIT/XIT (RxApplet), step size (RxApplet) and RTTY mark/shift
    // (VfoWidget) all use this class and must stay display-only.
    ScrollableLabel plain(QStringLiteral("+0 Hz"));
    check(!plain.isEditable(),
          "a ScrollableLabel is NOT editable by default (RIT/XIT/step/RTTY)");
    QTest::mouseDClick(&plain, Qt::LeftButton);
    check(!plain.isEditing(),
          "double-clicking a default ScrollableLabel does nothing");

    // ── The existing affordance still works ───────────────────────────────
    model.setTxFilter(200, 3300);
    QSignalSpy wheelSpy(low, &ScrollableLabel::scrolled);
    QWheelEvent up(QPointF(5, 5), QPointF(5, 5), QPoint(), QPoint(0, 120),
                   Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(low, &up);
    check(wheelSpy.count() == 1, "making the label editable did not break wheel stepping");

    std::printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
