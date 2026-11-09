---
title: face_recognition Endpoints
---

<!-- GENERATED FILE - do not edit manually. Regenerate with Script/gen_web_api_docs.py -->

# face_recognition Endpoints



Source: [`Custom/Services/Web/api/api_face_recognition_module.c`](https://github.com/camthink-ai/ne302/blob/main/Custom/Services/Web/api/api_face_recognition_module.c)

**14** endpoints. The ✅ marker in the Auth column means the request must carry [credentials](../authentication.md).

| Method | Path | Auth | Handler |
|--------|------|:----:|---------|
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
