# Project 3：背景音訊處理與固定延遲播放

此元件完成薩克斯風按鍵噪音抑制所需的系統管線：麥克風輸入、Input SPSC、背景 DSP／
inference 介面、Output SPSC、播放期限判斷與時間對齊的原音備援。預設演算法是直通，
沒有實作 STFT、分類器、遮罩、AR 修復或 ISTFT；這些工作由 `AudioProcessor` 接入。

```text
麥克風 → input callback 階段 → input SPSC → ProcessingWorker → AudioProcessor
                    │                                            │
                    │                                        output SPSC
                    ↓                                            ↓
             固定延遲的原音 ──────────→ output callback 階段 → 喇叭
                                       選擇同一時間位置的完整 block
```

裝置 adapter 使用一個全雙工 PortAudio callback，先 capture，再 render。輸入與輸出階段
共用同一個 frame 時鐘與 audio thread，避免原音 delay ring 的跨執行緒競爭。
不支援把同一個 engine 的 capture／render 分散到兩條未同步的裝置執行緒。

## 和 Project 2 的差異

| | Project 2 | Project 3 |
|---|---|---|
| 背景 worker 回傳 | 完整濾波參數 | 處理後音訊＋原始 sample index |
| 演算法執行位置 | 即時 callback 執行 IIR；背景分析 | 背景 worker 執行音訊處理 |
| Worker 未完成 | 保留目前濾波設定 | 播放同一位置的原音 |
| 延遲 | 直接音訊處理路徑 | 固定 software delay，含 lookahead／等待預算 |

## 建置

可單獨複製此 `components/project3` 目錄使用；核心不依賴 Project 2、外部 quant checkout、
PortAudio 或 Python。SPSC 副本來自目前 Project 2 的 `detail/spsc_ring.hpp`，包含既有
`atomic_ref` 與原地讀寫 API，原始來源追溯至 quant-architect；註記在 header 內。
此處只驗證同一 process 中的執行緒，不宣稱跨 process SHM 可攜性。

在此目錄內（C++20、GCC／Clang、CMake 3.20+）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

接入其他 CMake 專案：

```cmake
add_subdirectory(path/to/project3 project3-build)
target_link_libraries(my_app PRIVATE rt_audio::project3)
```

設定 `RT_AUDIO_PROJECT3_WITH_PORTAUDIO=ON` 可增加 `rt_audio::project3_portaudio` target。
它需要 `pkg-config` 能找到 PortAudio，但公開 device header 不引入 `portaudio.h`。

從本 repository 根目錄：

```bash
make test                              # 兩個專案的測試與模擬器
./build/project3_sim                   # 合成音訊＋中途停止 worker，驗證備援
bash scripts/bootstrap_portaudio.sh    # 若尚未準備 PortAudio
make live                             # 建立兩個專案的裝置程式
./build/project3_live --list-devices
./build/project3_live --input 0 --output 1 --channels 1 \
  --sample-rate 48000 --frames 128 --block-frames 128 \
  --lookahead-frames 256 --delay-frames 512 --seconds 10
```

裝置編號需依列表修改，省略則使用系統預設。直通播放請使用耳機，避免聲學回授。
程式在指定時間或 Ctrl-C 時先停止裝置，再停止／join worker。

## 演算法 I/O ports

完整入口為 `#include <rt_audio/project3.hpp>`，namespace 是 `rt_audio::project3`。

| 介面 | 執行緒 | 資料與契約 |
|---|---|---|
| `Engine::process_callback(input, output, frames, timing)` | 唯一 audio callback | interleaved float32；輸入先複製，再填滿輸出 |
| `AudioProcessor::prepare(config)` | control，啟動前 | 初始化 STFT／模型／window 狀態 |
| `AudioProcessor::on_audio(block, sink)` | 背景 worker | 取得固定 hop 的音訊；可累積上下文，發布零個、一個或多個較早的處理區塊 |
| `OutputSink::try_submit_output(block)` | 唯一 worker producer | 非阻塞發布完整處理區塊；失敗時由 worker 決定重試／放棄 |
| `OutputSink::playback_deadline()` | worker 可讀 | 尚未決定播放方式的第一個 source sample，僅供參考，讀完可能立即改變 |
| `Engine::try_read_input(block)` | 唯一 worker consumer | 給自訂 worker loop；不可和 `ProcessingWorker` 同時讀取 |
| `Engine::statistics()` | 監控／control | 獨立 atomic counters；非同一瞬間的整組快照 |

`AudioBlock` 的有效資料是前 `frames * channels` 個 float，並帶有 `first_sample`、
`input_time_seconds`、`status_flags`、`input_missing`。Queue 持有樣本副本，不保留裝置指標。
`on_audio` 的 block storage 會重複使用；rolling window 必須複製所需資料，不能保留 reference。

輸出必須遵守：

- `first_sample` 指向原始輸入的 sample 位置，**不能加上 lookahead 或 delay**。
- `frames == config.block_frames`、`channels == config.channels`，且起點為 hop 的整數倍。
- 依 sample index 遞增、不重複、不重疊；允許跳過沒有結果的區塊。
- 不能發布尚未收錄的區間，不能把較新的音訊拿來補較舊的播放位置。
- Queue 滿而提交失敗時，不會消耗該起點，可稍後重試；已過期限就應放棄。
- 提交成功只代表進入 queue；若與播放期限競爭，結果仍可能被 consumer 當作遲到資料丟棄。
- 插件自行處理輸入缺口、裝置 discontinuity、輸出數值範圍與演算法狀態。

若演算法使用較大 STFT window 或不同內部 hop，請在插件中重組，最後仍發布固定輸出 hop。
例如處理 source `[0,128)` 需要參考後面 256 frames，就保留 window，等上下文到齊後
發布 `first_sample=0, frames=128`；引擎會在固定播放時間選取它。

最小接線：

```cpp
#include <rt_audio/project3.hpp>
#include <rt_audio/project3/portaudio_device.hpp>
#include <memory>

namespace p3 = rt_audio::project3;
p3::StreamConfig config;
auto engine = std::make_unique<p3::Engine>(config);
p3::PassthroughProcessor processor; // 替換成自己的 AudioProcessor
p3::ProcessingWorker worker(*engine, processor);
p3::PortAudioDevice device(*engine); // 宣告在 worker 後，例外時先關閉裝置
worker.start();
device.start();
// control thread 管理執行期間；勿在此解構物件
device.stop();
worker.stop();
worker.rethrow_if_failed();
```

插件範例（僅示範 I/O，並非抑制演算法）：

```cpp
class MyProcessor final : public p3::AudioProcessor {
public:
    void prepare(const p3::StreamConfig&) override {
        // 配置自己的模型、STFT window、overlap-add 狀態。
    }
    void on_audio(const p3::AudioBlock& input, p3::OutputSink& output) override {
        // 複製到自己的 rolling window，保留 source sample index。
        // 推論／修復／重建後，將完整 hop 寫入 result.samples。
        p3::AudioBlock result = input; // 此範例保留原音
        if (!output.try_submit_output(result)) {
            // 滿載可保留 pending；遲到結果放棄，不在 callback 等待。
        }
    }
};
```

## 時間與播放政策

預設值為 48 kHz、1 聲道、device callback 128 frames、worker hop 128 frames、
保留 lookahead 256 frames、固定 software delay 512 frames（約 10.67 ms）。

`lookahead_frames` 是插件宣告的前視需求，不會自動產生 STFT window 或強制直通插件等待。
設定必須滿足 `delay_frames >= block_frames + lookahead_frames`，且 delay ≤ 16384 frames。
多出的時間可供排程與處理使用；仍須以實際演算法與裝置量測。較大的 delay 不代表
queue 自動擴容；兩條 queue 都只有 32 blocks，過大的積壓會觸發丟棄與備援。

以輸出 frame 時鐘 `t` 表示：

```text
t < delay_frames       → 啟動靜音
t >= delay_frames      → 播放 source sample = t - delay_frames
```

在每個 source block 的第一個 sample 要播放時，只做一次選擇：處理結果已就緒就播放它；
否則整個 block 使用同時間位置的原音。此決定跨 callback 保留，遲到結果不能在 block
中途替換內容。尚未輪到的結果暫存，過期結果丟棄，絕不補播或改變固定延遲。

實際 device callback 可以是 1–2048 frames，未必等於 worker hop。引擎會累積輸入、
分段播放，也支援 delay 不是 hop 整數倍。裝置停止時未填滿的最後一個 input hop 不提交，
不自動播放尾端 delay buffer；新串流需建立新 engine／worker。

`first_sample` 計算已交付 frames，不是硬體 sample clock。Input queue 丟失會形成 index 缺口；
裝置 overflow／underflow 也需檢查 flags 與 ADC timestamp。

## 即時與過載契約

- Callback 不執行使用者 DSP、不配置／釋放、不加 mutex、不等 worker、不做 I/O。
- 每次 callback 最多 2048 frames，最多嘗試讀取 32 筆 output；即使 worker 持續發布也有界。
- Dry ring 在 engine 建構時配置，容量涵蓋最大 delay 加上整個 callback，支援 in-place I/O。
  Buffer 必須分離或完全相同，不支援部分重疊。
- Null input 視為零；null output、無效大小等回傳 false，不碰 buffer。裝置 adapter 清零並中止。
- Input queue 滿只丟新的 worker 輸入；原音仍寫入 dry ring，播放繼續。
- Output queue 滿回傳 false；缺少處理結果時自動使用對齊原音。
- Worker 拋例外會停止 worker、設定 `failed()`；callback 繼續播放已就緒結果或原音。
- 生命週期操作限 control thread；processor 必須返回，`stop()` 才能 join。
- 原音與處理音訊在 block 邊界切換可能有音色或波形不連續；此版不加入 crossfade 演算法。
- 參數與 UI 控制可由自己的 worker 管理；不能增加同一 queue 的 producer／consumer。

## 目錄與驗證

| 檔案 | 責任 |
|---|---|
| `include/rt_audio/project3/types.hpp` | 格式、時間、容量與統計 |
| `processor.hpp` | 背景演算法與音訊發布介面 |
| `engine.hpp`、`src/engine.cpp` | 音訊 callback、兩條 queue、delay ring 與播放選擇 |
| `worker.hpp`、`src/worker.cpp` | 背景執行緒與錯誤傳遞 |
| `portaudio_device.hpp`、`backends/portaudio/` | 可選的麥克風／喇叭 adapter |
| `tests/project3_tests.cpp` | 時間對齊、過載、邊界與執行緒驗證 |

測試涵蓋立體聲／可變 callback／原地 I/O、dry ring 回繞、音訊重新分塊、延遲結果、
未來結果、queue 滿載與重試、含 lookahead 的插件接線、worker 失敗與並行資料完整性。
`project3_sim` 中途停止 worker，仍逐 sample 比對固定延遲輸出。

功能測試不代表達成講義的端到端 ≤ 30 ms 目標。Live 程式顯示 software delay 以及裝置
回報延遲的估計總和；仍需要真實音訊 loopback 測量，尚未實測麥克風／喇叭或抑制效果。

本次在 macOS arm64／Apple Clang 17 驗證：Project 3 的 12 組測試、Project 2 的既有
10 組測試與兩個模擬器均通過；CMake Release 整合 CTest 4/4、獨立元件 CTest 1/1 通過。
PortAudio adapter 完成編譯連結，`--help` 與無效參數拒絕通過，未啟動真實裝置。
