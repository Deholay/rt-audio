# AHS release v7-lowlat-c0291dd1e

- 來源：與 `ahs-v7-c0291dd1e` 相同（`harryliao0615/AHS`，分支 `codex/item2-round45-20261013`，commit `c0291dd1e2051d7934563bbc31daa36bbc044fb4`），共用其 `upstream/`，不另外複製。
- 架構：`callback`。整個 AHS 在 `RealtimeProcessor` 內，和 `v7-c0291dd1e` 相同。
- 唯一差異：`lowlat.cpp` 重組 upstream `apps/live_pipeline.h` 的 `LivePipeline`，只改 `pump()`。
  - upstream：麥克風與參考佇列**都滿 64 點**才呼叫 `AhsEngine::processBlock(…, 64)`。
  - 本版：兩個佇列**都有的樣本**立即處理，每次最多 64 點，處理到取完為止。
  - `AhsEngine` 以內部樣本計數決定每 64 點偵測一次（upstream `engine.h` 註明可接受部分或跨 hop 的呼叫），所以偵測節奏、引擎程式碼、重取樣器都不變。`capture()`、`committed()`、`render()` 與呼叫順序沿用 upstream。
- 驗證（Apple M5／Apple clang，合成輸入）：
  - 程式碼音訊延遲（脈衝、無回授）：frames 32／64／128／256 皆 **4.000 ms**；`v7-c0291dd1e` 為 6.667 ms（32 點時 7.333 ms）。剩下的 4.000 ms 即兩次 Speex Q4。
  - 無回授：輸出等於 `v7-c0291dd1e` 平移 128 樣本（2.67 ms），479,065／479,872 樣本逐位元組相同。
  - 有回授：與 v7 不同（迴路延遲變短，閉迴路行為改變）。共振迴路 A/B 中嘯叫壓得更快、較少再冒（單一模型，非 MSG 矩陣）。
  - callback CPU：平均與 v7 相同（約 18.5 µs），p99 約 38 → 32 µs（非隔離量測，僅供參考）。
- **不與 AHS 逐位元組相同**；閉迴路效果須以 MSG／540 矩陣重驗後才能作為交付依據。此改動屬部署管線（`live_pipeline.h`），建議回饋 AHS repo，供其 Windows 程式（`ahs_live_win.cpp`）一併採用。
