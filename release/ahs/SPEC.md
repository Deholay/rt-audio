# AHS release I/O 介面規格

適用：`release/ahs/`，rt-audio Project 2。對應實作：`include/rt_audio/ahs.hpp`、`registry.cpp`、`ahs-v7-c0291dd1e/processor.cpp`、`ahs-v7-split-c0291dd1e/split.cpp`；核心泛型引擎 `components/project2/include/rt_audio/project2/engine.hpp`。

本規格分兩部分：§1 給**使用者**（呼叫端怎麼用、保證什麼）；§2 給**發布者**（新增一個 AHS 版本要交什麼）。日後 Project 3 的 release 另放 `release/<名稱>/`，沿用 §2 的目錄慣例，不共用本檔的 API。

---

## 1. 使用者介面

### 1.1 公開 API

```cpp
#include <rt_audio/ahs.hpp>   // CMake target：rt_audio::ahs

namespace rt_audio::ahs {
struct Release {
    std::string_view name;          // 版本名稱，例如 "v7-c0291dd1e"
    std::string_view source_commit; // AHS 完整 commit（40 字元）
    std::string_view source_branch; // AHS 來源分支
    std::string_view configuration; // 此版本執行的 AHS 組態摘要（給人讀）
    std::string_view architecture;  // "callback" 或 "split"，見 §1.8
};
std::span<const Release> releases() noexcept;

class Session {                     // 一條串流：engine + 該版本需要的 analysis worker
public:
    virtual AudioCallback& callback() noexcept = 0;  // 交給 PortAudioDevice 或模擬器
    virtual void start() = 0;                        // 先 start()，再啟動裝置
    virtual void stop() noexcept = 0;                // 先停裝置，再 stop()
    virtual void rethrow_if_failed() const = 0;      // stop() 後呼叫
};
std::unique_ptr<Session> make_session(
    std::string_view name, const StreamConfig& config, float output_gain = 0.1F);
std::unique_ptr<RealtimeProcessor> make_processor(   // 只限 "callback" 版本
    std::string_view name = "latest", float output_gain = 0.1F);
}
```

| 函式 | 行為 |
|---|---|
| `releases()` | 回傳所有已登記版本，**舊到新**；最後一筆是最新版。字串指向靜態資料，程式結束前都有效 |
| `make_session(name, config, gain)` | **所有版本都可用**。`name` 是 `"latest"`（最新版）或 `releases()` 中的某個 `name`。建立時即 `prepare()`，回傳可直接接裝置的 `Session` |
| `make_processor(name, gain)` | 只限 `"callback"` 版本，給你嵌進自己的 `Project2`。這裡的 `"latest"` 是**最新的 callback 版本**。回傳尚未 `prepare()` 的 `RealtimeProcessor` |

錯誤（都在控制執行緒、建立時拋出）：

| 情況 | 例外 |
|---|---|
| `name` 不在清單內 | `std::invalid_argument("Unknown AHS release: <name>")` |
| `output_gain` 非有限值或不在 [0, 1] | `std::invalid_argument` |
| 對 `"split"` 版本呼叫 `make_processor` | `std::invalid_argument("... is a split release; use make_session()")` |
| `make_session` 的串流格式不符 §1.3 | `std::invalid_argument` |

### 1.2 接線方式

建議用 `Session`，兩種架構接法相同：

```cpp
auto session = rt_audio::ahs::make_session("latest", config, 0.1F);
rt_audio::PortAudioDevice device(session->callback());
session->start(); device.start();
// ...
device.stop(); session->stop(); session->rethrow_if_failed();
```

- `device` 宣告在 `session` 之後，例外展開時會先停裝置。

`"callback"` 版本也可以只拿 processor，自己接 Project 2：

```cpp
auto processor = rt_audio::ahs::make_processor("latest", 0.1F);
auto engine = std::make_unique<rt_audio::Project2>(config, *processor); // 這裡呼叫 prepare()
rt_audio::NoopAnalysisHandler handler;                                  // AHS 不需要 analysis
rt_audio::AnalysisWorker worker(*engine, handler);
```

- `processor` 的生命週期必須長於 `engine`（先宣告 processor，再宣告 engine）。
- 一個 processor 只給一個 engine 使用。

### 1.3 串流需求

由 `prepare(const StreamConfig&)` 檢查，不符合即拋例外，engine 建立失敗。

| 欄位 | 要求 | 不符合時 |
|---|---|---|
| `sample_rate` | **必須 48000.0** | `std::invalid_argument("AHS release <name> requires 48000 Hz")` |
| `channels` | 1 或 2（Project 2 本身已限制） | — |
| `frames_per_buffer` | 1..`max_frames`（2048）；實際每次 callback 的 frames 可以變動 | — |

### 1.4 訊號語意

| 項目 | 規格 |
|---|---|
| 輸入 | 交錯 float32。立體聲先取 (L+R)/2 降為單聲道；非有限值當 0 |
| 輸入缺失 | Project 2 以靜音緩衝代入，等同 AHS 的 SILENT 輸入 |
| 輸出 | 同一個單聲道訊號寫到每個聲道；乘 `output_gain` 後限制在 [−1, 1]；**每個樣本都會寫** |
| 喇叭參考訊號 | processor 自己上一刻的最終輸出（已乘 gain、已限幅），在內部降取樣後交給 AFC。**呼叫端不用、也不能另外提供參考訊號** |
| 原地處理 | `input == output` 可以 |
| `apply_parameters()` | 忽略。AHS 自己偵測、自己下陷波；`ParameterSnapshot` 不影響輸出 |
| 啟動 | `prepare()` 先放一個 `frames_per_buffer` 長的靜音參考。開頭幾個 callback 輸出靜音，直到 48→16 kHz 重取樣與 64 點區塊填滿；這與 AHS `ahs_live_win.cpp` 相同 |
| 延遲 | 重取樣每個方向約 2 ms（AHS 離線量測，Speex Q4 R2），再加 64 點（16 kHz）區塊累積；**在 rt-audio 內未實測**，裝置延遲另計 |

### 1.5 即時性保證

符合 `RealtimeProcessor` 的契約：

- `prepare()`：控制執行緒呼叫，在這裡配置所有記憶體（佇列容量 `max_frames × 8`）。可重複呼叫，每次重建全部狀態。
- `process()`：音訊執行緒呼叫，`noexcept`，不配置記憶體、不加鎖、不做 I/O。
- 內部時間線若損壞（佇列溢位、重取樣錯誤），AHS 原本會重開 epoch；callback 裡不能配置記憶體，所以這裡改成**之後一律輸出靜音**，直到下一次 `prepare()`。輸入和輸出等長的 duplex callback 不會觸發這種情況。

### 1.6 版本相同性保證

每個 release 在 rt-audio 裡的輸出，必須和該 AHS commit 自己的 `LivePipeline` 用同樣呼叫順序跑出來的輸出**逐位元組相同**（同一台機器、同一個編譯器）。跨平台或跨編譯器不保證逐位元組相同。

目前紀錄：`v7-c0291dd1e` 在 Apple M5／Apple clang 上，用 6 種閉迴路增益各 480,000 個樣本比對，0 個樣本不同（AHS 端 C++17 `-O3`，rt-audio 端 C++20 `-O2`）。

### 1.7 架構說明（`architecture`）

| | `"callback"` | `"split"` |
|---|---|---|
| callback 內 | 整個 AHS：AFC → 偵測 → 陷波 → guard | AFC → 陷波 → guard |
| analysis worker | Noop | AHS `HowlingDetector`（256 點 FFT，每 64 點一次） |
| analysis 佇列內容 | 不使用 | `Hop`：序號 + 64 點 AFC 後訊號（16 kHz） |
| 回傳佇列內容 | 不使用 | `Detection`：hop 序號 + 最多 8 個 `ahs::HowlingCandidate` |
| 偵測結果套用 | 同一個 64 點邊界 | 抵達後在下一個 64 點邊界**依序全部套用**，每筆推進 AHS 計時一個 hop |
| 與 AHS 相同性 | 逐位元組相同（§1.6） | **不相同**：偵測晚生效；結果取決於 worker 執行時機，不同次執行可能不同 |

split 版的佇列型別是 AHS 自己的型別，經由 `Engine<Protocol>` 走 Project 2 的 SPSC 與 worker（lock-free 行為不變）。`statistics()` 的 `analysis_dropped` 是遺失的 hop，`commands_dropped` 是 worker 重試 5 ms 仍送不進的偵測結果，`commands_applied` 是已套用的偵測筆數。

split 版目前紀錄（Apple M5／Apple clang，合成輸入，scratchpad 對照程式）：

- 偵測改成同步套用時，與 AHS `LivePipeline` 逐位元組相同：證明 callback 端的重組忠實。
- 真實 worker 執行緒（callback 間隔 2.67 ms）：輸出和「固定晚一個 callback 套用」完全相同；0 遺失 hop、0 丟棄偵測。和 AHS 相比約 98% 樣本不同，相對誤差 −14 dB（無回授）到 −20 dB（嘯叫）。
- 嘯叫抑制：和 callback 版大致相當，有時較好、有時較差（單一共振迴路模型，不能下結論；要比較須跑 MSG 矩陣）。

### 1.7.1 已發布版本

| 版本 | 架構 | 與 AHS | 程式碼音訊延遲（脈衝、無回授） |
|---|---|---|---|
| `v7-c0291dd1e` | callback | 逐位元組相同 | 6.667 ms（frames 32：7.333 ms） |
| `v7-split-c0291dd1e` | split | 不同（偵測晚一個 callback） | 同 v7 管線 |
| `v7-lowlat-c0291dd1e` | callback | 不同：pump 不等滿 64 點；無回授時等於 v7 平移 2.67 ms | 4.000 ms（所有 frames） |
| `v7-lowlat-fir-c0291dd1e` | callback | 不同：lowlat ＋ 重取樣器換成線性相位 FIR（算法變更，通過 R2 準則） | **2.146 ms**（所有 frames） |

`"latest"` 現在是 `v7-lowlat-fir-c0291dd1e`（`make_session` 與 `make_processor` 皆是）。

**算法變更只在 rt-audio 時**：upstream 仍須原樣；變更放在 release 自己的轉接檔，並附設計／驗證腳本與結果（例：`ahs-v7-lowlat-fir-c0291dd1e/design_fir.py`），PROVENANCE 寫明改了哪一塊、對照哪一份 AHS 規格、哪些驗證未做。要與 AHS 逐位元組相同，指定 `v7-c0291dd1e`。

### 1.8 不在範圍內

- 即時狀態輸出（是否偵測到嘯叫、陷波數量與頻率、guard 降幅）：目前沒有，排在之後。
- AHS 自己的 underrun 計數：沒有接到 `Statistics`。
- 48 kHz 以外的取樣率。

---

## 2. 發布者規格：新增一個 AHS 版本

`"callback"` 版本照 §2.1–2.5。`"split"` 版本另見 §2.6。

### 2.1 命名

| 項目 | 規則 | 範例 |
|---|---|---|
| 版本名稱 `name` | `<組態版本>-<commit 前 9 碼>` | `v7-c0291dd1e` |
| 資料夾 | `release/ahs/ahs-<name>/` | `release/ahs/ahs-v7-c0291dd1e/` |
| C++ 識別字 `<sym>` | `name` 的 `-` 換成 `_` | `v7_c0291dd1e` |

已發布的 `name` 不得改名或刪除；呼叫端可能寫死了版本名稱。

### 2.2 資料夾內容

```
release/ahs/ahs-<name>/
  PROVENANCE.md      來源 repo、分支、完整 commit、日期、組態、授權、upstream 每個檔案的 SHA-256
  processor.cpp      唯一的轉接層
  upstream/          從 AHS commit 原樣複製，路徑保持 AHS 的相對路徑
    src/ahs/*.h      engine.h 以及它 include 到的所有標頭
    apps/            live_pipeline.h、render_reference.h、fixed_resampler.h
    third_party/speexdsp/  resample.c、speex_resampler.h、arch.h、COPYING、provenance.json
```

- `upstream/` 的檔案一律**逐位元組不改**；複製後以 SHA-256 對照來源 commit，結果寫入 `PROVENANCE.md`。
- 只複製實際被 include 到的檔案。不得複製無授權或 GPL／AGPL 的程式碼。

### 2.3 `processor.cpp` 必須做到

1. 先 include `<rt_audio/ahs.hpp>` 等 rt-audio 標頭，再用巨集替 upstream 命名空間改名，include 完立刻 `#undef`：
   ```cpp
   #define ahs rt_audio_ahs_<sym>
   #define ahs_live rt_audio_ahs_live_<sym>
   #include "live_pipeline.h"
   #undef ahs_live
   #undef ahs
   ```
   這樣多個版本可以連進同一個執行檔而不違反 ODR。
2. 在 `namespace rt_audio::ahs::<sym>` 提供工廠函式：
   ```cpp
   std::unique_ptr<RealtimeProcessor> make(float output_gain);
   ```
   `output_gain` 已由 registry 驗證過範圍。
3. 實作 §1.3–1.5 的行為，呼叫順序照該版本 AHS 的部署程式（v7 是 `ahs_live_win.cpp` 的 `transfer()`：先放靜音參考，之後每個 callback 依序 `capture` → `render` → `committed`）。若新版本 AHS 改了部署順序或取樣率，以該版本的程式碼為準，並在 `PROVENANCE.md` 寫明差異。
4. 不得修改 upstream 行為；需要不同行為時，應該在 AHS 端發新 commit，再發新 release。

### 2.4 登記

**`release/ahs/registry.cpp`**：兩張表都**加在最後面**，順序一致：

```cpp
namespace <sym> { std::unique_ptr<RealtimeProcessor> make(float output_gain); }
// table：
{"<name>", "<完整 40 碼 commit>", "<分支>", "<組態摘要>"},
// factories：
&<sym>::make,
```

**`release/ahs/CMakeLists.txt`**：仿照 v7 的區塊，為新版本建一個 OBJECT library：
- upstream 的三個路徑用 `SYSTEM PRIVATE` include（upstream 的警告不會讓 `-Werror` 失敗）。
- 定義 `OUTSIDE_SPEEX`、`RANDOM_PREFIX=rt_audio_ahs_<sym>_speex`、`FLOATING_POINT`（Speex C 符號依版本加前綴）。
- 只對 C++ 檔加 `-Wall -Wextra -Wpedantic -Werror`。
- 用 `target_sources(rt_audio_ahs PRIVATE $<TARGET_OBJECTS:...>)` 併入 `rt_audio_ahs`。

**`Makefile`**：目前的 `AHS_RELEASE`、`AHS_UPSTREAM`、`AHS_FLAGS` 只描述一個版本。新增第二個版本時，要為它另設一組 upstream 路徑與 `RANDOM_PREFIX`，並把它的 `processor.o`、`resample.o` 加進 `AHS_OBJECTS`。

### 2.5 發布前檢查

| 檢查 | 通過條件 |
|---|---|
| upstream 原樣 | 每個檔案的 SHA-256 等於來源 commit 的 `git show <commit>:<path>` |
| 建置 | `make` 與 CMake 都無警告、無錯誤 |
| 選擇 | `project2_live --help` 列出新版本；`--processor <name>` 與 `--processor latest` 都能啟動 |
| 相同性 | 依 §1.6，與該 AHS commit 的 `LivePipeline` 在同一輸入下逐位元組相同；至少涵蓋無回授、迴路增益 < 1、嘯叫三種情況 |
| 舊版本 | 已發布版本的輸出不因這次變更而改變 |

### 2.6 `"split"` 版本

- 資料夾 `release/ahs/ahs-<名稱>-split-<commit>/` 或 `ahs-<組態>-split-<commit>/`，例如 `ahs-v7-split-c0291dd1e/`。
- 和同一 commit 的 callback 版本**共用** `upstream/`，不重複複製；使用相同的命名空間改名與 Speex 前綴，連到同一個 Speex 物件。
- 轉接檔（例如 `split.cpp`）定義一個 Protocol（欄位見 `engine.hpp` 註解）：`Tap`、`Command` 用 AHS 型別組成並且 trivially copyable；`sequence()` 非零遞增；`deliver()` 呼叫 handler；`callback()` 呼叫 processor。
- engine 迴圈若需要重寫（因為 `AhsEngine` 的偵測是 private），必須逐行對應 upstream `engine.h`，並在檔頭註明省略了哪些部分以及理由。
- 提供 `make_session(const StreamConfig&, float)`，在 `registry.cpp` 以 `{nullptr, &<sym>::make_session}` 登記，`architecture` 填 `"split"`。
- 發布前另外檢查：偵測**同步套用**時輸出與 AHS 逐位元組相同；真實 worker 下回報與 AHS 的差異、遺失 hop 與丟棄偵測數。
