/**
 * @file api_face_recognition_module.h
 * @brief Face recognition demo API module (local dual-model recognition)
 */

#ifndef API_FACE_RECOGNITION_MODULE_H
#define API_FACE_RECOGNITION_MODULE_H

#include "aicam_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the face recognition module
 * @return aicam_result_t Operation result
 */
aicam_result_t web_api_register_face_recognition_module(void);

#ifdef __cplusplus
}
#endif

#endif /* API_FACE_RECOGNITION_MODULE_H */
