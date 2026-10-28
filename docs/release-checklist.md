# Before publishing

- [ ] Publish from a fresh repository made from a clean tree, not this
      history: early commits contain simulator screenshots with album art
      and a Bluetooth trace's device addresses.
- [ ] No pairing keys, logs or recordings: `lib/lbt/test/traces/*.pklg`
      stays out (gitignored); `bt-run*` logs live outside this repository.
- [ ] Licences: LICENSE (Apache-2.0), NOTICE, `lib/lbt/sbc/LICENSE`,
      `assets/fonts/OFL-*.txt`. Check the Noto subset against the OFL's
      rules for modified fonts (it keeps no reserved name).
- [ ] The Bluetooth firmware patch is not included (README says where it
      comes from).
- [ ] README: hardware list, install and the way back are current.
- [ ] `docs/h2-hardware.md` sources are cited for every fact.
- [ ] Simulator and all tests build and pass on a clean checkout.
- [ ] Our own USB vendor and product IDs (pid.codes gives them to open
      source hardware and firmware) in `platform/h2/h2_usb.c`, in place of
      pid.codes' test ID 1209:0001, which is for testing only.
