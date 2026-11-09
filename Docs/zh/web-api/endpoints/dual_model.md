---
title: dual_model 端点参考
---

<!-- GENERATED FILE - 由 Script/gen_web_api_docs.py 自动生成，请勿手工编辑 -->

# dual_model 端点参考



源文件: [`Custom/Services/Web/api/api_dual_model_module.c`](https://github.com/camthink-ai/ne302/blob/main/Custom/Services/Web/api/api_dual_model_module.c)

共 **4** 个端点。鉴权列 ✅ 表示需要携带[认证凭据](../authentication.md)。

| 方法 | 路径 | 鉴权 | 处理函数 |
|------|------|:----:|----------|
| `GET` | `/api/v1/dual-model/config` | ✅ | `dual_model_get_config_handler` |
| `POST` | `/api/v1/dual-model/config` | ✅ | `dual_model_set_config_handler` |
| `GET` | `/api/v1/dual-model/status` | ✅ | `dual_model_status_handler` |
| `POST` | `/api/v1/dual-model/reload` | ✅ | `dual_model_reload_handler` |
