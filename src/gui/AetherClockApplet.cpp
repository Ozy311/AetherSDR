#include "AetherClockApplet.h"

#include "ClockAlignmentWidget.h"
#include "ComboStyle.h"
#include "GuardedSlider.h"  // GuardedComboBox
#include "core/AetherClockEngine.h"
#include "core/AetherClockSettings.h"
#include "core/ThemeManager.h"
#include "core/TimeFrameVoter.h"  // ClockStation / ClockLockState
#include "models/AetherClockModel.h"
#include "models/SliceModel.h"  // complete type required by QPointer<SliceModel> assignment

#include <QComboBox>
#include <QDateTime>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

// Per-item combo roles: the WWV/WWVB carrier (MHz) and the station enum
// value, so the Tune + Start actions can recover the full preset from the
// current selection without a parallel table.
constexpr int kCarrierRole = Qt::UserRole;
constexpr int kStationRole = Qt::UserRole + 1;

// Status-signal colours (NIST time-signal acquisition states). These are
// semantic indicator colours like the style guide's gauge zones, so they
// stay literal rather than routing through the structural token map.
constexpr const char* kLedNoSignal = "#405060";
constexpr const char* kLedAcquiring = "#ffb800";
constexpr const char* kLedLocked = "#00ff88";

// Shared themed button chrome (matches the applet style guide standard
// button: compact, bold, token-driven so it live-re-themes).
const QString& kButtonBase()
{
    static const QString s = ThemeManager::instance().resolve(
        "QPushButton { background: {{color.background.1}};"
        " border: 1px solid {{color.background.2}};"
        " border-radius: 3px; color: {{color.text.primary}};"
        " font-size: 10px; font-weight: bold; padding: 2px 4px; }"
        "QPushButton:hover { background: {{color.background.2}}; }");
    return s;
}

// Green "active" checked state — style guide's activate/running family
// (background #006040, text #00ff88, border #00a060).
const QString kStartActive =
    "QPushButton:checked { background-color: #006040; color: #00ff88;"
    " border: 1px solid #00a060; }";

const QString kDisabledBtn =
    "QPushButton:disabled { background-color: #1a1a2a; color: #556070;"
    " border: 1px solid #2a3040; }";

QLabel* makeSettingLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFixedWidth(52);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ThemeManager::instance().applyStyleSheet(
        label, "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");
    return label;
}

// Inset readout stylesheets (style guide inset pattern: dark well, subtle
// border, centred text). The warn variant swaps only the text colour to amber
// #ffb800 — the same semantic warning literal as the acquiring LED — so the DAX
// inset can flag a running-but-no-channel bound slice without losing its chrome.
const QString& kInsetStyle()
{
    static const QString s = QStringLiteral(
        "QLabel { font-size: 11px; background: #0a0a18;"
        " border: 1px solid #1e2e3e; border-radius: 3px;"
        " padding: 1px 4px; color: #c8d8e8; }");
    return s;
}
const QString& kInsetStyleWarn()
{
    static const QString s = QStringLiteral(
        "QLabel { font-size: 11px; background: #0a0a18;"
        " border: 1px solid #1e2e3e; border-radius: 3px;"
        " padding: 1px 4px; color: #ffb800; }");
    return s;
}

// Inset value readout (style guide inset pattern: dark well, subtle border,
// centred primary text).
QLabel* makeInsetReadout(QWidget* parent, int minWidth)
{
    auto* label = new QLabel(parent);
    label->setAlignment(Qt::AlignCenter);
    label->setMinimumWidth(minWidth);
    label->setStyleSheet(kInsetStyle());
    return label;
}

struct PresetChoice {
    ClockStation station = ClockStation::Wwv;
    double carrierMHz = 0.0;
};

PresetChoice currentChoice(const QComboBox* combo)
{
    PresetChoice c;
    if (!combo)
        return c;
    const int i = combo->currentIndex();
    if (i < 0)
        return c;
    c.carrierMHz = combo->itemData(i, kCarrierRole).toDouble();
    c.station = ClockStation(combo->itemData(i, kStationRole).toInt());
    return c;
}

void applyLockLed(QLabel* led, ClockLockState state)
{
    if (!led)
        return;
    const char* colour = kLedNoSignal;
    switch (state) {
    case ClockLockState::Acquiring:
        colour = kLedAcquiring;
        break;
    case ClockLockState::Locked:
        colour = kLedLocked;
        break;
    case ClockLockState::NoSignal:
    default:
        colour = kLedNoSignal;
        break;
    }
    led->setStyleSheet(
        QStringLiteral("QLabel { background-color: %1; border-radius: 5px; }")
            .arg(QLatin1String(colour)));
}

void applyStationTag(QLabel* tag, const QString& stationName)
{
    if (!tag)
        return;
    tag->setText(stationName == QStringLiteral("Unknown") ? QStringLiteral("--")
                                                          : stationName);
}

void applyOffsetReadout(QLabel* label, const QDateTime& utc, double offsetMs)
{
    if (!label)
        return;
    if (!utc.isValid()) {
        label->setText(QStringLiteral("--"));
        return;
    }
    // + = host clock BEHIND the broadcast (engine offset convention).
    label->setText(QString::asprintf("%+.2f s", offsetMs / 1000.0));
}

// Decode age for the trust line: seconds while under 100 ("6s"), then whole
// minutes ("2m") so the compact label never widens the status row.
QString fmtDecodeAge(qint64 ageMs)
{
    const qint64 ageS = ageMs < 0 ? 0 : ageMs / 1000;
    if (ageS < 100)
        return QStringLiteral("%1s").arg(ageS);
    return QStringLiteral("%1m").arg(ageS / 60);
}

// Listening dial (MHz) formatted for the preset note — "9.999 MHz",
// "2.499 MHz", "0.059 MHz" — trailing zeros trimmed sensibly.
QString fmtDialMHz(double mhz)
{
    QString s = QString::number(mhz, 'f', 3);
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(QLatin1Char('.')))
            s.chop(1);
    }
    return s + QStringLiteral(" MHz");
}

// Tune-button label + the always-visible per-preset dial note follow the
// current preset selection.
void refreshPresetUi(QComboBox* combo, QPushButton* tuneBtn, QLabel* note)
{
    if (!combo)
        return;
    if (tuneBtn) {
        // U+2192 rightwards arrow — a text glyph, not an icon.
        tuneBtn->setText(
            QStringLiteral("Tune slice → %1").arg(combo->currentText()));
    }
    if (note) {
        const PresetChoice c = currentChoice(combo);
        const QString dial =
            fmtDialMHz(AetherClockEngine::listeningDialMHz(c.carrierMHz));
        // The deliberate carrier−1 kHz USB dial (post-demod high-pass
        // workaround) must read as intent, not error.
        QString text =
            QStringLiteral("dial %1 USB — 1 kHz below carrier").arg(dial);
        if (c.station == ClockStation::Wwvb)
            text += QStringLiteral(" · AGC off on tune");
        note->setText(text);
    }
}

} // namespace

AetherClockApplet::AetherClockApplet(QWidget* parent)
    : QWidget(parent)
{
    buildUi();
}

QSize AetherClockApplet::sizeHint() const
{
    const int scopeH = m_scope ? m_scope->sizeHint().height() : 150;
    const int drawerH = (m_settingsDrawer && !m_settingsDrawer->isHidden())
                            ? m_settingsDrawer->sizeHint().height() + 3
                            : 0;
    // The half-width toggle row is still one fixed 20 px row (folded into the
    // +62 baseline); the warning banner adds height only while it is shown.
    const int warnH = (m_daxWarning && !m_daxWarning->isHidden())
                          ? m_daxWarning->sizeHint().height() + 3
                          : 0;
    return {260, std::max(240, scopeH + drawerH + warnH + 62)};
}

QSize AetherClockApplet::minimumSizeHint() const
{
    const int scopeH = m_scope ? m_scope->minimumSizeHint().height() : 110;
    const int drawerH = (m_settingsDrawer && !m_settingsDrawer->isHidden())
                            ? m_settingsDrawer->minimumSizeHint().height() + 3
                            : 0;
    const int warnH = (m_daxWarning && !m_daxWarning->isHidden())
                          ? m_daxWarning->minimumSizeHint().height() + 3
                          : 0;
    return {220, std::max(180, scopeH + drawerH + warnH + 50)};
}

void AetherClockApplet::buildUi()
{
    theme::setContainer(this, QStringLiteral("applet/clock"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(3);

    // Alignment scope — the dominant differentiator, takes all stretch.
    m_scope = new ClockAlignmentWidget(this);
    layout->addWidget(m_scope, 1);

    // Status row: lock LED · station tag · decoded UTC · offset.
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(5);

        m_lockLed = new QLabel(this);
        m_lockLed->setFixedSize(10, 10);
        applyLockLed(m_lockLed, ClockLockState::NoSignal);
        row->addWidget(m_lockLed);

        m_stationTag = new QLabel(QStringLiteral("--"), this);
        ThemeManager::instance().applyStyleSheet(
            m_stationTag,
            "QLabel { color: {{color.text.secondary}}; font-size: 11px;"
            " font-weight: bold; }");
        row->addWidget(m_stationTag);

        // Trust line: decode quality + age since the last decoded frame, so a
        // ticking UTC readout can't be misread as a live-locked clock.
        m_trustLine = new QLabel(QStringLiteral("q--"), this);
        ThemeManager::instance().applyStyleSheet(
            m_trustLine,
            "QLabel { color: {{color.text.secondary}}; font-size: 10px; }");
        row->addWidget(m_trustLine);

        // Bound-slice indicator ("▸<letter>") — empty unless the engine is
        // running; secondary-text themed like the station tag.
        m_boundSliceTag = new QLabel(this);
        m_boundSliceTag->setToolTip(
            QStringLiteral("Slice the running decoder is bound to"));
        ThemeManager::instance().applyStyleSheet(
            m_boundSliceTag,
            "QLabel { color: {{color.text.secondary}}; font-size: 11px;"
            " font-weight: bold; }");
        row->addWidget(m_boundSliceTag);

        row->addStretch(1);

        m_utcValue = makeInsetReadout(this, 60);
        QFont mono = m_utcValue->font();
        mono.setStyleHint(QFont::Monospace);
        mono.setFamily(QStringLiteral("monospace"));
        m_utcValue->setFont(mono);
        row->addWidget(m_utcValue);

        m_offsetValue = makeInsetReadout(this, 52);
        row->addWidget(m_offsetValue);

        // DAX channel of the relevant slice (bound while running, else the
        // selected slice). Amber when a running bound slice has no channel.
        m_daxDisplay = makeInsetReadout(this, 40);
        m_daxDisplay->setToolTip(
            QStringLiteral("DAX channel — bound slice while running, "
                           "else the selected slice"));
        row->addWidget(m_daxDisplay);

        layout->addLayout(row);
    }

    // No-DAX warning directly under the status row: amber, word-wrapped, hidden
    // until a running bound slice is found to have no DAX channel (no audio).
    m_daxWarning = new QLabel(
        QStringLiteral("Bound slice has no DAX channel — no audio"), this);
    m_daxWarning->setObjectName(QStringLiteral("clockDaxWarning"));
    m_daxWarning->setWordWrap(true);
    m_daxWarning->setStyleSheet(
        QStringLiteral("QLabel { color: #ffb800; font-size: 10px; }"));
    m_daxWarning->setVisible(false);
    layout->addWidget(m_daxWarning);

    // Half-width button-grid row: the settings drawer toggle (collapsed by
    // default) beside the DAX chooser for the selected slice, each ~50%.
    {
        auto* controlRow = new QHBoxLayout;
        controlRow->setSpacing(3);

        m_drawerToggle = new QPushButton(this);
        m_drawerToggle->setStyleSheet(kButtonBase());
        m_drawerToggle->setFixedHeight(20);
        m_drawerToggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(m_drawerToggle, &QPushButton::clicked, this, [this]() {
            setSettingsExpanded(m_settingsDrawer && m_settingsDrawer->isHidden());
        });
        controlRow->addWidget(m_drawerToggle, 1);

        // DAX chooser acts on the strip's SELECTED slice — a client-scoped DAX
        // assignment. index == channel (0 = Off, 1-4). Disabled with no slice.
        m_daxCombo = new GuardedComboBox(this);
        m_daxCombo->setObjectName(QStringLiteral("clockDaxCombo"));
        m_daxCombo->setAccessibleName(QStringLiteral("AetherClock DAX channel"));
        applyComboStyle(m_daxCombo);
        m_daxCombo->setFixedHeight(20);
        m_daxCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_daxCombo->addItems({QStringLiteral("DAX Off"), QStringLiteral("DAX 1"),
                              QStringLiteral("DAX 2"), QStringLiteral("DAX 3"),
                              QStringLiteral("DAX 4")});
        connect(m_daxCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int idx) {
            // Skip the echo while we sync the combo from the model (VfoWidget
            // precedent); index == DAX channel.
            if (m_updatingDaxFromModel)
                return;
            if (!m_slice.isNull())
                m_slice->setDaxChannel(idx);
        });
        controlRow->addWidget(m_daxCombo, 1);

        layout->addLayout(controlRow);
    }

    buildSettingsDrawer();
    layout->addWidget(m_settingsDrawer);

    setSettingsExpanded(false);

    // 1 Hz display tick — extrapolates the UTC readout between decodes and ages
    // the trust line. Coarse timer: this is a human-glance readout, not timing.
    // Runs only while the engine is running AND a decode anchor exists
    // (updateTickTimer), so it's idle in the detached/stopped state.
    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(1000);
    m_tickTimer->setTimerType(Qt::CoarseTimer);
    connect(m_tickTimer, &QTimer::timeout, this,
            [this]() { refreshTrustAndTime(); });

    // Prime the status row + action enables to the detached/no-signal state.
    applyStationTag(m_stationTag, QStringLiteral("Unknown"));
    applyOffsetReadout(m_offsetValue, QDateTime(), 0.0);
    applyStaticTooltips();
    updateStartStopUi();
    refreshDaxUi();
    refreshTrustAndTime();  // seeds "--:--:--" / "q--" and the dynamic tooltips
}

void AetherClockApplet::buildSettingsDrawer()
{
    m_settingsDrawer = new QFrame(this);

    auto* drawer = new QVBoxLayout(m_settingsDrawer);
    drawer->setContentsMargins(5, 4, 5, 5);
    drawer->setSpacing(4);

    // Station preset combo.
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(5);
        row->addWidget(makeSettingLabel(QStringLiteral("Station:"), m_settingsDrawer));

        m_presetCombo = new GuardedComboBox(m_settingsDrawer);
        m_presetCombo->setObjectName(QStringLiteral("clockPresetCombo"));
        m_presetCombo->setAccessibleName(QStringLiteral("AetherClock station preset"));
        applyComboStyle(m_presetCombo);
        m_presetCombo->setFixedHeight(20);
        m_presetCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        row->addWidget(m_presetCombo, 1);

        drawer->addLayout(row);
    }

    // Tune slice → <preset>.
    m_tuneButton = new QPushButton(m_settingsDrawer);
    m_tuneButton->setStyleSheet(kButtonBase() + kDisabledBtn);
    m_tuneButton->setFixedHeight(20);
    m_tuneButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_tuneButton, &QPushButton::clicked, this,
            &AetherClockApplet::applyPresetSelection);
    drawer->addWidget(m_tuneButton);

    // Always-visible per-preset dial note (text set by refreshPresetUi).
    m_presetNote = new QLabel(m_settingsDrawer);
    m_presetNote->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(
        m_presetNote,
        "QLabel { color: {{color.text.secondary}}; font-size: 10px; }");
    drawer->addWidget(m_presetNote);

    // Start / Stop.
    m_startStopButton = new QPushButton(QStringLiteral("Start"), m_settingsDrawer);
    m_startStopButton->setCheckable(true);
    m_startStopButton->setStyleSheet(kButtonBase() + kStartActive + kDisabledBtn);
    m_startStopButton->setFixedHeight(22);
    m_startStopButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_startStopButton, &QPushButton::clicked, this, [this]() {
        // A checkable button toggles before this fires, so isChecked() is the
        // user's intended next state; updateStartStopUi reconciles it with the
        // engine's actual running state afterward.
        const bool wantRun = m_startStopButton && m_startStopButton->isChecked();
        if (!m_engine.isNull()) {
            if (wantRun) {
                if (!m_slice.isNull()) {
                    m_engine->start(m_slice.data(), currentChoice(m_presetCombo).station);
                    // The applet is the sole start caller, so binding here is
                    // authoritative for the running-slice display. Watch the
                    // bound slice's DAX channel too, so the no-audio warning
                    // reacts if the user (re)assigns a channel mid-run.
                    m_boundSlice = m_slice;
                    if (m_boundSliceDaxConn)
                        disconnect(m_boundSliceDaxConn);
                    m_boundSliceDaxConn =
                        connect(m_boundSlice.data(), &SliceModel::daxChannelChanged,
                                this, [this](int) { refreshDaxUi(); });
                }
            } else {
                m_engine->stop();
            }
        }
        updateStartStopUi();
        refreshDaxUi();
    });
    drawer->addWidget(m_startStopButton);

    // Populate carriers from the engine preset list, then restore the saved
    // selection under a signal block so restore never persists or applies.
    const QVector<double> wwvCarriers = AetherClockEngine::wwvCarrierFrequenciesMHz();
    {
        QSignalBlocker block(m_presetCombo);
        for (double mhz : wwvCarriers) {
            m_presetCombo->addItem(QStringLiteral("WWV %1 MHz").arg(QString::number(mhz)));
            const int idx = m_presetCombo->count() - 1;
            m_presetCombo->setItemData(idx, mhz, kCarrierRole);
            m_presetCombo->setItemData(idx, int(ClockStation::Wwv), kStationRole);
        }
        m_presetCombo->addItem(QStringLiteral("WWVB 60 kHz"));
        {
            const int idx = m_presetCombo->count() - 1;
            m_presetCombo->setItemData(idx, AetherClockEngine::wwvbCarrierFrequencyMHz(),
                                       kCarrierRole);
            m_presetCombo->setItemData(idx, int(ClockStation::Wwvb), kStationRole);
        }

        int sel = 0;
        if (AetherClockSettings::stationPreset() == QStringLiteral("WWVB")) {
            sel = m_presetCombo->count() - 1;
        } else {
            const double carrier = AetherClockSettings::wwvCarrierMHz();
            for (int i = 0; i < m_presetCombo->count(); ++i) {
                if (ClockStation(m_presetCombo->itemData(i, kStationRole).toInt())
                        == ClockStation::Wwv
                    && std::abs(m_presetCombo->itemData(i, kCarrierRole).toDouble()
                                - carrier)
                           < 1e-6) {
                    sel = i;
                    break;
                }
            }
        }
        m_presetCombo->setCurrentIndex(sel);
    }

    connect(m_presetCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) {
        if (!m_presetCombo)
            return;
        const PresetChoice c = currentChoice(m_presetCombo);
        // Persist the chosen preset only (radio-authoritative tuning is never
        // saved). WWVB keeps the last WWV carrier untouched.
        if (c.station == ClockStation::Wwvb) {
            AetherClockSettings::setStationPreset(QStringLiteral("WWVB"));
        } else {
            AetherClockSettings::setStationPreset(QStringLiteral("WWV"));
            AetherClockSettings::setWwvCarrierMHz(c.carrierMHz);
        }
        refreshPresetUi(m_presetCombo, m_tuneButton, m_presetNote);
    });

    refreshPresetUi(m_presetCombo, m_tuneButton, m_presetNote);
}

void AetherClockApplet::setSettingsExpanded(bool expanded)
{
    if (!m_settingsDrawer)
        return;

    m_settingsDrawer->setVisible(expanded);
    if (m_drawerToggle) {
        // U+25BE / U+25B8 triangles — text glyphs, not emoji icons.
        m_drawerToggle->setText(expanded ? QStringLiteral("▾ Settings")
                                         : QStringLiteral("▸ Settings"));
    }
    setMinimumHeight(minimumSizeHint().height());
    updateGeometry();
    adjustSize();
    if (auto* p = parentWidget())
        p->updateGeometry();
}

void AetherClockApplet::attach(AetherClockEngine* engine, AetherClockModel* model)
{
    if (!m_engine.isNull())
        disconnect(m_engine, nullptr, this, nullptr);
    if (!m_model.isNull())
        disconnect(m_model, nullptr, this, nullptr);

    m_engine = engine;
    m_model = model;

    // Event-driven fields (LED, station, offset) refreshed on any model change
    // — 1 Hz-class traffic, so a full re-read is trivially cheap. The UTC
    // readout + trust line are NOT set here: they tick from the decode anchor
    // (refreshTrustAndTime) so they stay live between the once-per-frame decodes.
    auto syncStatus = [this]() {
        AetherClockModel* m = m_model.data();
        applyLockLed(m_lockLed, m ? m->lockState() : ClockLockState::NoSignal);
        applyStationTag(m_stationTag, m ? m->stationName() : QStringLiteral("Unknown"));
        const QDateTime utc = m ? m->decodedUtc() : QDateTime();
        applyOffsetReadout(m_offsetValue, utc, m ? m->offsetMs() : 0.0);
        updateDynamicTooltips();
    };

    if (!m_model.isNull()) {
        connect(m_model, &AetherClockModel::stateChanged, this, syncStatus);
        connect(m_model, &AetherClockModel::stationChanged, this, syncStatus);
        connect(m_model, &AetherClockModel::offsetMsChanged, this, syncStatus);
        // A fresh decode re-anchors the ticking UTC readout + trust age.
        connect(m_model, &AetherClockModel::decodedUtcChanged, this,
                &AetherClockApplet::updateDecodeAnchor);
        // Quality feeds the trust line and the LED tooltip.
        connect(m_model, &AetherClockModel::lockQualityChanged, this,
                &AetherClockApplet::refreshTrustAndTime);
    }

    if (!m_engine.isNull()) {
        connect(m_engine, &AetherClockEngine::alignmentFrame, this,
                [this](const ClockAlignmentFrame& frame) {
            if (m_scope)
                m_scope->appendFrame(frame);
        });
        connect(m_engine, &AetherClockEngine::runningChanged, this,
                [this](bool running) {
            updateStartStopUi();
            if (!running) {
                // A real stop unbinds the slice; drop its DAX watch so a later
                // channel edit can't revive the running-state warning.
                if (m_boundSliceDaxConn)
                    disconnect(m_boundSliceDaxConn);
                m_boundSliceDaxConn = {};
                m_boundSlice = nullptr;
                // Drop the decode anchor so the UTC readout falls back to
                // "--:--:--" and the tick stops (updateTickTimer).
                m_anchorUtc = QDateTime();
                m_anchorHostMs = 0;
            }
            refreshDaxUi();
            updateTickTimer();
            refreshTrustAndTime();
            // History is useful across a NoSignal dropout; only a real stop
            // clears the scope.
            if (!running && m_scope)
                m_scope->clear();
        });
    }

    syncStatus();
    updateStartStopUi();
    refreshDaxUi();
    updateDecodeAnchor();  // seed the anchor if the model already has a decode
}

void AetherClockApplet::setSlice(SliceModel* slice)
{
    // Store only; never auto-start on bind, and never double-stop — if the
    // engine is running and the slice is lost, the engine stops itself and
    // its runningChanged(false) drives the UI. Here we just refresh enables.
    if (m_sliceDaxConn)
        disconnect(m_sliceDaxConn);
    m_sliceDaxConn = {};
    m_slice = slice;
    if (slice) {
        // The DAX chooser + display track the selected slice's channel.
        m_sliceDaxConn = connect(slice, &SliceModel::daxChannelChanged, this,
                                 [this](int) { refreshDaxUi(); });
    }
    updateStartStopUi();
    refreshDaxUi();
}

void AetherClockApplet::updateStartStopUi()
{
    const bool haveEngine = !m_engine.isNull();
    const bool haveSlice = !m_slice.isNull();
    const bool running = haveEngine && m_engine->isRunning();

    if (m_startStopButton) {
        QSignalBlocker block(m_startStopButton);
        m_startStopButton->setChecked(running);
        m_startStopButton->setText(running ? QStringLiteral("Stop")
                                           : QStringLiteral("Start"));
        // Always able to stop while running; only startable when attached to
        // both an engine and a slice.
        m_startStopButton->setEnabled(running || (haveEngine && haveSlice));
    }
    if (m_tuneButton)
        m_tuneButton->setEnabled(haveEngine && haveSlice);
}

void AetherClockApplet::applyPresetSelection()
{
    // Tune acts on the strip's SELECTED slice via the slice-scoped overload,
    // which works while the engine is stopped (it refuses locked slices
    // internally). Tune is enabled whenever an engine + slice are present — it
    // never requires the engine to be running or a slice to be bound.
    if (m_engine.isNull() || m_slice.isNull() || !m_presetCombo)
        return;
    const PresetChoice c = currentChoice(m_presetCombo);
    m_engine->applyStationPreset(m_slice.data(), c.station, c.carrierMHz);
}

void AetherClockApplet::refreshDaxUi()
{
    const bool running = !m_engine.isNull() && m_engine->isRunning();
    SliceModel* bound = m_boundSlice.data();
    SliceModel* sel = m_slice.data();

    // Bound-slice indicator: "▸<letter>" (U+25B8 text glyph), only while running.
    if (m_boundSliceTag) {
        m_boundSliceTag->setText((running && bound)
                                     ? QStringLiteral("▸") + bound->letter()
                                     : QString());
    }

    // DAX chooser tracks the SELECTED slice (the pick a user change acts on).
    // The QSignalBlocker stops the programmatic setCurrentIndex from echoing
    // back as a user edit; the guard flag is the VfoWidget belt-and-suspenders.
    if (m_daxCombo) {
        m_daxCombo->setEnabled(sel != nullptr);
        const int ch = std::clamp(sel ? sel->daxChannel() : 0, 0, 4);
        m_updatingDaxFromModel = true;
        {
            QSignalBlocker block(m_daxCombo);
            m_daxCombo->setCurrentIndex(ch);
        }
        m_updatingDaxFromModel = false;
    }

    // A running bound slice with no DAX channel is the no-audio condition — it
    // drives both the amber DAX inset and the warning banner.
    const bool noDaxWhileRunning = running && bound && bound->daxChannel() == 0;

    // DAX display inset: the RELEVANT slice — bound while running, else selected.
    if (m_daxDisplay) {
        SliceModel* shown = (running && bound) ? bound : sel;
        QString text;
        if (!shown) {
            text = QStringLiteral("--");
        } else if (shown->daxChannel() == 0) {
            // U+2013 en dash: slice present, no channel assigned.
            text = QStringLiteral("DAX –");
        } else {
            text = QStringLiteral("DAX %1").arg(shown->daxChannel());
        }
        m_daxDisplay->setText(text);
        m_daxDisplay->setStyleSheet(noDaxWhileRunning ? kInsetStyleWarn()
                                                      : kInsetStyle());
    }

    // Warning banner toggles height, so re-run the geometry path (mirroring
    // setSettingsExpanded) only when its visibility actually flips.
    if (m_daxWarning && m_daxWarning->isHidden() == noDaxWhileRunning) {
        m_daxWarning->setVisible(noDaxWhileRunning);
        setMinimumHeight(minimumSizeHint().height());
        updateGeometry();
        adjustSize();
        if (auto* p = parentWidget())
            p->updateGeometry();
    }
}

void AetherClockApplet::updateDecodeAnchor()
{
    // A decode gives the true broadcast second; pin it to the host clock now so
    // the readout can extrapolate forward between the once-per-frame decodes.
    AetherClockModel* m = m_model.data();
    const QDateTime utc = m ? m->decodedUtc() : QDateTime();
    if (utc.isValid()) {
        m_anchorUtc = utc;
        m_anchorHostMs = QDateTime::currentMSecsSinceEpoch();
    } else {
        m_anchorUtc = QDateTime();
        m_anchorHostMs = 0;
    }
    updateTickTimer();
    refreshTrustAndTime();
}

void AetherClockApplet::updateTickTimer()
{
    if (!m_tickTimer)
        return;
    // Tick only when there is something to extrapolate: engine running AND a
    // decode anchor to extrapolate from.
    const bool running = !m_engine.isNull() && m_engine->isRunning();
    const bool active = running && m_anchorUtc.isValid();
    if (active) {
        if (!m_tickTimer->isActive())
            m_tickTimer->start();
    } else if (m_tickTimer->isActive()) {
        m_tickTimer->stop();
    }
}

void AetherClockApplet::refreshTrustAndTime()
{
    // 1 Hz-cheap: only the time-dependent fields (UTC readout, trust line, and
    // their tooltips). The LED / station / offset / DAX surfaces are event-
    // driven elsewhere and deliberately untouched here.
    const bool running = !m_engine.isNull() && m_engine->isRunning();
    const bool haveDecode = m_anchorUtc.isValid();

    // Display-side extrapolation: anchorUtc + (now − anchorHostMs), floored to
    // the second. Monotonic, so the readout never ticks backward between
    // anchors. The engine/model are never touched.
    QDateTime shownUtc;
    qint64 ageMs = -1;
    if (running && haveDecode) {
        ageMs = QDateTime::currentMSecsSinceEpoch() - m_anchorHostMs;
        shownUtc = m_anchorUtc.addMSecs(ageMs);
    }
    if (m_utcValue) {
        m_utcValue->setText(
            shownUtc.isValid()
                ? shownUtc.toUTC().toString(QStringLiteral("HH:mm:ss"))
                : QStringLiteral("--:--:--"));
    }
    if (m_trustLine) {
        AetherClockModel* m = m_model.data();
        m_trustLine->setText(
            (running && haveDecode)
                ? QStringLiteral("q%1 · %2")
                      .arg(m ? m->lockQuality() : 0)
                      .arg(fmtDecodeAge(ageMs))
                : QStringLiteral("q--"));
    }
    updateDynamicTooltips();
}

void AetherClockApplet::updateDynamicTooltips()
{
    AetherClockModel* m = m_model.data();
    const bool running = !m_engine.isNull() && m_engine->isRunning();
    const bool haveDecode = m_anchorUtc.isValid();
    const int quality = m ? m->lockQuality() : 0;

    if (m_lockLed) {
        QString tip = QStringLiteral("Lock: %1 — quality %2/100")
                          .arg(m ? m->stateName() : QStringLiteral("NoSignal"))
                          .arg(quality);
        if (!haveDecode)
            tip += QStringLiteral(" · no decode yet");
        m_lockLed->setToolTip(tip);
    }
    if (m_utcValue) {
        QString tip;
        if (running && haveDecode) {
            const qint64 ageS =
                (QDateTime::currentMSecsSinceEpoch() - m_anchorHostMs) / 1000;
            tip = QStringLiteral(
                      "Decoded broadcast time (UTC). Ticks between decodes; "
                      "last decode %1, %2s ago")
                      .arg(m_anchorUtc.toUTC().toString(QStringLiteral("HH:mm:ss")))
                      .arg(ageS < 0 ? 0 : ageS);
        } else {
            tip = QStringLiteral(
                "Decoded broadcast time (UTC). Ticks between decodes; "
                "no decode yet");
        }
        m_utcValue->setToolTip(tip);
    }
}

void AetherClockApplet::applyStaticTooltips()
{
    if (m_stationTag)
        m_stationTag->setToolTip(QStringLiteral(
            "Detected station (WWV/WWVH auto-tagged by tick band; -- until "
            "confident)"));
    if (m_trustLine)
        m_trustLine->setToolTip(QStringLiteral(
            "Decode quality 0-100 and time since the last decoded frame"));
    if (m_offsetValue)
        m_offsetValue->setToolTip(QStringLiteral(
            "decoded − host at the second edge. Positive = this computer's "
            "clock is behind the broadcast"));
    if (m_scope)
        m_scope->setToolTip(QStringLiteral(
            "Received envelope vs expected symbol template; ticks mark detected "
            "second edges; bar lane = per-second confidence; glyph lane = "
            "decoded symbols (M = marker)"));
    if (m_presetCombo)
        m_presetCombo->setToolTip(QStringLiteral(
            "Station + carrier preset the Tune button applies to the selected "
            "slice"));
    if (m_tuneButton)
        m_tuneButton->setToolTip(QStringLiteral(
            "Tune the selected slice to the chosen preset — works while "
            "stopped"));
    if (m_startStopButton)
        m_startStopButton->setToolTip(QStringLiteral(
            "Start or stop decoding on the selected slice"));
    if (m_drawerToggle)
        m_drawerToggle->setToolTip(QStringLiteral(
            "Show or hide the station / tune / start settings"));
    if (m_daxCombo)
        m_daxCombo->setToolTip(QStringLiteral(
            "Assign the selected slice's DAX channel (0 = Off) — the audio "
            "path"));
}

} // namespace AetherSDR
