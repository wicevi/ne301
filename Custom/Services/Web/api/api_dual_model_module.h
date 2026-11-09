/**
 * @file api_dual_model_module.h
 * @brief Generic dual-model (Model A -> crop -> Model B) engine API module
 */

#ifndef API_DUAL_MODEL_MODULE_H
#define API_DUAL_MODEL_MODULE_H

#include "aicam_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the dual-model engine module
 * @return aicam_result_t Operation result
 */
aicam_result_t web_api_register_dual_model_module(void);

#ifdef __cplusplus
}
#endif

#endif /* API_DUAL_MODEL_MODULE_H */
