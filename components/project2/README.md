# Project 2 元件

此目錄可獨立複製、建置，或用 CMake `add_subdirectory()` 加入其他應用程式。
核心提供同步音訊 callback、分析 SPSC、參數 SPSC，以及可選的背景分析 worker。
預設 processor 只做音訊直通；IIR、smoother 與偵測演算法透過介面接入。

## 建置與接入

只需要 C++20、GCC／Clang、CMake 3.20+ 和系統執行緒函式庫。
本地 SPSC 使用 GCC／Clang atomic builtins，要求原生 lock-free 64-bit atomics。
核心不需要 PortAudio、pkg-config、Python 或 quant-architect checkout。

從本目錄執行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

在其他 CMake 專案使用（呼叫端已宣告 `project(... LANGUAGES CXX)`）：

```cmake
add_subdirectory(path/to/project2 project2-build)
add_executable(my_audio_app main.cpp)
target_link_libraries(my_audio_app PRIVATE rt_audio::project2)
```

應用程式可繼續使用 `#include <rt_audio/project2.hpp>`，或依元件職責引入個別 header。
核心 CMake target 會傳遞 include 目錄、C++20 和 Threads 相依性。
獨立建置預設包含測試；作為子目錄時遵循呼叫專案的 `BUILD_TESTING`。

## 介面與依賴方向

| Header（位於 `include/rt_audio/project2/`） | 責任 |
|---|---|
| `types.hpp` | StreamConfig、音訊區塊、參數快照、時間及統計 |
| `processor.hpp` | RealtimeProcessor 與 PassthroughProcessor |
| `analysis.hpp` | AnalysisHandler、NoopAnalysisHandler、ParameterSink |
| `engine.hpp` | Project2 callback 與本地 SPSC 管線 |
| `worker.hpp` | AnalysisWorker 啟停及例外傳遞 |
| `portaudio_device.hpp` | 可選的 PortAudioDevice；公開 header 不需要 PortAudio header |
| `detail/spsc_ring.hpp` | 複製進來的固定容量 SPSC，屬於元件內部實作 |

`types.hpp` 是共用格式；processor 與 analysis 介面依賴這些格式；engine 組合 DSP 和 queue；
worker 驅動分析介面；裝置 adapter 呼叫 engine。核心沒有對裝置 backend 的依賴。

```text
mic -> PortAudioDevice -> Project2::process_callback -> RealtimeProcessor -> speaker
                            |                              ^
                            v                              |
                       analysis SPSC                  parameter SPSC
                            |                              ^
                            +---- AnalysisWorker ----------+
                                  AnalysisHandler
```

所有物件在開始串流前建構。即時端只能由一個 audio callback 呼叫；分析 queue 只有
一個 consumer，參數 queue 只有一個 producer。音訊 callback 不等待分析執行緒。

## 可選裝置元件

`RT_AUDIO_PROJECT2_WITH_PORTAUDIO=ON` 會增加 `rt_audio::project2_portaudio` target，
需要 pkg-config 能找到 PortAudio。它會自動連結核心 target；純核心預設不啟用此選項。

```cmake
set(RT_AUDIO_PROJECT2_WITH_PORTAUDIO ON)
add_subdirectory(path/to/project2 project2-build)
target_link_libraries(my_audio_app PRIVATE rt_audio::project2_portaudio)
```

`PortAudioDevice` 的 constructor 開啟串流但不開始錄放；`start()`／`stop()` 控制串流，
`list_devices()` 回傳擁有自身資料的裝置列表，`info()` 提供格式與延遲資訊。
實作透過 PImpl 隱藏 PortAudio API。裝置必須在 engine 之後建構、在 engine 之前解構。
先停止裝置 callback，再停止 worker；所有裝置生命週期操作由 control thread 執行。

## 本地 SPSC 來源

`detail/spsc_ring.hpp` 複製自 `quant-architect/include/quant/ipc/spsc_ring.hpp`，
來源 commit 為 `cedae9ed27577a96c225eacc49f2e7f103cbecb2`。僅修改 namespace 與平台說明，
保留固定容量、單 producer／consumer、acquire／release、滿／空立即回傳的既有邏輯。
這是原始碼副本，不是 symlink、submodule 或外部 include path。

## 行為契約

- 音訊格式為 interleaved float32，1–2 聲道，每次 1–2048 frames。
- `process_callback` 直接產生音訊；分析 queue 滿只丟棄分析副本。
- DSP 的 `prepare()` 在啟動前配置狀態；`process()` 與 `apply_parameters()` 必須即時安全。
- 每個 block 最多讀取 4 個完整參數快照，套用其中最新的一個；queue 空則保留既有 DSP 狀態。
- `ParameterSnapshot` 暴露頻率、頻寬、抑制深度、bypass 與 transition time；具體效果由 DSP 實作。
- 使用 `AnalysisWorker` 時不要另外讀取 analysis queue，也不要從 UI 並行寫入 parameter queue。
- Frame／buffer／參數的精確範圍及引用生命週期記載於各公開 header。

裝置程式與模擬器保留在 repository 根目錄的 `apps/`；完整操作說明見根目錄 README。
