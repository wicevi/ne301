/**
 * @file face_db.h
 * @brief Local face template database (LittleFS-persisted)
 * @details Stores enrolled face templates (L2-normalized embedding vectors)
 *          as one file per entry (/faces/e<id>.fr) on the internal-flash
 *          LittleFS partition and provides cosine-similarity matching.
 *          Commits are rename-atomic with a per-file CRC; NVS-era databases
 *          migrate transparently on first boot.
 *
 *          Thread model: all API functions take an internal mutex; safe to
 *          call from the AI node thread (matching) and the web server thread
 *          (enroll/list/delete) concurrently.
 */

#ifndef FACE_DB_H
#define FACE_DB_H

#include "aicam_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== Limits ==================== */

/* Template dimensionality cap: MobileFaceNet-class networks are 128-512d */
#define FACE_DB_EMBED_DIM_MAX 512u
/* Max entries kept in RAM. Capacity is bounded by PSRAM (~2.1KB per 512-d
 * template, ~1MB at the cap) and the 96MB LittleFS partition - NOT by any
 * NVS value-size limit (the database no longer uses NVS). */
#define FACE_DB_MAX_ENTRIES 512u
#define FACE_DB_NAME_MAX 32u

/* ==================== Types ==================== */

typedef struct {
    uint32_t id;                                   /* unique, monotonic */
    char name[FACE_DB_NAME_MAX];                   /* display name (UTF-8) */
    uint32_t dim;                                  /* template dimensionality */
    uint32_t created_s;                            /* unix seconds at enrollment */
    float template[FACE_DB_EMBED_DIM_MAX];         /* L2-normalized */
} face_db_entry_t;

/* Lightweight listing (no templates) */
typedef struct {
    uint32_t id;
    char name[FACE_DB_NAME_MAX];
    uint32_t dim;
    uint32_t created_s;
} face_db_summary_t;

/* entries are HEAP allocated by face_db_list() (hundreds of entries do
 * not fit the web-handler stack); release with face_db_list_free(). */
typedef struct {
    uint32_t count;
    face_db_summary_t *entries;
} face_db_list_t;

/* Match verdict */
typedef struct {
    aicam_bool_t matched;        /* best similarity >= threshold */
    uint32_t id;                 /* best entry id (0 when db empty) */
    char name[FACE_DB_NAME_MAX]; /* best entry name */
    float similarity;            /* cosine similarity of the best entry [0..1] */
    float second_similarity;     /* runner-up similarity (gap heuristics) */
} face_db_match_t;

/* ==================== API Functions ==================== */

/**
 * @brief Load the database from the filesystem (idempotent, lazy)
 * @details An absent/corrupt database starts empty; NVS-era databases are
 *          migrated into files on first load.
 */
aicam_result_t face_db_init(void);

/* Snapshot of all entries (heap-allocated; free with face_db_list_free) */
aicam_result_t face_db_list(face_db_list_t *out);

/* Release a face_db_list() snapshot */
void face_db_list_free(face_db_list_t *list);

uint32_t face_db_count(void);

/**
 * @brief Enroll a template (averaged/normalized by the caller or here)
 * @param name Display name (truncated to FACE_DB_NAME_MAX-1)
 * @param dim Template dimensionality (<= FACE_DB_EMBED_DIM_MAX)
 * @param template L2-normalized template (copied)
 * @param id_out Optional: new entry id
 * @return AICAM_OK, or AICAM_ERROR_NO_MEMORY when the database is full
 *         (entry slots or the NVS blob size cap)
 */
aicam_result_t face_db_add(const char *name, uint32_t dim, const float *template,
                           uint32_t *id_out);

/**
 * @brief Rename an entry by id
 */
aicam_result_t face_db_rename(uint32_t id, const char *name);

/**
 * @brief Delete one entry by id
 */
aicam_result_t face_db_delete(uint32_t id);

/**
 * @brief Delete every entry
 */
aicam_result_t face_db_clear(void);

/**
 * @brief Match a normalized probe vector against the database
 * @param dim Probe dimensionality
 * @param vec L2-normalized probe (cosine similarity == dot product)
 * @param threshold Min similarity to report as matched (0..1)
 * @param out Verdict (always filled; similarity 0 when db empty / dim mismatch)
 * @note  Entries whose dim != probe dim are skipped (model B was re-flashed
 *        with another output size -> stale templates cannot match).
 */
aicam_result_t face_db_match(uint32_t dim, const float *vec, float threshold,
                             face_db_match_t *out);

/**
 * @brief Average + L2-normalize sample vectors into one template
 * @param dim Dimensionality
 * @param samples Concatenated sample vectors (sample_count * dim floats)
 * @param sample_count Number of samples
 * @param out Normalized average template (dim floats)
 */
void face_db_average_templates(uint32_t dim, const float *samples,
                               uint32_t sample_count, float *out);

#ifdef __cplusplus
}
#endif

#endif /* FACE_DB_H */
