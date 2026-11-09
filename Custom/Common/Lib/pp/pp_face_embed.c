/**
******************************************************************************
* @file    pp_face_embed.c
* @brief   Embedding (feature-vector) postprocess - generic passthrough
* @details Turns a recognition network's first output tensor into a
*          PP_TYPE_EMBEDDING result: dequantizes int8/uint8 to float (using
*          the quantization parameters of the compiled network), optionally
*          L2-normalizes the vector, and exposes {vec, dim, normalized}.
*
*          Used as Model B of the dual-model engine (e.g. a MobileFaceNet
*          face-embedding network whose vectors are matched by cosine
*          similarity); also fits any "crop -> classify/embed" second-stage
*          network (license-plate embedding, re-ID, ...).
*
*          Model JSON (optional overrides):
*            "postprocess_type": "pp_face_embed",
*            "embedding_output_index": 0,     # which output tensor to read
*            "embedding_dim": 128,            # 0/absent = derive from buffer
*            "embedding_l2_normalize": true   # default true
******************************************************************************
*/

#include "pp.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "cJSON.h"
#include "mem.h"
#include "debug.h"
#include "ll_aton_runtime.h"
#include "ll_aton_reloc_network.h"

#define PP_EMBED_MAX_DIM 1024u

/* Per-instance parameters */
typedef struct {
    uint32_t output_index;    /* which network output tensor to read */
    uint32_t dim;             /* vector dimensionality used at runtime */
    uint32_t l2_normalize;    /* 1 = L2-normalize the output vector */
    float scale;              /* quant scale of the selected output */
    float zero_point;         /* quant zero point of the selected output */
    int8_t output_signed;     /* 1 = int8 output, 0 = uint8 output */
    float *vec;               /* dequantized vector (dim entries) */
} pp_face_embed_params_t;

static int32_t init(const char *json_str, void **pp_params, void *nn_inst)
{
    pp_face_embed_params_t *params =
        (pp_face_embed_params_t *)hal_mem_alloc_any(sizeof(pp_face_embed_params_t));
    if (!params) {
        return -1;
    }
    memset(params, 0, sizeof(pp_face_embed_params_t));
    params->output_index = 0;
    params->l2_normalize = 1;
    params->scale = 1.0f;
    params->zero_point = 0.0f;
    params->output_signed = 1;

    /* Optional JSON overrides (all have safe defaults) */
    if (json_str) {
        cJSON *root = cJSON_Parse(json_str);
        if (root) {
            cJSON *j;
            if ((j = cJSON_GetObjectItemCaseSensitive(root, "embedding_output_index")) &&
                cJSON_IsNumber(j)) {
                params->output_index = (uint32_t)j->valuedouble;
            }
            if ((j = cJSON_GetObjectItemCaseSensitive(root, "embedding_l2_normalize")) &&
                cJSON_IsBool(j)) {
                params->l2_normalize = cJSON_IsTrue(j) ? 1u : 0u;
            }
            /* embedding_dim acts as a sanity cap; 0/absent = derive below */
            uint32_t json_dim = 0;
            if ((j = cJSON_GetObjectItemCaseSensitive(root, "embedding_dim")) &&
                cJSON_IsNumber(j)) {
                json_dim = (uint32_t)j->valuedouble;
            }

            /* Quantization of the selected output, straight from the
             * compiled relocatable network (authoritative over JSON).
             * Only 8-bit quantized outputs are supported (bytes == elements). */
            NN_Instance_TypeDef *NN_Instance = (NN_Instance_TypeDef *)nn_inst;
            if (NN_Instance) {
                const LL_Buffer_InfoTypeDef *info =
                    ll_aton_reloc_get_output_buffers_info(NN_Instance,
                                                          params->output_index);
                if (info) {
                    /* ATON reports int8/uint8 tensors as DataType_FXP (100)
                     * with the quant scale/zero in the scale/offset fields
                     * (Qunsigned tells the sign); the plain INT8/UINT8 codes
                     * appear for other frontends. Only 8-bit outputs are
                     * supported (bytes == elements). */
                    int fxp = (info->type == DataType_FXP);
                    int is_float = (info->nbits == 32u && info->type == DataType_FLOAT);
                    if (!is_float &&
                        (info->nbits != 8u ||
                         (!fxp && info->type != DataType_INT8 && info->type != DataType_UINT8))) {
                        LOG_DRV_ERROR("pp_face_embed: output[%lu] dtype unsupported "
                                      "(nbits=%lu type=%ld)\n",
                                      (unsigned long)params->output_index,
                                      (unsigned long)info->nbits, (long)info->type);
                        cJSON_Delete(root);
                        hal_mem_free(params);
                        return -1;
                    }
                    uint32_t elements = LL_Buffer_len(info);
                    if (json_dim != 0 && json_dim <= elements) {
                        elements = json_dim;
                    }
                    if (elements == 0 || elements > PP_EMBED_MAX_DIM) {
                        LOG_DRV_ERROR("pp_face_embed: bad dim %lu (len=%lu json=%lu)\n",
                                      (unsigned long)elements,
                                      (unsigned long)LL_Buffer_len(info),
                                      (unsigned long)json_dim);
                        cJSON_Delete(root);
                        hal_mem_free(params);
                        return -1;
                    }
                    params->dim = elements;
                    if (is_float) {
                        params->output_signed = 2; /* marker: raw float32 */
                    } else if (info->type == DataType_UINT8) {
                        params->output_signed = 0;
                    } else if (fxp) {
                        params->output_signed = info->Qunsigned ? 0 : 1;
                    } else {
                        params->output_signed = 1; /* DataType_INT8 */
                    }
                    if (info->scale != NULL) {
                        params->scale = *(info->scale);
                    }
                    if (info->offset != NULL) {
                        params->zero_point = (float)(*info->offset);
                    }
                } else {
                    LOG_DRV_ERROR("pp_face_embed: output[%lu] buffer info NULL\n",
                                  (unsigned long)params->output_index);
                }
            } else {
                LOG_DRV_ERROR("pp_face_embed: no nn_inst at init\n");
            }
            cJSON_Delete(root);
        } else {
            LOG_DRV_ERROR("pp_face_embed: config JSON parse failed\n");
        }
    }

    if (params->dim == 0) {
        /* No nn_inst/JSON info (unexpected in the relocatable flow) */
        hal_mem_free(params);
        return -1;
    }

    params->vec = (float *)hal_mem_alloc_any(sizeof(float) * params->dim);
    if (!params->vec) {
        hal_mem_free(params);
        return -1;
    }

    *pp_params = params;
    return 0;
}

static int32_t run(void *pInput[], uint32_t nb_input, void *pResult,
                   void *pInput_param, void *nn_inst)
{
    (void)nn_inst;
    pp_face_embed_params_t *params = (pp_face_embed_params_t *)pInput_param;
    pp_result_t *result = (pp_result_t *)pResult;

    if (!params || !result || !pInput || nb_input <= params->output_index) {
        return -1;
    }

    const int8_t *raw_s = (const int8_t *)pInput[params->output_index];
    const uint8_t *raw_u = (const uint8_t *)pInput[params->output_index];

    /* float32 output: read as-is; int8/uint8: dequantize */
    float inv = 0.0f;
    double norm2 = 0.0;
    if (params->output_signed == 2) {
        const float *raw_f = (const float *)pInput[params->output_index];
        for (uint32_t i = 0; i < params->dim; i++) {
            params->vec[i] = raw_f[i];
            norm2 += (double)raw_f[i] * (double)raw_f[i];
        }
    } else {
        for (uint32_t i = 0; i < params->dim; i++) {
            float q = params->output_signed ? (float)raw_s[i] : (float)raw_u[i];
            float v = (q - params->zero_point) * params->scale;
            params->vec[i] = v;
            norm2 += (double)v * (double)v;
        }
    }

    /* L2-normalize so downstream cosine similarity is a plain dot product */
    if (params->l2_normalize) {
        float norm = (float)sqrt(norm2);
        if (norm > 1e-12f) {
            inv = 1.0f / norm;
        }
        for (uint32_t i = 0; i < params->dim; i++) {
            params->vec[i] *= inv;
        }
    }

    result->type = PP_TYPE_EMBEDDING;
    result->is_valid = 1;
    result->embed.vec = params->vec;
    result->embed.dim = params->dim;
    result->embed.normalized = params->l2_normalize;
    return 0;
}

/* Thresholds are meaningless for embeddings - accept and ignore */
static int32_t set_confidence_threshold(void *params, float threshold)
{
    (void)params; (void)threshold;
    return 0;
}

static int32_t get_confidence_threshold(void *params, float *threshold)
{
    (void)params;
    if (threshold) {
        *threshold = 0.0f;
    }
    return 0;
}

static int32_t set_nms_threshold(void *params, float threshold)
{
    (void)params; (void)threshold;
    return 0;
}

static int32_t get_nms_threshold(void *params, float *threshold)
{
    (void)params;
    if (threshold) {
        *threshold = 0.0f;
    }
    return 0;
}

static int32_t deinit(void *params)
{
    pp_face_embed_params_t *p = (pp_face_embed_params_t *)params;
    if (p) {
        if (p->vec) {
            hal_mem_free(p->vec);
        }
        hal_mem_free(p);
    }
    return 0;
}

const pp_vtable_t pp_face_embed_vtable = {
    .init = init,
    .run = run,
    .set_confidence_threshold = set_confidence_threshold,
    .get_confidence_threshold = get_confidence_threshold,
    .set_nms_threshold = set_nms_threshold,
    .get_nms_threshold = get_nms_threshold,
    .deinit = deinit,
};

const pp_entry_t pp_entry_face_embed = {
    .name = "pp_face_embed",
    .vt = &pp_face_embed_vtable,
};
