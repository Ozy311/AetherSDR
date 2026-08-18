#pragma once

#include "DragValuePopup.h"

#include <QSlider>
#include <QComboBox>
#include <QAbstractItemView>
#include <QLabel>
#include <QLineEdit>
#include <QIntValidator>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QWheelEvent>
#include <functional>
#include <utility>

// Global lock for sidebar controls — when locked, sliders, combo boxes,
// and scrollable labels ignore wheel/mouse events so the user can scroll
// the applet panel without accidentally changing values. (#745)
class ControlsLock {
public:
    static bool isLocked() { return s_locked; }
    static void setLocked(bool locked) { s_locked = locked; }
private:
    static inline bool s_locked = false;
};

// QSlider subclass that always consumes wheel events, even at min/max
// boundaries. Prevents scroll from propagating to parent widgets (e.g.
// SpectrumWidget tuning the VFO when a slider bottoms out). (#570)
// When controls are locked (#745), ignores wheel events and lets the
// parent scroll area handle them.
class GuardedSlider : public QSlider {
public:
    using DragValueFormatter = std::function<QString(int)>;

    explicit GuardedSlider(QWidget* parent = nullptr)
        : QSlider(parent)
    {
    }

    explicit GuardedSlider(Qt::Orientation orientation, QWidget* parent = nullptr)
        : QSlider(orientation, parent)
    {
    }

    void setDragValueFormatter(DragValueFormatter formatter) {
        m_dragValueFormatter = std::move(formatter);
    }

    void setDragValuePopupEnabled(bool enabled) {
        m_dragValuePopupEnabled = enabled;
        if (!enabled && m_dragValuePopup)
            m_dragValuePopup->hideNow();
    }

    // Flash the value badge in response to a keyboard step, then let it
    // linger and fade with the same timeout as a mouse release.  Keyboard
    // nudges for these sliders are routed through MainWindow's shortcut
    // lease (so global operating shortcuts can resume), so the lease handler
    // calls this to mirror the mouse-drag readout. (#3303 follow-up)
    void flashDragValue() {
        if (!m_dragValuePopupEnabled)
            return;
        showDragValuePopup(mapToGlobal(rect().center()));
        if (m_dragValuePopup)
            m_dragValuePopup->linger();
    }

    void mousePressEvent(QMouseEvent* ev) override {
        if (ControlsLock::isLocked()) {
            ev->ignore();
            return;
        }
        QSlider::mousePressEvent(ev);
        if (ev->button() == Qt::LeftButton) {
            m_dragValueActive = true;
            showDragValuePopup(ev->globalPosition().toPoint());
        }
    }
    void mouseMoveEvent(QMouseEvent* ev) override {
        if (ControlsLock::isLocked()) {
            ev->ignore();
            return;
        }
        QSlider::mouseMoveEvent(ev);
        if (m_dragValueActive || isSliderDown())
            showDragValuePopup(ev->globalPosition().toPoint());
    }
    void mouseReleaseEvent(QMouseEvent* ev) override {
        const bool wasActive = m_dragValueActive;
        QSlider::mouseReleaseEvent(ev);
        if (wasActive && ev->button() == Qt::LeftButton) {
            showDragValuePopup(ev->globalPosition().toPoint());
            m_dragValueActive = false;
            if (m_dragValuePopup)
                m_dragValuePopup->linger();
        }
    }
    void wheelEvent(QWheelEvent* ev) override {
        if (ControlsLock::isLocked()) {
            ev->ignore();
            return;
        }
        // Use singleStep (default 1) instead of pageStep (default 10) so
        // that mouse-wheel adjustments are fine-grained (#1026).
        int delta = ev->angleDelta().y();
        if (delta != 0)
            setValue(value() + (delta > 0 ? singleStep() : -singleStep()));
        ev->accept();
    }

protected:
    // Below is protected (not private) so subclasses that override the
    // mouse handlers for custom drag behaviour — e.g. WaterfallRateSlider's
    // click-to-jump positioning — can still drive the same drag-value popup
    // instead of silently losing it.
    QString dragValueText() const {
        if (m_dragValueFormatter)
            return m_dragValueFormatter(value());
        return QString::number(value());
    }

    QPoint dragValueAnchor(const QPoint& fallbackGlobal) const {
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const QRect handle = style()->subControlRect(
            QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
        if (handle.isValid())
            return mapToGlobal(handle.center());
        return fallbackGlobal;
    }

    void showDragValuePopup(const QPoint& fallbackGlobal) {
        if (!m_dragValuePopupEnabled)
            return;
        if (!m_dragValuePopup)
            m_dragValuePopup = new AetherSDR::DragValuePopup(this);
        m_dragValuePopup->showValue(dragValueAnchor(fallbackGlobal),
                                    dragValueText());
    }

    DragValueFormatter m_dragValueFormatter;
    AetherSDR::DragValuePopup* m_dragValuePopup{nullptr};
    bool m_dragValuePopupEnabled{true};
    bool m_dragValueActive{false};
};

// QComboBox subclass that only responds to wheel events when the dropdown
// popup is open. Prevents accidental value changes when scrolling the applet
// panel, but allows normal wheel scrolling through the list when the user
// has clicked to open the dropdown. (#570, #676)
// When controls are locked (#745), also blocks mouse press to prevent
// opening the dropdown.
class GuardedComboBox : public QComboBox {
public:
    using QComboBox::QComboBox;
    void wheelEvent(QWheelEvent* ev) override {
        if (ControlsLock::isLocked()) {
            ev->ignore();
            return;
        }
        if (view() && view()->isVisible())
            QComboBox::wheelEvent(ev);  // popup open — scroll the list
        else
            ev->ignore();  // popup closed — let parent handle scroll
    }
    void mousePressEvent(QMouseEvent* ev) override {
        if (ControlsLock::isLocked()) {
            ev->ignore();
            return;
        }
        QComboBox::mousePressEvent(ev);
    }
};

// QIntValidator reports an out-of-range number as Intermediate, not Invalid
// — so a QLineEdit happily holds "20000" but then refuses to emit
// returnPressed/editingFinished, because those require Acceptable input.
// Measured against Qt 6.10: validate("20000") with a 0..10000 range returns
// Intermediate. Without a fixup() the operator types a too-large value, hits
// Enter, and nothing whatsoever happens.
//
// Qt calls fixup() on Return precisely for this, so clamping there turns a
// dead keypress into the nearest legal value.
class ClampingIntValidator : public QIntValidator {
public:
    ClampingIntValidator(int minimum, int maximum, QObject* parent = nullptr)
        : QIntValidator(minimum, maximum, parent) {}

    void fixup(QString& input) const override {
        bool ok = false;
        const int v = input.toInt(&ok);
        if (ok)
            input = QString::number(qBound(bottom(), v, top()));
        // Deliberately NOT chaining to QIntValidator::fixup(): it inserts
        // locale group separators ("20000" -> "20,000"), and the caller
        // parses this back with toInt(), which rejects them. Measured on
        // Qt 6.10 — chaining silently dropped every out-of-range commit.
    }
};

// QLabel subclass that emits scrolled(int steps) on wheel events and
// always consumes them. Used for RIT/XIT/pitch numeric displays. (#619)
// When controls are locked (#745), ignores wheel events.
//
// The label can also be typed into (#3627): call setEditable() with the
// accepted range and a double-click swaps a QLineEdit over the label.
// Editing is OFF by default, so every existing user — RIT/XIT, step size,
// RTTY mark/shift — keeps its display-only behaviour unchanged.
//
// A committed value is only ever a REQUEST. The owner clamps it against
// whatever relationship its own controls carry (e.g. TX low <= high - 50)
// and then re-syncs the text from the model, so the label can never show a
// number the model did not accept.
class ScrollableLabel : public QLabel {
    Q_OBJECT
public:
    using QLabel::QLabel;

    // Enable double-click-to-type. min/max bound the editor's validator;
    // the owner still applies any cross-control clamping on commit.
    void setEditable(int minValue, int maxValue) {
        m_editable = true;
        m_min = minValue;
        m_max = maxValue;
    }
    bool isEditable() const { return m_editable; }

    // The owner styles the editor, so it does not flash an unthemed system box
    // over a dark panel. A callback rather than a stylesheet string for two
    // reasons: the owner writes a QLineEdit rule directly (a QLabel rule would
    // not match a QLineEdit at all, since Qt selects on widget class), and the
    // stylesheet is applied by ThemeManager itself, which owns theming
    // and is where the colour audit expects it to live.
    void setEditorStyler(std::function<void(QWidget*)> styler) {
        m_editorStyler = std::move(styler);
    }
    bool hasEditorStyler() const { return static_cast<bool>(m_editorStyler); }

    void wheelEvent(QWheelEvent* ev) override {
        if (ControlsLock::isLocked()) {
            ev->ignore();
            return;
        }
        int delta = ev->angleDelta().y();
        if (delta > 0) emit scrolled(1);
        else if (delta < 0) emit scrolled(-1);
        ev->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent* ev) override {
        // Same gate as the wheel handler: a locked panel is being scrolled,
        // not operated. (#745)
        if (!m_editable || ControlsLock::isLocked()) {
            QLabel::mouseDoubleClickEvent(ev);
            return;
        }
        beginEdit();
        ev->accept();
    }

    // Exposed so a test (and any future keyboard route) can open the editor
    // without synthesising a double-click.
    void beginEdit() {
        if (!m_editable || m_editor)
            return;
        m_editor = new QLineEdit(text(), this);
        m_editor->setValidator(new ClampingIntValidator(m_min, m_max, m_editor));
        m_editor->setAlignment(alignment());
        if (m_editorStyler)
            m_editorStyler(m_editor);
        m_editor->setGeometry(rect());
        m_editor->selectAll();
        // Escape has to be claimed on the EDITOR, via ShortcutOverride — see
        // eventFilter() below for why a keyPressEvent override cannot work.
        m_editor->installEventFilter(this);
        connect(m_editor, &QLineEdit::editingFinished, this, [this]() { commitEdit(); });
        m_editor->show();
        m_editor->setFocus(Qt::MouseFocusReason);
    }

    bool isEditing() const { return m_editor != nullptr; }

signals:
    void scrolled(int direction);
    // A typed value the owner should clamp and push at the model.
    void editCommitted(int value);

protected:
    // Cancel the edit on Esc.
    //
    // This MUST filter the editor and MUST answer ShortcutOverride, not just
    // KeyPress. The main window carries `QShortcut(Qt::Key_Escape, window())`
    // (CwxPanel, DvkPanel), and Qt dispatches a shortcut by first offering a
    // ShortcutOverride event: if nothing accepts it, the shortcut fires and
    // the focused widget is never sent a KeyPress at all. A keyPressEvent
    // override on this label therefore never runs, which is exactly how this
    // shipped broken — Esc did nothing in the app while passing a unit test
    // that delivered the key straight to the widget with no shortcut present.
    //
    // Same shape as VfoWidget::eventFilter for its frequency direct-entry.
    bool eventFilter(QObject* obj, QEvent* ev) override {
        if (obj == m_editor
            && (ev->type() == QEvent::ShortcutOverride
                || ev->type() == QEvent::KeyPress)) {
            auto* ke = static_cast<QKeyEvent*>(ev);
            if (ke->key() == Qt::Key_Escape || ke->key() == Qt::Key_Cancel) {
                // Accepting the ShortcutOverride claims the key so the window
                // shortcut does not consume it; the KeyPress that follows is
                // what actually closes the editor.
                if (ev->type() == QEvent::KeyPress)
                    closeEditor();
                ev->accept();
                return true;
            }
        }
        return QLabel::eventFilter(obj, ev);
    }

private:
    // Tear the editor down. Clears the member FIRST: destroying a focused
    // QLineEdit emits editingFinished, and a null member is what stops that
    // re-entering commitEdit() and committing a value twice.
    void closeEditor() {
        if (!m_editor)
            return;
        QLineEdit* ed = m_editor;
        m_editor = nullptr;
        // hide() before deleteLater(): the delete only lands on the next
        // event-loop pass, and until then the editor is still a visible
        // child sitting on top of the label.
        ed->hide();
        ed->deleteLater();
    }

    void commitEdit() {
        if (!m_editor)
            return;
        const QString typed = m_editor->text().trimmed();
        closeEditor();
        bool ok = false;
        const int value = typed.toInt(&ok);
        // Empty or unparseable cancels: the label keeps whatever the model
        // last put there, so a stray double-click costs nothing.
        if (ok)
            emit editCommitted(value);
    }

    bool       m_editable{false};
    int        m_min{0};
    int        m_max{0};
    std::function<void(QWidget*)> m_editorStyler;
    QLineEdit* m_editor{nullptr};
};
