# Evidence for AetherSDR #6510

Source fix: `495b29c77289d4c5baf91a591ea702beb944ae9d`, based on upstream `b5b540920985cc214bac259104be2f8045030578`.

- `layout-before.png` / `layout-after.png`: actual Qt label captures from the isolated layout probe, using a 12px system font and a 450px test window. Before: the 113x30 label needs 45px of wrapped height and clips. After: 203x30, requiring 30px, fits.
- `layout-matrix.json`: 5 font sizes (12, 14, 16, 20, 24px) x 3 window widths (300, 450, 900px); original clips 15/15, fixed fits 15/15.
- `layout_probe.cpp`: the exact empty-state widget factory from the baseline alongside the one-line fix, in a QScrollArea/QVBoxLayout matching the production nesting. The icon renderer is replaced by a transparent pixmap of the same 56x56 size. This isolates text geometry; it is not an application screenshot. Source follows AetherSDR's GPLv3 license.
- `waveforms-after-100.png` / `waveforms-after-150.png`: actual full-application agent automation bridge captures on macOS/Qt 6.12, at the 900x660 minimum window size and 100%/150% scale. 980x720 was also checked. Fresh settings profile, autoconnect disabled, authenticated `whoami` verified the child PID and `txAllowed:false`, and `get radio` verified disconnected. Each child was stopped afterward.

The default Inter font in the macOS full app did not clip before the fix. The isolated font matrix reproduces the clipping mechanism. No claim of native Windows reproduction or reporter confirmation is made.

Build the probe with CMake and Qt6 Widgets, then run with `QT_QPA_PLATFORM=offscreen`. Screenshots are written under `/tmp/fb-6510` and measurements to stdout. The application proof build disabled optional ASR, RADE, D-STAR helper, and DFNR engines. The proof process used `DYLD_FRAMEWORK_PATH` to select the pinned Qt 6.12 frameworks consistently with the locally installed QtKeychain dependency.
