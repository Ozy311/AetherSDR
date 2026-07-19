#pragma once

// AetherClock persistence. Per Constitution Principle V the configuration
// lives as ONE nested JSON blob under the single AppSettings key
// "AetherClock" (the AprsSettings / AutomationBridgeSettings pattern) —
// never flat keys. Radio-authoritative state (slice frequency, mode, AGC)
// is NEVER persisted.

#include <QJsonObject>
#include <QString>

namespace AetherSDR {

class AetherClockSettings {
public:
    // Chosen station preset: "WWV" (default) or "WWVB".
    static QString stationPreset();
    static void setStationPreset(const QString& preset);

    // Chosen WWV carrier (MHz), one of AetherClockEngine's preset list.
    static double wwvCarrierMHz(); // default 10.0
    static void setWwvCarrierMHz(double mhz);

private:
    static QJsonObject readObj();
    static void write(const QJsonObject& o);
};

} // namespace AetherSDR
