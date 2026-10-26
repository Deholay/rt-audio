# Project 2 快速導覽

核心位於 `components/project2/`，可獨立建置。音訊直接在 callback 處理；背景分析透過 SPSC 回傳參數，callback 不等待分析完成。

```text
音訊：麥克風 → PortAudioDevice → Project2 → RealtimeProcessor → 裝置輸出 → 喇叭
分析：Project2 → 分析 SPSC → AnalysisWorker → AnalysisHandler
回傳：AnalysisHandler → 參數 SPSC → Project2 → processor.apply_parameters()
```

- `PortAudioDevice`：連接麥克風／喇叭，由音訊系統觸發 callback。
- `Project2`：持有兩條 queue，複製分析音訊、接收參數並呼叫 DSP。
- `RealtimeProcessor`：音訊處理介面，供 IIR 與 smoother 接入。
- `AnalysisWorker`：背景執行緒，取出音訊並呼叫分析器。
- `AnalysisHandler`：分析介面，供嘯叫偵測接入並發布參數。
- `types.hpp`：定義音訊區塊、參數快照與共用格式。

`apps/project2_live.cpp` 負責接線：把 processor 交給 engine，再讓 device 和 worker 使用同一個 engine。先啟動 worker，再啟動 device；停止時順序相反。

## 接入演算法

- DSP：實作 `RealtimeProcessor` 的 `prepare()`、`apply_parameters()`、`process()`。
- 偵測：實作 `AnalysisHandler::on_audio()`，透過 `ParameterSink::try_submit_parameters()` 發布完整參數快照。
- 目前預設為 `PassthroughProcessor` 原音直通與 `NoopAnalysisHandler` 空分析器，尚未實作抑制演算法。

## 執行規則

- Callback 不配置記憶體、不加鎖、不等待，也不做檔案或終端 I/O。
- 每條 SPSC 嚴格限一個 producer 與一個 consumer；UI 不可直接成為第二個參數 producer。
- 分析 queue 滿只丟棄分析副本，播放繼續；參數 queue 空則保留目前設定。
- 每個音訊區塊最多讀取 4 筆參數，套用其中最新一筆；回傳 queue 裝的是參數，不是音訊。

## 濃縮規則

```C++
using namespace rt_audio;

StreamConfig config;

// ① 準備音訊處理器
PassthroughProcessor processor;

// ② 引擎知道要呼叫哪個處理器
auto engine = std::make_unique<Project2>(config, processor);

// ③ 背景 worker 知道去哪拿音訊、交給誰分析
NoopAnalysisHandler handler;
AnalysisWorker worker(*engine, handler);

// ④ 音訊裝置知道收到聲音後，要呼叫哪個引擎
PortAudioDevice device(*engine);

// ⑤ 啟動
worker.start();
device.start();
```

## 順序
```
types.hpp                         先認識資料格式
    ↓
processor.hpp → processor.cpp     音訊處理介面與直通實作
    ↓
analysis.hpp                      分析與參數發布介面
    ↓
engine.hpp → engine.cpp           callback 如何串接 DSP 和兩條 queue
    ↓
worker.hpp → worker.cpp           背景執行緒如何執行分析
    ↓
project2_live.cpp                 所有元件如何建立、連接與啟停
```

* `.hpp` 宣告介面，不實作
* `.cpp` 實作相對應的 `.hpp`

> 其他程式則以 `.hpp` 為依賴對象