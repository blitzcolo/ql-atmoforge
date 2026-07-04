# ql-atmoforge — MODTRAN 4 大气数据生成器

批量产出 **(输入参数向量 → 大气光谱量)** 样本对，作为下游深度学习代理模型的训练集。
独立于主项目，C++17 + CMake，Windows / Linux 双端；MODTRAN 二进制按安装路径调用，不复制。

每个样本 = 一组大气/几何/太阳条件，按波段跑 2–3 次 MODTRAN：

| Run | IEMSCT | 路径 | 产出 |
|---|---|---|---|
| `tau` | 0 | 视线路径 | 透过率全分量（21 列：TOTAL、H2O、CO2P、O3、AER、连续吸收…） |
| `lpath` | 2 | 视线路径 | 辐亮度全分量（12 列：PTH_THRML、SOL_SCAT、TOTAL_RAD…） |
| `ldown` | 2 | 目标位置向上仰视 | 下行天空辐亮度（仅 `thermal` 波段：mwir/lwir） |

SURREF=0、TPTEMP=0 固定：数据集只含**纯大气量**。地表/目标的反射、发射项由下游用
τ 和 L_down 按辐射传输方程自行合成——地表参数进输入向量是数据结构错误，一个样本
应当对任意地表可复用。

红外波段每样本得到三元组 **{τ_view, L_path_view, L_down_thermal}**；可见光/SWIR 只有前两个。

## 1. 构建

唯一第三方依赖是 vendored 的 `third_party/nlohmann/json.hpp`，无包管理器，无网络要求。

```sh
# 纯 Linux（配 Linux 版 MODTRAN）
cmake -B build && cmake --build build

# Windows，或 WSL 内编 Windows 原生程序（配 MOD4v1r1.EXE）
cmake.exe -B build && cmake.exe --build build --config Release
# 产物：build/Release/ql-atmoforge.exe
```

**规则：ql-atmoforge 和 MODTRAN 必须在同一侧。** MODTRAN 是 Windows 版 `MOD4v1r1.EXE` 就把
ql-atmoforge 编成 Windows 程序（WSL 里用 `cmake.exe` 即可，产物照样能在 WSL shell 里执行）。
Windows 版 ql-atmoforge 看不见 WSL 原生文件系统——配置文件、out_dir、exe/Data 路径全部必须在
`/mnt/<盘>` 下，且配置里写 Windows 风格路径（`D:/...`）。`doctor` 会检查这条。

## 2. 快速开始

```sh
export MODTRAN_EXE=C:/path/to/MOD4v1r1.EXE    # 或直接改配置默认值
export MODTRAN_DATA=C:/path/to/Data

./build/Release/ql-atmoforge.exe doctor configs/demo.json      # 环境自检 + 试跑一条
./build/Release/ql-atmoforge.exe print  configs/demo.json -i 0 # 看第 0 号样本参数 + 三张 tape5
./build/Release/ql-atmoforge.exe gen    configs/demo.json      # 生成（可中断，重跑自动续）
./build/Release/ql-atmoforge.exe merge  configs/demo.json      # shards -> npy 数据集
```

自带两个示例：`configs/demo.json`（mwir 斜程）、`configs/demo_horizontal.json`（lwir 横程）。

## 3. 配置详解

```jsonc
{
  "modtran": {
    "exe":      "${MODTRAN_EXE:-C:/path/to/MOD4v1r1.EXE}",  // ${VAR:-default} 环境变量展开
    "data_dir": "${MODTRAN_DATA:-C:/path/to/Data}",
    "timeout_s": 300,          // 单次运行超时；超时杀整个进程树（进程组 / Job Object）
    "link_mode": "auto"        // auto | junction | symlink | copy，见 §6
  },
  "run": {
    "out_dir": "out/mwir_slant_v1",
    "workers": 0,              // 并发 MODTRAN 实例数；0 = 硬件线程数
    "seed": 42,
    "n_samples": 10000,
    "sampler": "sobol",        // sobol | random | grid
    "csv_preview": 5           // merge 时导出前 N 个样本的抽查 CSV
  },
  "band": { "preset": "mwir" },        // 见 §3.2
  "path_type": "slant_to_ground",      // horizontal | slant_to_ground，见 §3.3
  "fixed":   { "iday": 93 },           // 固定参数；两边都没写的参数用内置默认值
  "sampled": {                         // 采样维度；声明顺序 = 特征向量布局
    "vis_km": { "log_uniform": [0.5, 50.0] }
  },
  "columns": "all"                     // 或列名白名单数组，merge 时按列裁剪
}
```

### 3.1 参数清单（18 个）

每个参数分派进 `fixed` 或 `sampled` 之一，或都不写（用默认值）。同名同时出现在两边是配置错误。

| 参数 | 含义 | 合法域 | 默认 |
|---|---|---|---|
| `atmos_model` | 模式大气：1 热带 / 2 中纬夏 / 3 中纬冬 / 4 亚北极夏 / 5 亚北极冬 / 6 美国标准 | {1..6} | 2 |
| `ihaze` | 气溶胶：0 无 / 1 乡村 / 2 乡村霾 / 4 海洋 / 5 城市 / 6 对流层 / 8 平流雾 / 9 辐射雾 / 10 沙漠 | {0..6,8,9,10} | 1 |
| `icld` | 云/雨：0 无 / 1–10 水云（**6 = 雨模型**）/ 18,19 卷云 | {0..10,18,19} | 0 |
| `vis_km` | 能见度 [km]；0 = 用 IHAZE 内置默认 | [0, 300] | 0 |
| `rainrt_mm_h` | 降雨率 [mm/h]；**仅 icld=6 时生效，否则自动钳 0**（条件维度） | [0, 100] | 0 |
| `t_ground_K` | 地面温度 [K]（写入廓线最低层，JCHAR='AAH'） | [200, 340] | 288.15 |
| `rh` | 地面相对湿度 | [0, 1] | 0.5 |
| `p_hPa` | 地面气压 [hPa] | [500, 1100] | 1013.25 |
| `h2o_scale` | 整柱水汽缩放因子（Card 1A H2OSTR） | [0.01, 10] | 1.0 |
| `o3_scale` | 整柱臭氧缩放因子（Card 1A O3STR） | [0.01, 10] | 1.0 |
| `co2_ppmv` | CO₂ 浓度（Card 1A CO2MX） | [100, 1000] | 365 |
| `h1_km` | 传感器高度 [km]；slant 时必须 > 0 | [0, 99] | 0 |
| `range_km` | 路径长度 [km]；**仅 horizontal 可采样** | [0.001, 1000] | 1.0 |
| `view_zenith_deg` | 观测天顶角 [°]，>90 = 向下看；**仅 slant 可采样** | (90, 180] | 180 |
| `sun_zenith_deg` | 太阳天顶角 [°]（IPARM=2，辐亮度运行用） | [0, 89.9] | 45 |
| `sun_rel_azimuth_deg` | 视线→太阳相对方位角 [°]，北偏东为正 | [-180, 180] | 0 |
| `iday` | 年内日序（只影响日地距离），一般固定 | [1, 365] | 93 |
| `ldown_zenith_deg` | ldown 运行的仰视天顶角 [°]，一般固定 | [0, 89.9] | 45 |

采样分布三种写法：

```json
"icld":       { "values": [0, 5, 6, 18] },        // 离散集合（离散型整数参数必须用这个）
"t_ground_K": { "uniform": [230.0, 320.0] },      // 均匀分布
"vis_km":     { "log_uniform": [0.5, 50.0] }      // 对数均匀（跨数量级的量用这个）
```

`grid` 采样器要求每个**连续**维度额外给 `"grid_n": N`（该维取 N 个单元中心）；
离散维度的格点数 = values 个数；总格点数 < n_samples 时报错。

### 3.2 波段（整套固定，决定输出向量维度 K）

| 预设 | V1–V2 (cm⁻¹) | 波长 | K | thermal（跑 ldown） |
|---|---|---|---|---|
| `vis` | 12500–25000 | 0.4–0.8 µm | 12501 | 否 |
| `nir1064` | 8333–10753 | 0.93–1.2 µm | 2421 | 否 |
| `swir1535` | 6061–7143 | 1.4–1.65 µm | 1083 | 否 |
| `mwir` | 2000–3333 | 3–5 µm | 1334 | 是 |
| `lwir` | 833–1250 | 8–12 µm | 418 | 是 |

自定义：`"band": {"name": "myband", "v1_cm": 4000, "v2_cm": 5000, "thermal": false}`，
可选 `dv_cm`（默认 1）、`fwhm_cm`（默认 2，三角狭缝）。

**一个数据集 = 一个波段 + 一个路径类型。** K 不同的东西不要塞进同一个数组。

### 3.3 几何语义

- **horizontal**：采样自由度 `(h1_km, range_km)`；天顶角恒 90°，H2=H1。
  ldown（若有）在 h1 高度向上看。
- **slant_to_ground**（空对地）：采样自由度 `(h1_km, view_zenith_deg)`；H2≡0 是路径类型
  的定义。斜程长度由球面无折射几何派生并存进特征向量。天顶角低于擦地临界角时钳到
  临界值 +0.05°，**钳后值如实写进 tape5 和特征向量**（不产生 X/y 错配）。
  ldown 在地面（H2=0 处）向上看——它下游的用途是目标对天空辐射的反射。

**特征向量布局**：`[采样维度按 sampled 声明顺序] + [h1_km, h2_km, cos_view_zenith, range_km]`。
天顶角以 cos 形式追加（有界、光滑、正比气团数），几何量冗余存全。布局写在 manifest 的
`feature_names` 里——**读它，别自己数列号**。

### 3.4 复现性

`index → 参数` 是 `(seed, index)` 的纯函数：Sobol 按 index 直接求值（Joe–Kuo 方向数，
≤40 维），Random 是 counter-based splitmix64，Grid 是混合进制解码。worker 数量、调度
顺序、断点续跑均不影响任何样本的内容。同配置同 seed，任何机器上第 N 号样本恒同。

## 4. 批量生产流程

以 1 万条 mwir 斜程为例：

```sh
# 1. 抄 demo.json 改 out_dir / n_samples / workers / 采样范围
# 2. 体检 + 抽查
./build/Release/ql-atmoforge.exe doctor configs/mwir_slant_v1.json
./build/Release/ql-atmoforge.exe print  configs/mwir_slant_v1.json -i 0
# 3. 生产。可随时 Ctrl+C / 断电，重跑同一命令自动跳过已完成样本
./build/Release/ql-atmoforge.exe gen    configs/mwir_slant_v1.json
# 4. 合并
./build/Release/ql-atmoforge.exe merge  configs/mwir_slant_v1.json
```

- **失败不中断**：单样本失败被记录（`status=2` + `failures/<index>/` 验尸存档）后继续跑。
- **增量扩容**：把 `n_samples` 从 10000 改 20000 再 `gen`，前 1 万条直接跳过。
  但**不要动 seed、sampler、sampled 的任何范围**——那等于换数据集。维度数变化 shard
  校验能拦住，范围变化拦不住，自己管住手。
- **速度参考**（实测，单 worker）：mwir 每样本 3 次运行约 1 s；lwir 约 0.6 s；vis 波段
  （K=12501）慢一个量级以上，首批建议从 mwir/lwir 起步。
- **重新裁列不用重跑**：shard 里永远存全列，`columns` 白名单只在 merge 阶段生效，
  改完白名单重新 `merge` 即可。

## 5. 输出格式与 Python 用法

```
out_dir/
├── manifest.json       # 特征布局、列名、采样范围、seed、波段网格、成败计数、exe 指纹
├── params.npy          # float64 [N, P]   特征向量
├── tau.npy             # float32 [N, 21, K]  透过率分量（无量纲；LOG_TOTAL 列是 -ln τ）
├── lpath.npy           # float32 [N, 12, K]  路径辐亮度分量 [W·cm⁻²·sr⁻¹/cm⁻¹]
├── ldown.npy           # float32 [N, 12, K]  下行辐亮度（仅 thermal 波段）
├── wavenumber.npy      # float32 [K]  cm⁻¹ 升序（波长换算交给下游）
├── index.npy           # uint64 [N]  样本号
├── status.npy          # uint8  [N]  0=ok  1=partial  2=failed
├── failures.jsonl      # 每条失败样本：完整参数 + 错误摘要 + 失败阶段
├── preview/            # csv_preview 个抽查 CSV（波数 × 全列，Excel 可画）
├── shards/             # 运行时定长记录分片（续跑依据；merge 后可删可留）
├── workers/worker_*/   # 持久工作目录（Data 链接 + 运行日志）
└── failures/<index>/   # 失败/警告样本的 tape5/tape6/日志存档，供验尸
```

```python
import json, numpy as np

d = "out/mwir_slant_v1/"
man    = json.load(open(d + "manifest.json"))
X      = np.load(d + "params.npy")                # [N, P]，列名 = man["feature_names"]
tau    = np.load(d + "tau.npy",   mmap_mode="r")  # 大数据集用 mmap，不整块进内存
lpath  = np.load(d + "lpath.npy", mmap_mode="r")
ldown  = np.load(d + "ldown.npy", mmap_mode="r")
status = np.load(d + "status.npy")

good  = status < 2                                # 剔除 failed（其光谱是全零占位）
i_tau = man["arrays"]["tau"]["columns"].index("TOTAL")
i_rad = man["arrays"]["lpath"]["columns"].index("TOTAL_RAD")
y_tau = tau[good][:, i_tau, :]                    # [N_good, K]
```

**status=1 (partial)** 表示光谱数据完整、但带 MODTRAN 警告（水柱超上限被内部截断、
液态水滴密度截断等），警告全文在 `failures/<index>/warnings.txt`。取舍自定。

透过率 21 列：`TOTAL H2O CO2P O3 TRACE N2_CONT H2O_CONT MOLEC_SCAT AER HNO3 AER_AB
LOG_TOTAL CO2 CO CH4 N2O O2 NH3 NO NO2 SO2`（H2O 是线吸收，H2O_CONT 是连续吸收）。
辐亮度 12 列：`TOT_TRANS PTH_THRML THRML_SCT SURF_EMIS SOL_SCAT SING_SCAT GRND_RFLT
DRCT_RFLT TOTAL_RAD REF_SOL SOL_OBS DEPTH`（SURREF=0 下 GRND_RFLT/DRCT_RFLT 恒 0，
留列是为了格式统一）。

选 npy 的理由：下游是 Python/numpy，`np.load` 直读 + mmap；写入约 60 行 C++ 零依赖。

**体积**：全列 float32 下 mwir ≈ 250 KB/样本（1 万条 ≈ 2.5 GB），vis ≈ 1.2 MB/样本
（1 万条 ≈ 12 GB）。大批量先想清楚要哪些列。

## 6. 并行与 DATA 目录（重要）

MODTRAN 相对 **cwd**（不是 exe 所在目录）寻找 `DATA/`（实验证实）。ql-atmoforge 给每个
worker 一个持久目录 `workers/worker_<i>/`，初始化时建一次 Data 链接，跨运行复用：

| 环境 | `link_mode: auto` 的行为 |
|---|---|
| Windows | NTFS junction（`mklink /J`，无需管理员权限） |
| WSL 调 Windows exe | 同样走 `cmd.exe` junction —— WSL 的 `ln -s` 创建 LX 重解析点，Windows 进程**无法跟随**（实测） |
| 纯 Linux + Linux 版 MODTRAN | `symlink`（basename 非 DATA 时补一个大写 `DATA` 链接） |
| 万能降级 | `copy`（每 worker ~26 MB） |

调度：原子计数器发放样本号，每 worker 一个线程（线程只负责等子进程），每 worker 一个
shard 文件，无锁无争用。`workers` 就是并发 MODTRAN 实例数，MODTRAN 是纯 CPU 单线程
程序，开到物理核数即可。

## 7. 已知坑（实现里都处理了，但你该知道）

1. **MODTRAN 退出码不可信**：fatal error 也返回 0。成败判据 = tape6 末尾 `CARD 5`
   标记 + tape7 解析出与配置一致的完整光谱网格。
2. **Windows delete-pending 竞态**：杀毒/索引器扫描刚生成的输出文件时，"删旧文件 +
   立即重建同名"会让 MODTRAN 启动即死（`*ERR* IO-09`，tape6 零字节）。因此每次运行用
   唯一文件前缀（`r000042_.tp5` 式，经 modroot.in 机制），从不重建同名文件；另有一次
   自动失败重试兜底。这个坑是概率性的，串行测试永远遇不到。
3. **打印下限饱和**：浓雾/雨 + 长路径下 tape7 透过率打印为 0.0000，光谱压平成零
   （`LOG_TOTAL` 列仍有信息）。生成器如实记录不钳制；训练前按
   `tau[:, i_TOTAL, :].max(-1) < 阈值` 过滤，或收窄 `vis_km` / `range_km` 的采样范围。
4. **辐亮度表的空白字段**（如无多次散射时 `THRML_SCT` 整列）是 MODTRAN 打印精确零的
   方式。按定宽切列后记 0，不算缺数据、不打警告。
5. **T/RH/P 覆盖在地面层**，不在传感器高度——气象旋钮描述的是地面天气。这是对主项目
   旧实现的有意修正（旧代码注入 h1 层，对横程碰巧对，对斜程语义错误）。
6. `h2o_scale` 缩放的是**整柱**水汽（含地面 RH 覆盖后的廓线），和 `rh` 不是一回事：
   `rh` 改地面层，`h2o_scale` 乘整列。两个都采样时注意组合可能触发水柱超限警告（partial）。

## 8. 目录结构与 CLI

```
ql-atmoforge/
├── CMakeLists.txt
├── README.md / DESIGN.md          # DESIGN.md 含完整设计决策与实现偏差记录
├── configs/demo.json              # mwir + 斜程示例
├── configs/demo_horizontal.json   # lwir + 横程示例
├── third_party/nlohmann/json.hpp  # 唯一第三方件（vendored 单头）
└── src/
    ├── main.cpp          # 子命令分发：gen / merge / print / doctor
    ├── config.{h,cpp}    # schema 解析 + ${VAR:-default} 展开 + 校验
    ├── params.{h,cpp}    # 参数描述表、ParamSpace（index -> 参数，纯函数）
    ├── sampler.{h,cpp}   # Sobol / Random / Grid
    ├── tape5.{h,cpp}     # 参数 x RunKind -> tape5 文本
    ├── subprocess.{h,cpp}# 跨平台 spawn/cwd/超时/日志（POSIX 进程组 / Job Object）
    ├── parse.{h,cpp}     # tape6 状态判定 + tape7 两种表解析
    ├── shard.{h,cpp}     # 定长记录分片 + 续跑扫描 + 截尾恢复
    ├── npy.{h,cpp}       # 流式 .npy 写入 + merge
    ├── worker.{h,cpp}    # worker 主循环、失败存档、重试
    └── datalink.{h,cpp}  # Data junction/symlink/copy
```

```
ql-atmoforge gen    <config.json>            生成（可续跑）
ql-atmoforge merge  <config.json>            shards -> npy 数据集（可重复执行）
ql-atmoforge print  <config.json> -i <N>     打印第 N 号样本参数 + 特征向量 + 三张 tape5
ql-atmoforge doctor <config.json>            环境自检：exe/Data/路径可达性/链接方式/试跑一条
```
