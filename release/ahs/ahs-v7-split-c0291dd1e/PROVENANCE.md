# AHS release v7-split-c0291dd1e

- 來源：與 `ahs-v7-c0291dd1e` 相同（`harryliao0615/AHS`，分支 `codex/item2-round45-20261013`，commit `c0291dd1e2051d7934563bbc31daa36bbc044fb4`），共用其 `upstream/`，不另外複製。
- 架構：`split`。AFC、陷波、guard 在 callback；AHS `HowlingDetector` 在 Project 2 analysis worker。佇列型別：`Hop`（64 點 AFC 後訊號）與 `Detection`（`ahs::HowlingCandidate` 陣列）。
- `split.cpp` 內容與 upstream 的對應：
  - `SplitEngine::process_hop()`、`apply()`：逐行對應 `src/ahs/engine.h` 的 `AhsEngine::processBlock()` 與 `runDetection()`；`runDetection()` 的偵測呼叫移到 worker。**省略頻移器**（含 `howling_time_s_`、shift 判斷）：交付組態 `enable_shift = false`，建構時檢查。
  - `Processor`：對應 `apps/live_pipeline.h` 的 `LivePipeline`（capture／committed／render／pump），只把 `AhsEngine` 換成 `SplitEngine`，並在每個 hop 推送 tap、套用已抵達的偵測。佇列與重取樣用 upstream 類別原樣。
  - `Handler`：用 upstream `HowlingDetector`，以 64 點 hop 組 256 點滑動窗，等同 `AhsEngine` 的 `frame_`。
  - 組態取自 upstream `LivePipeline::config()`。
- 驗證：偵測同步套用時與 AHS `LivePipeline` 逐位元組相同（3 種迴路增益 × 480,000 樣本）；真實 worker 下行為等於晚一個 callback 套用，不再與 AHS 相同。細節見 `../SPEC.md` §1.7。
