# AHS release v7-lowlat-fir-c0291dd1e

- 來源：與 `ahs-v7-c0291dd1e` 相同（`harryliao0615/AHS`，分支 `codex/item2-round45-20261013`，commit `c0291dd1e2051d7934563bbc31daa36bbc044fb4`），共用其 `upstream/`，upstream 檔案未修改。
- 架構：`callback`。**算法變更（只在 rt-audio）**：重取樣器。
  - 以 `v7-lowlat-c0291dd1e` 為基礎（pump 不等滿 64 點）。
  - Speex Q4（`apps/fixed_resampler.h`，每方向 2.000 ms）換成線性相位等漣波 FIR：降取樣 111 tap（55 樣本 = 1.146 ms）、升取樣 97 tap（48 樣本 = 1.000 ms）。麥克風與參考訊號用同一種降取樣器。
  - AFC、偵測器、陷波、guard 與組態（`LivePipeline::config()`）不變；mono 混音、非有限值處理、靜音參考 priming、呼叫順序沿用 upstream。
- 檔案：`design_fir.py`（設計＋R2 驗證，產生 `fir_coeffs.hpp`）、`fir_coeffs.hpp`（float 係數）、`fir.cpp`（polyphase 實作與 pipeline）。
- R2 準則（AHS `docs/windows_a_resampler_revision2_for_review.md` @ c0291dd1e）驗證，以 float 係數計算，FFT 網格 2^19：

| 項目 | 本版 | 門檻 |
|---|---|---|
| 通帶 0–6500 Hz 偏差（降／升） | 0.067／0.071 dB | ≤0.10 dB |
| 降取樣阻帶 8000–24000 Hz | 77.6 dB | ≥70 dB |
| 升取樣阻帶 8200–24000 Hz | 77.1 dB | ≥70 dB |
| 升取樣 8000／8100 Hz（過渡區，只報告） | 42.3／53.8 dB | — |
| 串接 48→16→48 鏡像 8000–24000 Hz（1 Hz 網格） | 77.3 dB | ≥70 dB |
| DC 偏差（降／升） | 1.6e-8／1.7e-8 | ≤1e-4 |
| 每方向延遲 | 1.146／1.000 ms | ≤2.1 ms |

- 實作驗證（Apple M5／Apple clang，合成輸入）：
  - 無回授、無偵測時引擎為恆等，整條鏈脈衝響應 = numpy 串接設計係數，最大誤差 3.5e-8；鏈通帶 20–6500 Hz −0.066～+0.072 dB。
  - 程式碼音訊延遲（脈衝）：frames 32／64／128／256 皆 **2.146 ms**（v7-lowlat 4.000、v7 6.667）。
  - 閉迴路增益掃描（notebook 合成路徑，−20 dB → g @4 s）。**嚴格標準**：步階後任一 250 ms ≤ −12 dBFS 才算穩定：passthrough +2、v7 +4、v7-lowlat +6、本版 +5 dB。先前用「末 4 s 平均 ≤ −10 dBFS」得到 +9 dB，該標準漏掉中途爆發，已撤回。
  - **6 條隨機路徑（seed 1–6）嚴格掃描**：v7 平均 +3.3、lowlat +3.8、本版 +3.7 dB；本版對 lowlat 逐路徑 −3～+4 dB（較好 2／相同 1／較差 3）。**無系統性差異**；單一路徑的 1～4 dB 差距屬路徑相依。
  - 失守點：+6～+8 dB 嘯叫在約 123 Hz（+6 dB 持續約 1.5 s 後被壓下）。假說（未驗證）：延遲變短改變迴路相位，相位對齊的頻率落到低頻；AHS 偵測器低頻弱（`min_freq_hz` 100、256 點 FFT 每格 62.5 Hz；AHS 報告也記錄真實房間 137 Hz 未觸發）。lowlat 在 +9 dB 也卡在 113 Hz。
  - callback CPU 平均約 15.5 µs（Speex 版約 18.2 µs），非隔離量測。
- 未做：R2 的逐頻串接 cos/sin 穩態投影交叉驗證、引擎新增 7800–8000 Hz 成分的影像分析（R2 §引擎新增成分）、MSG／540 矩陣、實機。與 AHS 不逐位元組相同。
