# NE301 自动 OTA 升级与调试指南（Agent 用）

> 本文档面向在本仓库（main 分支标准固件）工作的自动化 Agent，总结已实测的 OTA 升级与调试全流程。
> **每台设备的 IP、admin 密码、串口 COM 号都不同**——先填好第 1 节的设备登记表，再执行后续命令。
> 所有命令中的 `<DEV_IP>` `<ADMIN_PW>` `<COMx>` 均为占位符。

---

## 1. 设备登记表（先填这个）

| 设备别名 | IP | admin 密码 | UART COM（优先） | USB CDC COM（备用） | 备注 |
|---|---|---|---|---|---|
| lab-93.90 | 192.168.93.90 | （问操作者，勿写入文件） | （问操作者） | （插 USB 后枚举，见 §5） | PoE 上行，静态地址 |
| （新设备） | 192.168.10.10 | （问操作者） | — | — | 出厂默认：AP 热点 / USB RNDIS 共用此地址 |

要点：

- **默认地址** `192.168.10.10` 仅适用于 AP 热点与 USB RNDIS 两种接入；配置了 PoE/WiFi 上行的设备用自己的地址。**不要猜 IP**，向操作者要。
- 密码属于设备凭据，不要写入任何会提交或外发的文件。

---

## 2. 构建产物

仓库根目录（`E:\stm_workspace\ne301`）执行，产物在 `build/`：

```bash
make pkg-app    # APP 固件 → build/ne301_App_signed_v<版本>_pkg.bin（约 4MB，构建数分钟）
make pkg-web    # Web 界面 → build/ne301_Web_v<版本>_pkg.bin（约 290KB，含 npm 构建）
```

版本号形如 `4.3.1.<build>`，**build 号由分支提交数派生**：

- 切分支后版本号可能**变小**（如 197 → 160）。OTA **无降级校验**，照常可刷，仅固件页面显示的版本号变小，不是故障。
- 需要显式控制版本：`make pkg-app VERSION_BUILD=200`。

---

## 3. OTA 升级流程（核心）

### 3.1 上传 + 重启 + 等待上线

```bash
DEV_IP=<DEV_IP>; ADMIN_PW=<ADMIN_PW>

# 1. 上传（firmwareType: app | web | ai）
curl -s -m 300 -u admin:$ADMIN_PW -X POST \
  "http://$DEV_IP/api/v1/system/ota/upload?firmwareType=app" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @build/ne301_App_signed_v4.3.1.163_pkg.bin
# 期望: {"success":true,"message":"Upgrade successful"}

# 2. 重启（响应含 delay_seconds:3；设备约 10 秒回来）
curl -s -m 10 -u admin:$ADMIN_PW -X POST "http://$DEV_IP/api/v1/system/restart" \
  -H "Content-Type: application/json" -d '{}'

# 3. 轮询等待上线
sleep 10
for i in 1 2 3 4 5 6; do
  V=$(curl -s -m 5 -u admin:$ADMIN_PW "http://$DEV_IP/api/v1/device/info" | grep -o '"software_version":"[^"]*"')
  [ -n "$V" ] && echo "上线: $V" && break
  sleep 10
done
```

### 3.2 各固件类型的重启语义（重要）

| 类型 | firmwareType | 上传后是否需重启 | 说明 |
|---|---|---|---|
| APP | `app` | **必须** | 写入非活动分区并激活，重启后运行新版 |
| Web | `web` | **必须** | **Web OTA 必须重启，没有热加载**——上传响应成功 ≠ 生效，界面重启后才换；新界面还依赖配套 APP 的 API |
| AI 检测模型 | `ai` | **必须** | 自动提升为活动模型，运行中的检测模型无法热切（标准固件只有这一个模型槽，**没有模型 B**） |

配套升级（APP+WEB 一起）的推荐顺序：**传 APP → 传 WEB → 重启一次（三类包都必须重启）→ 浏览器 Ctrl+F5 强刷**。

---

## 4. 验证与调试 API 工具箱

**约定：HTTP 状态码恒为 200，错误看响应体** `{"success":false,"error_code":"NOT_FOUND",...}`（未知路径也是这样，不要用 404 判断端点存在性）。

| 用途 | 端点 |
|---|---|
| 固件版本 / 存活探测 | `GET /api/v1/device/info` → `data.software_version` |
| AI 模型与管线状态 | `GET /api/v1/ai/status` → `data.model.status`、`data.pipeline.status` |
| 网络状态 | `GET /api/v1/system/network/status` |
| USB 开关与寻址 | `GET /api/v1/device/usb`；`POST /api/v1/device/usb/config`（`enabled`/`follow_ap`/`ip_address`/`netmask`） |
| NTP 配置 | `GET` / `POST /api/v1/system/ntp/config` |
| 日夜模式 / IR 亮度 | `GET` / `POST /api/v1/device/day-night/config`（`ir_brightness: 0` = 夜间 IR 关） |

调试技巧：

- **假死检测**：已知故障模式是"NPU 互斥锁卡死后 API 冻结但看门狗不复位"——用 15 秒间隔连续轮询 `device/info` 数分钟，全部 200 才算稳。
- **Web 换包验证**：`curl -H "Cache-Control: no-cache" http://$DEV_IP/` 取 `assets/index-<hash>.js`，哈希变了才算真的换了。
- **配置持久化验证**：改配置 → `POST /system/restart` → 回读确认（曾有过"RAM 钳制覆盖用户设定"的 bug，重启回读是唯一可信验证）。

---

## 5. 串口控制台（看日志/断言用）

设备有两种串口，**UART COM 优先**：

- **UART COM（优先）**：从复位起就有输出，**能捕获完整启动日志**；不依赖 USB 功能状态。看开机崩溃、早期断言必用这个。
- **USB CDC COM（备用）**：随 USB 枚举才出现，最早的启动日志会丢；且 main ≥ 4.3.1.163 **USB 默认关闭**（省电，NVS 记忆用户选择），新设备/恢复出厂后 CDC 不可用，需先经网络打开 USB 开关（§4 的 `POST /device/usb/config`）。

COM 号每台设备、每次插拔都可能变，先发现再打开：

```bash
python -m serial.tools.list_ports   # 需要 pyserial；或设备管理器里看
```

- 断言格式：`[ASSERT] <相对路径>:<行号> (<函数>): <条件>`——直接按文件行号回源码定位。

---

## 6. 已知坑清单

1. **commitlint**：提交 subject 必须全小写（husky 钩子，含大写如 "IR" 会被拒）。被拒会中断 `&&` 链——检查后续命令是否真的执行了。
2. **版本号跨分支回退**是正常现象（见 §2），OTA 不会被拒。
3. **WEB 上传不会热加载**：上传响应成功后界面仍是旧的，必须重启设备 + 浏览器 Ctrl+F5，再以 bundle 哈希确认换包（即便串口日志打出 assets reloaded 也不算数）。
4. **API 文档同步**：改动 API 相关文件（如 `Custom/Services/Web/api/`）后，必须同步更新 API 文档，否则 GitHub 检查（CI）过不了。
5. **AI 断言卡死**：`pp_*` 后处理 assert 发生在 NPU 互斥锁内，设备假死不复位；网络侧识别方法见 §4，根因定位走 UART 串口（§5）。

## 7. 命令速查（填好变量即可整段复制）

```bash
DEV_IP=<DEV_IP>; ADMIN_PW=<ADMIN_PW>; FW=build/ne301_App_signed_v4.3.1.163_pkg.bin

curl -s -m 300 -u admin:$ADMIN_PW -X POST "http://$DEV_IP/api/v1/system/ota/upload?firmwareType=app" -H "Content-Type: application/octet-stream" --data-binary @$FW
curl -s -m 10 -u admin:$ADMIN_PW -X POST "http://$DEV_IP/api/v1/system/restart" -H "Content-Type: application/json" -d '{}'
sleep 10
curl -s -m 5 -u admin:$ADMIN_PW "http://$DEV_IP/api/v1/device/info" | grep -o '"software_version":"[^"]*"'
```
