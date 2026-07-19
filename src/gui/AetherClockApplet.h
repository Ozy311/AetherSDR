#pragma once

// AetherClock strip applet: alignment scope (dominant) + status row +
// collapsed settings drawer, visually modeled on WaveApplet. The applet
// reads/writes the AetherClock MODEL + engine ACTION surface only — no DSP,
// no radio access, no DAX registration of any kind; engine/model
// construction and the DAX-provider wiring live in
// MainWindow_AetherClock.cpp.
//
// Slice binding: the applet listens on AppletPanel::setSlice forwarding —
// the user picks the listening slice by strip selection; the applet never
// creates or grabs a slice.

#include <QPointer>
#include <QWidget>

class QFrame;
class QLabel;
class QPushButton;
class GuardedComboBox;

namespace AetherSDR {

class AetherClockEngine;
class AetherClockModel;
class ClockAlignmentWidget;
class SliceModel;

class AetherClockApplet : public QWidget {
    Q_OBJECT

public:
    explicit AetherClockApplet(QWidget* parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // Wiring entry (called from MainWindow_AetherClock.cpp after engine +
    // model construction). Connects model change signals → status row,
    // engine alignmentFrame → scope, and enables the action controls.
    // Detaches any previously attached pair.
    void attach(AetherClockEngine* engine, AetherClockModel* model);

    ClockAlignmentWidget* alignmentWidget() const { return m_scope; }

public slots:
    // AppletPanel::setSlice forwarding (per-applet, explicit).
    void setSlice(SliceModel* slice);

private:
    void buildUi();
    void buildSettingsDrawer();
    void setSettingsExpanded(bool expanded);
    void updateStartStopUi();
    void applyPresetSelection();  // combo → engine applyStationPreset

    QPointer<AetherClockEngine> m_engine;
    QPointer<AetherClockModel> m_model;
    QPointer<SliceModel> m_slice;

    ClockAlignmentWidget* m_scope{nullptr};
    QLabel* m_utcValue{nullptr};      // decoded UTC, monospace
    QLabel* m_offsetValue{nullptr};   // signed offset, glanceable
    QLabel* m_lockLed{nullptr};       // state-colored LED dot
    QLabel* m_stationTag{nullptr};    // WWV / WWVH / WWVB / --
    QFrame* m_settingsDrawer{nullptr};
    QPushButton* m_drawerToggle{nullptr};
    GuardedComboBox* m_presetCombo{nullptr};
    QPushButton* m_tuneButton{nullptr};
    QPushButton* m_startStopButton{nullptr};
    QLabel* m_presetNote{nullptr};  // per-preset dial note (+ WWVB AGC note)
};

} // namespace AetherSDR
