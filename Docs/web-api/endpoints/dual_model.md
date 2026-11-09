---
title: dual_model Endpoints
---

<!-- GENERATED FILE - do not edit manually. Regenerate with Script/gen_web_api_docs.py -->

# dual_model Endpoints



Source: [`Custom/Services/Web/api/api_dual_model_module.c`](https://github.com/camthink-ai/ne302/blob/main/Custom/Services/Web/api/api_dual_model_module.c)

**4** endpoints. The ✅ marker in the Auth column means the request must carry [credentials](../authentication.md).

| Method | Path | Auth | Handler |
|--------|------|:----:|---------|
| `GET` | `/api/v1/dual-model/config` | ✅ | `dual_model_get_config_handler` |
| `POST` | `/api/v1/dual-model/config` | ✅ | `dual_model_set_config_handler` |
| `GET` | `/api/v1/dual-model/status` | ✅ | `dual_model_status_handler` |
| `POST` | `/api/v1/dual-model/reload` | ✅ | `dual_model_reload_handler` |
