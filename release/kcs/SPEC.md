# KCS release 規格（Project 3）

技術驗證，非合約交付。適用：`release/kcs/`，rt-audio Project 3。對應實作：`include/rt_audio/kcs.hpp`、`registry.cpp`、`kcs-<version>-<commit>/`。發布者慣例沿用 [AHS SPEC §2](../ahs/SPEC.md)（版本資料夾、`upstream/` 原樣複製、`PROVENANCE.md` SHA-256），但 API 不共用。

## 0. 硬體與串流規格（先於演算法）

來源分三類標示：**合約**＝KCS `item3/test_plan_v3_2026-10.md` §4 與 `item3/definition_table.md` 第 4 列（R2 批註確認，正式書面缺）；**KCS 設計**＝KCS repo 內的設計／實作；**本 release**＝rt-audio 這裡的選擇。

| 項目 | 規格 | 來源 | 程式強制 |
|---|---|---|---|
| 裝置取樣率 | 48000 Hz | 合約（16 kHz 修訂已於 2026-10-13 撤回） | `prepare()` 拒絕其他值 |
| 裝置 block | ≤128 frames（`frames_per_buffer`） | 合約 | `prepare()` 拒絕 >128 |
| Worker hop | ≤128 frames（`block_frames`） | 本 release（與裝置 block 同上限） | `prepare()` 拒絕 >128 |
| 聲道 | mono | KCS 設計（processor_io_spec：E_MONO，不自動混聲道） | `prepare()` 拒絕 channels≠1 |
| 資料格式 | float32 | KCS 設計（Bridge48k）；Project 3 本身即 float32 | — |
| 端到端延遲 | 20 個 click 取最差 ≤30 ms，實體迴路量測，bypass 與處理器分開報 | 合約 | **不能由程式保證**，須實測 |
| 量測鏈 | 有線音訊介面、實體 line out → line in loopback；裝置回報延遲不算量測 | 合約／AGENTS.md | — |
| 每 128 點運算預算 | 128/48000 s = 2.667 ms | KCS `bridge48k_timing_prediction_v1.md` | 須單獨執行量測 |

延遲預算（本 release，48 kHz frames）：

| 段 | frames | ms | 性質 |
|---|---:|---:|---|
| 下採 FIR 群延遲 | 30 | 0.625 | 演算法，固定 |
| v23 核心 `latency_samples` 144 @16 kHz | 432 | 9.000 | 演算法，固定 |
| 上採 FIR 群延遲 | 30 | 0.625 | 演算法，固定 |
| **演算法延遲 `lookahead_frames`** | **492** | **10.250** | 宣告給 Project 3 |
| hop 對齊（source block 需輸入到 s+619，落在 s+640 結束的 block） | 128 | 2.667 | 架構 |
| Worker 預算（一個 callback 週期） | 20 | 0.417 | 架構：640−620 |
| **建議 `delay_frames`** | **640** | **13.333** | software delay（固定） |
| 裝置輸入＋輸出緩衝、ADC／DAC | ? | ? | **未量測**，留給 ≤30 ms 中剩下的 16.67 ms |

`delay_frames=640` 時，source block `[s, s+128)` 所需的最後一個輸入在 callback m+4 擷取（s=128m），播放判斷在 callback m+5，worker 有一個 callback 週期（2.667 ms）完成。來不及時 engine 自動播放對齊原音（Project 3 契約）。

### 0.1 本機硬體現況（2026-11-05 查核，`system_profiler SPAudioDataType`）

MacBook Air（Apple M5）：內建麥克風／喇叭 48 kHz、AirPods Pro（Bluetooth，輸入 24 kHz）、車用藍牙麥克風、Teams 虛擬裝置。**沒有有線音訊介面**。

- 內建喇叭＋內建麥克風是聲學路徑，不是 line loopback，不符合量測鏈。
- 藍牙輸入 24 kHz 不符合 48 kHz；藍牙編碼延遲也不屬於合約量測鏈。
- 結論：本機只能做功能與等價驗證；**端到端 ≤30 ms 無法在本機驗證**。KCS `test_plan_v3` 的「環境與硬體」表（介面型號、驅動、緩衝、接線）仍全部空白，需先選定介面再填。

## 1. 使用者介面

```cpp
#include <rt_audio/kcs.hpp>   // CMake target：rt_audio::kcs

namespace p3 = rt_audio::project3;
auto config = rt_audio::kcs::recommended_config();          // 48k, mono, 128, lookahead 492, delay 640
auto processor = rt_audio::kcs::make_processor("latest");   // p3::AudioProcessor
auto engine = std::make_unique<p3::Engine>(config);
p3::ProcessingWorker worker(*engine, *processor);
// worker.start() → device.start() … device.stop() → worker.stop() → worker.rethrow_if_failed()
```

| 函式 | 行為 |
|---|---|
| `releases()` | 已登記版本，舊到新 |
| `make_processor(name)` | `"latest"` 或 `releases()` 的 `name`；未知名稱拋 `std::invalid_argument` |
| `recommended_config(name)` | 符合 §0 的 `StreamConfig` |

`prepare()` 檢查 §0 的「程式強制」欄，另要求 `lookahead_frames ≥` 該版本演算法延遲（492），不符合拋 `std::invalid_argument`。

### 1.1 訊號語意

- 每個輸入 hop 依絕對樣本軸送進 48 kHz 鏈（下採 FIR → 每 3 點取 1 → v23 核心 → 同相位插零 → 上採 FIR），輸出 sample n 對應 source n−492；processor 依此重組為 hop 對齊的輸出 block，`first_sample` 保持 source index。
- 輸出頻寬受 7 kHz 重採樣 FIR 限制（KCS 設計），與 fallback 原音（全頻）在 block 邊界切換時音色不同。
- **輸入不連續**（`first_sample` 跳號或 `InputOverflow`）：整條鏈重建（等同 KCS「一個 session 一個 fresh instance」），跨缺口的輸出 block 不發布，engine 播原音。
- **非有限輸入**：換成 0 並計數。KCS `Bridge48k` 在此進入終止 FAILED 狀態；即時串流不能停，這是本 release 與 KCS 的差異（有限輸入時不影響等價）。
- **不做** KCS 的 11 秒輸入配額與 `finish()` 排空：那是離線評分守門與 EOS 行為，即時串流沒有 EOS。
- Output queue 滿：保留一個待送 block，下次再試；再失敗即丟棄（允許跳過，engine 播原音）。

## 2. 發布者規格

| 項目 | 規則 |
|---|---|
| 資料夾 | `release/kcs/kcs-<KCS 版本>-<commit 前 9 碼>/` |
| `upstream/` | KCS 參考實作的 Python 原檔，逐位元組不改（SHA-256 寫入 PROVENANCE）；只作參考，不編譯、不在 runtime 執行 |
| 移植 | C++ 在 `pipeline.hpp`／`pipeline.cpp`；processor 轉接在 `processor.cpp` |
| 等價 | `verify_equivalence.py` 用同一輸入跑 KCS `Bridge48k` 與 C++ 鏈，報最大誤差、位置、事件清單是否一致；容許值寫在 PROVENANCE |
| 只在 rt-audio 的變更 | 寫在本檔 §1.1 與 PROVENANCE，註明分歧自哪個 KCS 版本 |
| 登記 | `registry.cpp` 表格加在最後；已發布名稱不得改名或刪除 |
