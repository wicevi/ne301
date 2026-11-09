# NE302 本地双模型人脸识别 Demo（Face Recognition）

分支：`feature/face-recognition-demo`

**全本地**双模型人脸识别 + 特征匹配示例：模型A（人脸检测）+ 模型B（人脸特征提取）在设备端串行推理，检测结果裁剪 → letterbox 缩放 → 特征提取 → 与本地人脸库做余弦相似度匹配，识别结果叠加在实时画面与 Web 页面上。不依赖任何云端服务（区别于 `feature/face-detection-demo` 的云端识别方案）。

设计参考：ST 案例研究 [id3 MicroFace on STM32N6（检测→特征→本地库匹配）](https://mp.stmicroelectronics.cn/content/st_com/zh/st-edge-ai-suite/case-studies/secure-entry-systems-using-id3-face-recognition-with-liveness-detection.html) 与 [双模型门禁](https://www.st.com.cn/content/st_com/zh/st-edge-ai-suite/case-studies/access-control-with-face-recognition.html)（本 demo 未包含活体检测——ToF 活体不在双模型范围内，列为后续方向）。

## 一、通用双模型引擎（Dual-Model Engine）

本分支将"两级模型"能力沉淀为**通用引擎**（`Custom/Core/Video/video_dual_model.{c,h}`），人脸识别只是它的第一个应用，后续车牌识别等两阶段任务可直接复用：

```
AI node 线程 (frame hook, 仅持有 pipe2 约 10ms):
  模型A推理 (NPU, 激活槽位 AI_1, ~7ms)
    1. 解析模型A结果 (OD / MPE 通用, 含关键点透传)
    2. 过滤(置信度/类别/最小尺寸) + 按面积排序, 取前 K 个
    3. 目标回调(跟踪/绘制状态立即更新) + 入队闸门判定
    4. 需要识别 → 检测框外扩 → 裁剪 + letterbox 到模型B输入 → 写入任务槽
    5. 归还 pipe2 缓冲 (不等模型B!)

模型B worker 线程 (专属):
  取任务槽 → 模型B推理 (NPU, 非激活槽位, 全局NPU互斥串行)
    → 余弦匹配 + 投票 → 按 track_id 回写身份 → 前端/OSD 异步刷新

入队闸门 (每 track): 新track确认后首次必推; infer_pending 期间不重复推
  (同track旧任务被新裁剪覆盖); 已知身份每4s刷新 / 未知每2s重试;
  任务槽满丢弃并立即释放pending(3s超时兜底); track过期则结果作废

车牌识别(未来): 同一引擎, 换 A/B 模型与应用层
```

关键事实：

- **双模型激活内存不冲突**：relocatable 模型安装时激活内存按实例从 PSRAM 堆分配（`nn.c` `hal_mem_alloc_large`），模型A/B 各自独立；NPU 访问由 `g_npu_mutex` 全局互斥自动串行。
- **模型B = 非激活槽位**：`ai_1_active` NVS 标志决定模型A所在槽位，模型B自动取另一个（与 people-counting demo 的互补地址约定一致）。
- **优雅降级**：模型B缺失/加载失败时仅检测继续，Web 页显示"识别模型未加载"；连续 3 次推理错误触发熔断，`POST /api/v1/dual-model/reload` 恢复。
- **frame hook 时序**：在主推理成功后、pipe2 缓冲归还相机前同步执行（像素仍有效）；每周期耗时 = 检测(~7ms) + K×(warp+模型B推理)。

## 二、双模型准备与烧录

| 模型 | 角色 | 槽位 | 地址 | 权重 |
|------|------|------|------|------|
| YuNet 320×320 RGB（模型A） | 人脸检测 + 5关键点 | AI_1（激活槽位） | `0x70900000` | `Model/weights/yunetn_320_qdq_int8_rgb_fd_ui.onnx`（移植自 face-detection-demo 分支） |
| MobileFaceNet 112×112 RGB（模型B） | 512维人脸特征 | AI_2（非激活槽位） | `0x71100000` | `Model/weights/mobilenet_face_embed_112_qdq_int8_rgb.onnx` |

模型B由 `Script/prepare_face_embed_model.py` 生成（已生成并提交）：

1. 下载 insightface buffalo_s 的 `w600k_mbf.onnx`（MobileFaceNet-class ArcFace，112×112，512维）
2. 图内嵌入归一化 `(x-127.5)/127.5`（insightface 在图外做，NE302 直接喂原始 RGB888 uint8，必须搬进网络）
3. onnxruntime 数值校验：改图前后输出逐位一致（max diff = 0，确认该模型为 RGB 输入，无需通道交换）
4. QDQ int8 静态量化（合成人脸图像校准；`--calib-dir` 可换真实人脸图提升精度）
5. 输入重接为 uint8 + DequantizeLinear 头（ST zoo 契约，`--input-data-type uint8` 所需）

```bash
# 重新生成模型B权重（可选，仓库已含产物）
python Script/prepare_face_embed_model.py --calib-n 96

# 构建两个模型包（输出 Model/build/models/）
bash Script/build_all_models.sh yunetn_320_qdq_int8_rgb_fd_ui
bash Script/build_all_models.sh mobilenet_face_embed_112_qdq_int8_rgb

# 烧录
# 模型A (AI_1):
make flash-model MODEL_NAME=yunetn_320_qdq_int8_rgb_fd_ui
# 模型B (AI_2) — 用 CubeProgrammer 烧 _pkg.bin 到 0x71100000，或走 Web：
#   人脸识别页 → "识别模型（模型B）" → 上传（OTA 参数 ai_b → AI_2，自动热加载）
```

模型B包编译数据（stedgeai 4.0.1，`yolov8_od` reloc profile）：weights 3.33MB + activations 931KB（安装时从 PSRAM 堆分配），MACC 480M，包 3.6MB。

## 三、识别流水线（应用层）

`Custom/Core/Video/face_recognition.{c,h}` 注册为双模型引擎的结果消费者：

1. **跟踪**：以模型B的运行节奏（默认 200ms 一轮）对目标框做 IoU 跟踪，2 次命中后确认显示；按时间老化（`face_hold_ms + track_max_missed × interval`）。
2. **匹配**：`pp_face_embed` 后处理将模型B输出反量化 + L2 归一化（余弦相似度退化为点积），与 `face_db`（NVS 单 blob，变长记录）比对；注册时多帧样本归一化平均为单模板，样本一致性下限可配。
3. **投票确认**：同一身份连续 `confirm_votes` 次匹配才确认（防闪烁）；低于阈值连续确认后标记"未知"。
4. **叠加**：PIPE1 流上画框+标签（绿=已识别，灰=未知，橙=识别中；Font16 为 ASCII 点阵，中文姓名显示为 `ID:n`，Web 端显示不受限）。
5. **注册（手动多姿态）**：Web 发起后进入手动模式，每次点击"抓拍一张"采一个样本（仅正脸姿态被采样，等待时提示"等待正脸"），采满 N 张自动入库或随时"完成注册"提前入库；样本与均值一致性 ≥ `enroll_min_similarity` 才入库，超时/不一致给出明确失败原因。建议正脸/微左/微右各一张。
6. **正脸闸门（关键点）**：YuNet 5 关键点几何判定（偏航=鼻点水平偏移比、翻滚=眼线倾角、近侧脸=眼距/脸宽，垂直两项为参考）——框照常跟踪显示，但只有正脸才推送识别/采样；偏航/翻滚容忍度 Web 可调（`pose_max_yaw` 默认 60≈±55°，`pose_max_roll` 默认 40°）。

### 涉及文件

| 文件 | 变更 |
|------|------|
| `Custom/Core/Video/video_dual_model.{c,h}` + `dual_model_config.h` | 新增：通用双模型引擎（裁剪/letterbox/模型B实例/熔断/消费者回调） |
| `Custom/Core/Video/video_ai_node.{c,h}` | 新增 frame hook（主推理后、缓冲归还前，附原始像素）与输入几何查询 |
| `Custom/Core/Video/face_recognition.{c,h}` + `face_recognition_config.h` | 新增：跟踪/匹配/投票/叠加/注册 |
| `Custom/Core/Video/face_db.{c,h}` | 新增：NVS 人脸库（逐条键存储 + 元数据目录，旧 blob 自动迁移） |
| `Custom/Common/Lib/pp/pp_face_embed.c` + `pp.h` | 新增：`PP_TYPE_EMBEDDING`（反量化 + L2 归一化直通） |
| `Custom/Services/AI/ai_service.c` | 装配：装 frame hook、拉起引擎与 demo、绘制链接入 `ai_service_draw_callback` |
| `Custom/Services/Web/api/api_dual_model_module.{c,h}` | 新增：引擎 REST（config/status/reload） |
| `Custom/Services/Web/api/api_face_recognition_module.{c,h}` | 新增：demo REST（config/status/results/faces/enroll…） |
| `Custom/Core/System/json_config_*` | 新增 `dual_model`（NVS 前缀 `dm_`）与 `face_recognition`（`fr_`）配置节 |
| `Custom/Services/Web/api/api_ota_module.c` / `ota_service.c` | OTA 槽位策略：`ai`→AI_1（模型A），`ai_b`/`ai_face`→AI_2（模型B），不再自动翻转 `ai_1_active` |
| `Web/src/pages/faceRecognition/`、`services/api/faceRecognition.ts` | 新增：前端页面与 API 客户端 |
| `Script/prepare_face_embed_model.py`、`Script/face_recognition_selftest.py` | 新增：模型B准备脚本、宿主机自测 |

## 四、Web 使用

导航栏 → **人脸识别**（`/face-recognition`）。

1. **实时画面**：H.264 流 + 固件 OSD（框/姓名/相似度）；右上角统计（画面/已识别/未知/库），左上角注册采样进度，右下角模型B徽标。
2. **实时识别卡**：当前 track 列表（姓名/未知/识别中 + 相似度）。
3. **人脸注册**：输入姓名 → 开始注册（采样 n/N 进度）→ 完成/失败 toast；提示保持正脸 30-80cm。
4. **人脸库**：列表（姓名/注册时间）、重命名（回车确认）、删除、清空（确认弹窗）。
5. **识别模型（模型B）卡**：上传模型包（`ai_b` → AI_2 + 自动热加载，流自动重启）、重新加载。
6. **参数设置**：匹配阈值/投票数/注册采样、双模型引擎（开关/置信度/裁剪外扩‰/每帧上限/间隔）、跟踪、叠加显示。

## 五、REST API

通用引擎（`/api/v1/dual-model/*`）与 demo（`/api/v1/face-recognition/*`）两组，完整字段见 `docs/web-api/endpoints/dual_model.md` 与 `face_recognition.md`（`gen_web_api_docs.py` 生成）：

| 方法 | 路径 | 说明 |
|------|------|------|
| GET/POST | `/api/v1/dual-model/config` | 引擎配置（部分合并更新，范围校验，NVS 持久化） |
| GET | `/api/v1/dual-model/status` | 模型B加载状态/几何/推理统计/熔断 |
| POST | `/api/v1/dual-model/reload` | 热重载模型B（烧录新包后） |
| GET/POST | `/api/v1/face-recognition/config` | demo 配置 |
| GET | `/api/v1/face-recognition/status` | 统计 + 注册进度 + 模型B摘要 |
| GET | `/api/v1/face-recognition/results` | 当前 track（框/身份/相似度） |
| GET | `/api/v1/face-recognition/faces` | 人脸库列表 |
| POST | `/api/v1/face-recognition/faces/enroll` | `{name}` 开始注册 |
| GET | `/api/v1/face-recognition/faces/enroll/status` | 注册进度/结果/抓拍等待状态 |
| POST | `/api/v1/face-recognition/faces/enroll/capture` | 手动抓拍一个样本（仅正脸姿态生效） |
| POST | `/api/v1/face-recognition/faces/enroll/finish` | 提前完成注册（≥1 张即可） |
| POST | `/api/v1/face-recognition/faces/enroll/cancel` | 取消注册 |
| POST | `/api/v1/face-recognition/faces/delete` / `rename` / `clear` | 库管理（`{id[, name]}`） |
| POST | `/api/v1/face-recognition/reset` | 清跟踪与统计 |

```bash
# 注册 + 验证一把
curl -X POST http://192.168.10.10/api/v1/face-recognition/faces/enroll \
  -u admin:<密码> -H 'Content-Type: application/json' -d '{"name":"Alice"}'
sleep 5
curl -u admin:<密码> http://192.168.10.10/api/v1/face-recognition/faces/enroll/status
curl -u admin:<密码> http://192.168.10.10/api/v1/face-recognition/results
```

## 六、验证

- 宿主机自测（无硬件）：`python Script/face_recognition_selftest.py` —— letterbox 定点算法 vs 浮点参考（量化网格下逐位一致）、模板平均+余弦匹配、量化模型同/异主体分离度（同主体 0.94 vs 异主体 0.77，合成图；真人差距更大）。
- `make app` / `make web` 零告警；模型包 `Script/build_all_models.sh` 构建通过。
- 上机：烧录固件+双模型 → Web 注册 2 人 → 实时画面显示姓名与相似度，未注册者显示"未知"。

## 七、已知限制与后续方向

- **无活体检测**：照片/屏幕攻击可被骗过（参考 id3 案例的 ToF 方案，硬件不支持，列为后续）。
- **无关键点对齐**：当前为检测框外扩 + letterbox；YuNet 已输出 5 关键点，后续可加相似变换对齐提升小脸/侧脸精度。
- **识别节奏**：模型B（480M MACC）单次约几十 ms，默认 200ms 一轮、每轮最多 2 张脸；人多时可调大 `interval_ms` 或减小 `max_targets`。
- **校准集为合成图**：正式量产建议 `--calib-dir` 用真实人脸裁剪图重新量化，并按场景调 `match_threshold`（默认 0.60）。
- **中文姓名**：OSD 点阵字体仅 ASCII（显示回退 `ID:n`）；Web 端完整显示，库内存储 UTF-8。
