# IC-705 preamp proof for #6497

Hardware receive frequency: **446.15 MHz**.

- `before.png`: baseline control settled back at P.AMP1 after the native click requested stage 2, which the radio rejected.
- `after.png`: fixed native control shows OFF after the click requested stage 0 and the radio acknowledged and reported stage 0.
- `preamp-proof.json`: only the preamp command, acknowledgement/rejection, and matching readback sequences. A further native click restored stage 1, confirmed by radio readback.

These are offscreen control captures, not GPU-renderer proof. The operating frequency, FM/FIL1 and attenuator OFF were preserved. This is receive-only evidence; no transmit testing. Other bands and the exact 74.800 MHz boundary were validated offline, with the boundary based on the issue reporter's measurements.
