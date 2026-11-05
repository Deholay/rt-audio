# kcs-v23-ce6237c12

技術驗證，非合約交付。

| 項目 | 值 |
|---|---|
| 來源 repo | https://github.com/harryliao0615/KCS（本機 `../KCS`） |
| 分支 | `main` |
| commit | `ce6237c124a62c79b28667aced2ee2ff6df6b1b0`（2026-11-02 20:28 +0800） |
| KCS 版本 | v23 基準（`TransientAR32`，ORDER 32，`latency_samples`=144 @16 kHz）＋ `Bridge48k`（batch19） |
| 移植日期 | 2026-11-05 |
| 授權 | KCS README：程式碼授權**尚未指定**，視為保留所有權利；本 release 僅供內部技術驗證，散布前須取得授權 |

## upstream/（逐位元組，參考用，不編譯）

`git -C ../KCS show ce6237c1…:scripts/<file>` 取得，SHA-256 與 KCS 工作樹檔案相同；`verify_equivalence.py` 每次執行時重新比對 commit。

| 檔案 | SHA-256 | 為什麼需要 |
|---|---|---|
| `scripts/item3_transient_v23.py` | `670fd51360a4e315f5664fa35025c759fff1cdc3f977e90fd7e038f966998c31` | 移植對象：16 kHz 核心 |
| `scripts/item3_transient.py` | `2fdc5079e1d977030be775876feaab17d98ac3a4c6cfa3c81a3cbb431e722e1c` | v23 匯入的常數 SHORT/LONG/T_DB/D/N/C/REFRACTORY |
| `scripts/item3_bridge48k.py` | `126b3625a98fe29a748acbe4399e490f0c218ca86d7bb8cd54535b4f98a26f8b` | 移植對象：48 kHz 傳輸、全域相位 |
| `scripts/item3_rt_host.py` | `d09576a89bfa73618047b85accf1c3fbc67bb52913584343f0b9041061af2c2a` | 移植對象：`CausalFIR`、`rate_taps()` |
| `scripts/item3_transient_v17.py` | `9711a0366d2e2a23ae11694c38cfc19dfd1bb54d8fdff5cce24a3f164d9c0ead` | 只為讓 bridge／host 可 import |
| `scripts/item3_rt.py` | `65395d90ce63eb9a26a6919983a6f65f762b76c3c85d0e89210459f2f7f2faf7` | 只為讓 host 可 import |
| `scripts/item3_hpss.py` | `4fe2753bfc007ee3b02d47fe9d16f60f234f17f7b256f2825d84174b53b3a301` | 只為讓 host 可 import |

KCS 凍結檔（`item3_transient_v15`…`v24.py` 等）在 KCS 端零改動；這裡只有複本。

## 移植了什麼

| C++ | 對應 Python | 備註 |
|---|---|---|
| `Core16k`（`pipeline.cpp`） | `TransientAR32._live_tick/_start_repair/_fit/_emit` | float64；固定大小 ring buffer，建構後不配置記憶體 |
| `solve()` | `np.linalg.solve`（LAPACK gesv） | LU、部分樞軸 |
| `hessenberg_eigenvalues()` | `np.roots` → `eigvals`（LAPACK geev） | 同一伴隨矩陣；balancing＋Francis 雙位移 QR（Numerical Recipes 3rd ed.）；不收斂 → `solve_failure`（同 NumPy LinAlgError 路徑） |
| poly 重建 | `np.poly(roots)[1:].real` | 依序複數卷積 |
| `Pipeline48k` | `Bridge48k.process_block`（`_through`/`_expand`）＋ `CausalFIR` ＋ `rate_taps()` | 全域相位 n%3==0；上採係數 3×taps；輸出轉 float32 |
| `processor.cpp` | 無（rt-audio 轉接） | Project 3 `AudioProcessor`，見 SPEC §1.1 |

**未移植**（刻意）：`flush()`／`finish()` 排空與 EOS、11 秒輸入配額、`events`／`timing` 完整紀錄（只保留最後一筆事件供等價比對）、Python 例外型別。

## 只在 rt-audio 的行為（分歧自 KCS v23 ce6237c1）

1. 非有限輸入換成 0（KCS `Bridge48k`：進入終止 FAILED、之後輸出零）。
2. 輸入缺口或 `InputOverflow` → 整條鏈重建（KCS：一個 session 一個 fresh instance；無缺口概念）。
3. 無 11 秒配額、無 `finish()`（即時串流無 EOS）。

有限、連續輸入時 1–3 都不改變輸出。

## 等價驗證

`python -I release/kcs/kcs-v23-ce6237c12/verify_equivalence.py`（rt-audio 根目錄，先 `make all`）；結果 `equivalence_results.json`。

環境：Local Mac，macOS 26.6 arm64，Apple M5，Apple clang 17.0.0（`-O2`，C++20），Python 3.14.3，NumPy 2.5.4（`build/notebook-venv`）。輸入：**合成訊號**（諧波音＋衰減雜訊 click、強 click、靜音＋click、白雜訊、削波），各 6 s @48 kHz，128 點 block；不是 KCS 素材，不代表品質。

| 訊號 | 最大 \|誤差\|（float32 輸出） | 位置（48k sample） | 不同樣本數 / 288000 | 觸發 Py/C++ | 事件與計數器 |
|---|---:|---:|---:|---:|---|
| tone_clicks_seed0 | 1.490e-08 | 258500 | 64 | 16/16 | 相同 |
| tone_clicks_seed1 | 7.451e-09 | 48710 | 39 | 16/16 | 相同 |
| tone_clicks_seed2 | 1.490e-08 | 192574 | 26 | 15/15 | 相同 |
| tone_clicks_seed3 | 1.863e-09 | 252704 | 8 | 14/14 | 相同 |
| tone_strong_clicks | 3.725e-09 | 48895 | 33 | 12/12 | 相同 |
| tone_very_strong_clicks | 3.725e-09 | 168454 | 68 | 12/12 | 相同（11 次 W=256） |
| silence_clicks | 0 | — | 0 | 10/10 | 相同（10 次 low_energy bypass） |
| noise | 0 | — | 0 | 0/0 | 相同 |
| loud_clipped | 2.980e-08 | 176587 | 26 | 15/15 | 相同 |

- **容許值**：事件（觸發 sample、W、是否 bypass）與全部計數器必須**完全相同**；連續值誤差 ≤1e-6（float32 輸出）。實測最大 2.98e-8，相當於輸出量級下 1–2 個 float32 ULP；R_db／S_dbfs 誤差 ≤7.1e-15。
- 誤差來源（假說）：BLAS 點積／LAPACK 與本移植的加總順序、特徵值演算法不同，差異落在修補段（AR 外推）附近；不是逐位元相同。
- 涵蓋路徑：觸發、W=128、W=256、極點投影（每次觸發都有）、low_energy bypass。**未涵蓋**：`solve_failure`、`nonfinite_prediction`、QR 不收斂（合成訊號未觸發；邏輯照抄但未經實測比對）。

## Project 3 整合驗證（`build/kcs_tests`，`make test` 內）

- 規格：44.1k／16k、立體聲、frames 256、hop 256、lookahead <492、未知版本 → 全部拒絕。
- 固定延遲對齊：同步 worker、callback 128／64／32 frames，engine 輸出逐 sample 等於 `Pipeline48k` 輸出平移 640−492，0 fallback。
- 缺口：跳過 3 個 block，不發布跨缺口 block、順序遞增、之後等於 fresh pipeline。
- 真實 worker、callback 以 2.667 ms 節拍（**參考值，非隔離量測**）：1120 processed blocks、0 fallback、0 stale。

## 未驗證

- 真實裝置、端到端延遲（本機無有線介面，見 SPEC §0.1）。
- 隔離環境下每 block 運算時間（KCS `bridge48k_timing_prediction_v1.md` 的預測格未對照）。
- KCS 素材（v13／holdout）上的品質；sanitizer；Linux／GCC。
- `project3_live --processor kcs-latest` 已建置、錯誤設定拒絕已測，**未開啟裝置播放**。
