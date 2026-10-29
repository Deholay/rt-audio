# Project 3 快速導覽

此元件的 worker 回傳處理後音訊；播放 callback 依 sample index 選擇結果或延遲原音。

閱讀順序：

```text
types.hpp → processor.hpp → engine.hpp / engine.cpp → worker.hpp / worker.cpp
                                                     ↓
                            portaudio_device.hpp → apps/project3_live.cpp
```

- `Engine`：音訊 callback 內先收音、重組固定 hop，再輸出固定延遲的樣本。
- `AudioProcessor`：在背景取得輸入，累積自己的 STFT／模型視窗，透過 `OutputSink` 發布音訊。
- `ProcessingWorker`：驅動處理器；例外不傳進音訊 callback。
- `PortAudioDevice`：處理裝置生命週期，不負責 DSP。
- `PassthroughProcessor`：接線用的直通實作，沒有按鍵噪音抑制演算法。

核心規則：兩條 queue 各一個 producer／consumer；輸出保留原始 sample index；遲到不補播；
callback 不等待 worker。先啟動 worker，再啟動 device；停止時先停 device，再 join worker。
詳細介面、延遲公式與過載策略見 [README](README.md)。
