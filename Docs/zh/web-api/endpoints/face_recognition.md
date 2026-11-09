---
title: face_recognition 端点参考
---

<!-- GENERATED FILE - 由 Script/gen_web_api_docs.py 自动生成，请勿手工编辑 -->

# face_recognition 端点参考



源文件: [`Custom/Services/Web/api/api_face_recognition_module.c`](https://github.com/camthink-ai/ne302/blob/main/Custom/Services/Web/api/api_face_recognition_module.c)

共 **14** 个端点。鉴权列 ✅ 表示需要携带[认证凭据](../authentication.md)。

| 方法 | 路径 | 鉴权 | 处理函数 |
|------|------|:----:|----------|
| `GET` | `/api/v1/face-recognition/config` | ✅ | `fr_get_config_handler` |
| `POST` | `/api/v1/face-recognition/config` | ✅ | `fr_set_config_handler` |
| `GET` | `/api/v1/face-recognition/status` | ✅ | `fr_status_handler` |
| `GET` | `/api/v1/face-recognition/results` | ✅ | `fr_results_handler` |
| `GET` | `/api/v1/face-recognition/faces` | ✅ | `fr_faces_list_handler` |
| `POST` | `/api/v1/face-recognition/faces/enroll` | ✅ | `fr_faces_enroll_handler` |
| `GET` | `/api/v1/face-recognition/faces/enroll/status` | ✅ | `fr_faces_enroll_status_handler` |
| `POST` | `/api/v1/face-recognition/faces/enroll/capture` | ✅ | `fr_faces_enroll_capture_handler` |
| `POST` | `/api/v1/face-recognition/faces/enroll/finish` | ✅ | `fr_faces_enroll_finish_handler` |
| `POST` | `/api/v1/face-recognition/faces/enroll/cancel` | ✅ | `fr_faces_enroll_cancel_handler` |
| `POST` | `/api/v1/face-recognition/faces/delete` | ✅ | `fr_faces_delete_handler` |
| `POST` | `/api/v1/face-recognition/faces/rename` | ✅ | `fr_faces_rename_handler` |
| `POST` | `/api/v1/face-recognition/faces/clear` | ✅ | `fr_faces_clear_handler` |
| `POST` | `/api/v1/face-recognition/reset` | ✅ | `fr_reset_handler` |
