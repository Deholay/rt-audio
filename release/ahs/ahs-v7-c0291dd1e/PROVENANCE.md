# AHS release v7-c0291dd1e

- 來源：`harryliao0615/AHS`，分支 `codex/item2-round45-20261013`，commit `c0291dd1e2051d7934563bbc31daa36bbc044fb4`（2026-11-02）
- 組態：AHS `apps/live_pipeline.h` 的 `LivePipeline::config()`：AFC + 陷波 + guard、16 kHz、block 64、頻移關閉、growth PAPR 15 dB；Speex Q4 48↔16 kHz
- `upstream/` 內檔案與來源 commit 逐位元組相同，未修改。唯一的轉接層是 `processor.cpp`：用巨集把 `ahs`／`ahs_live` 命名空間改名、編譯時加 Speex 符號前綴，讓日後多個 release 可以連進同一個執行檔。
- 授權：`src/ahs/`、`apps/` 為 AHS 自有程式；`third_party/speexdsp/` 為 BSD-3-Clause（見 `COPYING`、`provenance.json`）。

## upstream 檔案 SHA-256

```
8b62e29f9c973d86a340f3a2b9155fd38163cc8973d6f91fa45c8ceac383d150  apps/fixed_resampler.h
fa8f14c6630b89558d93502501ea6fdc4113bb3afbfc84c0cc98b25b907d0373  apps/live_pipeline.h
a5557b890faa10c194debf30ae032db47f03fba78c52b0d645d35b19ebbafac7  apps/render_reference.h
b91c7b3107a4358046b548364ebe353d48cf6e7359ed00be59d26d78a4127c58  src/ahs/afc.h
8f0d2845cb5ef68d257333afb293546cfa479267c13448f1f33ff3d15511d84c  src/ahs/afc_gate.h
879fb306e45c09cfc5646fa4d7316b2b4c3db69580c7f550253bc57da8314e81  src/ahs/biquad.h
f71998f50a7d8d4b2ef6d091611f41e235da4b101bc98a7cf673677204e4e9ed  src/ahs/detector.h
47f6e4e87198281058f8871c5c0d85dd0822ca44af12b4c9db68ce6651582c7c  src/ahs/engine.h
ad5c1ac1bf778060095fd2bc2b6343b031a6ec5135658ca7d0ffc40c3a5ce73e  src/ahs/fft.h
76ffc4a2371c71deddfa2ca4dd95c1091f0fbfb222946f438fa8ca5e4af0551b  src/ahs/freq_shift.h
a7a0af495239caaea073a53a0316211b6ba5d14e6800cd197c7ed09039c2b2ea  src/ahs/notch_bank.h
bb154b905da6447043ce0de76116fa5fd54a622e6c8cd1c3ed4c930071e3ce63  src/ahs/pbfd_kalman.h
2654a4264b2bfe298dedc508748d140111840c315cc8eb646a3a68c13fa75b01  third_party/speexdsp/COPYING
102f6a14a95f8ae0bcfc69270f3a9e3fbba08b63bba688ca524f71e3faa48c23  third_party/speexdsp/arch.h
d6c14298b723eb50fba388511971726dadf7e8faf685c152c8c01c3f42c25107  third_party/speexdsp/provenance.json
c4ec3bc71fc4414d7ebd62fc745290c7b5b06b149a05dd70cf72553bc57c8c76  third_party/speexdsp/resample.c
7e439ec0dd30c32216b3ced17135f8992e5aaf53389d3f5996a7d900c453e65f  third_party/speexdsp/speex_resampler.h
```
