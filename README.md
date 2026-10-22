# Fixbug: GHE port editing (#6519)

The default port is present in the model but invisible in the original GHE applet: the styled QSpinBox using NoButtons gives its internal QLineEdit a width of 1 pixel. Keeping its normal button mode and setting both stepper widths to zero through QSS restores the editor without adding visible arrows. The production port range, field width, theme tokens and connection gate stay unchanged.

## Evidence boundaries

- Baseline full app: signed commit 495b29c77289d4c5baf91a591ea702beb944ae9d from the preceding Waveforms fix. Its GHE source is byte-identical to upstream b5b540920985cc214bac259104be2f8045030578, the base of this fix.
- Fixed source: signed commit `16228c633ada0e4eda211e72f42737c0b6119174`. The app was built from the same source tree immediately before committing.
- Full app proof: native macOS arm64, Qt 6.12, offscreen rendering, isolated settings, no radio or Everyware server connected, TX disabled. The agent automation bridge checks five values (1, 9999, 10000, 12345, 65535), editor geometry and screenshots. Before: 1 pixel for each; after: 52 pixels for each.
- Keyboard probe: scratch Qt Widgets/Test executable containing the same port-field setup and stylesheet, with theme colors resolved to fixed opaque colors. QTest mouse click, Select All, keyClicks and Tab exercise editing. Both Fusion and Windows widget styles run on macOS, not native Windows. Ten before and ten after cases: every typed value accepted, but zero before and all ten after editors have enough width to show the text. The baseline variant reintroduces NoButtons and removes the arrow-width stylesheet rule.
- The agent automation bridge exposes setValue and editor geometry, but no general text-key injection for this spin box. The focused Qt keyboard probe complements the full-app bridge evidence; it does not claim physical-keyboard input through the complete app.
- No native Windows/Linux build or live Everyware connection proof is claimed.

## Reproduce the focused probe

Use Qt 6.12 with Widgets and Test components:

```sh
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/path/to/Qt/6.12/platform
cmake --build build
mkdir -p /tmp/fb-6519
QT_QPA_PLATFORM=offscreen ./build/spin_probe
```

The executable writes `/tmp/fb-6519/keyboard-matrix.json` and exits nonzero if a typed value differs or the before/after text-fit expectation changes. Source is provided under AetherSDR's GPL-3.0 license.
