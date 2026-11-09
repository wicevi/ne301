/**
 * @file face_db.c
 * @brief Local face template database (LittleFS-persisted)
 * @details Stores enrolled face templates (L2-normalized embedding vectors)
 *          as one file per entry under /faces on the internal-flash LittleFS
 *          partition, and provides cosine-similarity matching.
 *
 *          Why files (not NVS): NVS caps a single value at one 4K sector and
 *          rewrites wear the same sectors - fine for a handful of people,
 *          not for hundreds of templates (~2.1KB each at 512-dim). LittleFS
 *          gives per-entry files, power-cut-safe rename-based commits and a
 *          96MB partition. Databases created on NVS (v1 whole-db blob or v2
 *          per-entry keys) are migrated transparently on first boot and the
 *          NVS keys are dropped.
 *
 *          Thread model: all API functions take an internal mutex; safe to
 *          call from the AI node thread (matching) and the web server thread
 *          (enroll/list/delete) concurrently.
 */

#include "face_db.h"
#include "aicam_error.h"
#include "debug.h"
#include "mem.h"
#include "drtc.h"
#include "storage.h"
#include "generic_file.h"
#include "cmsis_os2.h"
#include "lfs.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>

/* ==================== Layout ==================== */

#define FACE_DB_DIR       "/faces"
#define FACE_DB_ENTRY_FMT "/faces/e%lu.fr"   /* final entry file (id decimal) */
#define FACE_DB_TMP_FMT   "/faces/e%lu.tmp"  /* staging name, renamed on commit */
#define FACE_DB_NAME_MAX_LEN 8u              /* "e4294967295.fr" worst case */

/* NVS keys of the previous layout - migration sources only */
#define FACE_DB_META_KEY   "fr_meta"
#define FACE_DB_ENTRY_FMT_NV "fr_e%lu"
#define FACE_DB_LEGACY_KEY "fr_db"

#pragma pack(push, 1)
/* One entry file: header + dim floats + CRC32 over the preceding bytes.
 * Written to the .tmp name and renamed onto the final name, so a power cut
 * can never leave a half-written .fr behind (LittleFS rename is atomic). */
typedef struct {
    uint32_t magic;      /* 'FRDB' */
    uint32_t version;    /* 3 */
    uint32_t id;
    uint32_t created_s;
    uint32_t dim;
    char name[FACE_DB_NAME_MAX];
} face_db_record_t;

/* Legacy NVS layouts (migration sources) */
typedef struct {
    uint32_t magic;
    uint32_t version;    /* 2 */
    uint32_t next_id;
    uint32_t count;
    uint32_t ids[FACE_DB_MAX_ENTRIES];
} face_db_meta_t;

typedef struct {
    uint32_t magic;
    uint32_t version;    /* 1 */
    uint32_t count;
    uint32_t next_id;
} face_db_legacy_header_t;
#pragma pack(pop)

#define FACE_DB_MAGIC 0x42445246u /* 'FRDB' little-endian */
#define FACE_DB_VERSION 3u

typedef struct {
    aicam_bool_t initialized;
    osMutexId_t mutex;
    uint32_t next_id;    /* monotonic, derived from max(id)+1 at load */
    uint32_t count;
    /* Templates kept per entry up to FACE_DB_EMBED_DIM_MAX floats each */
    struct {
        uint32_t id;
        uint32_t created_s;
        uint32_t dim;
        char name[FACE_DB_NAME_MAX];
        float *template;  /* FACE_DB_EMBED_DIM_MAX floats, heap allocated */
    } entries[FACE_DB_MAX_ENTRIES];
} face_db_ctx_t;

static face_db_ctx_t g_fdb = {0};

/* ==================== Internal Functions ==================== */

static size_t fdb_record_size(uint32_t dim)
{
    return sizeof(face_db_record_t) + sizeof(float) * dim + 4u /* CRC32 */;
}

/* Software CRC32 (self-contained: no HW dependency on this path). */
static uint32_t fdb_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static void fdb_entry_path(char *out, size_t out_size, uint32_t id, int tmp)
{
    snprintf(out, out_size, tmp ? FACE_DB_TMP_FMT : FACE_DB_ENTRY_FMT,
             (unsigned long)id);
}

static int32_t fdb_find_index(face_db_ctx_t *ctx, uint32_t id)
{
    for (uint32_t i = 0; i < ctx->count; i++) {
        if (ctx->entries[i].id == id) {
            return (int32_t)i;
        }
    }
    return -1;
}

static void fdb_free_entries(face_db_ctx_t *ctx)
{
    for (uint32_t i = 0; i < FACE_DB_MAX_ENTRIES; i++) {
        if (ctx->entries[i].template) {
            hal_mem_free(ctx->entries[i].template);
            ctx->entries[i].template = NULL;
        }
    }
    memset(ctx->entries, 0, sizeof(ctx->entries));
    ctx->count = 0;
}

static void fdb_ensure_init(void)
{
    if (g_fdb.initialized) {
        return;
    }
    face_db_init();
}

/* Persist one entry: staging file + fflush + atomic rename. */
static aicam_bool_t fdb_save_entry(face_db_ctx_t *ctx, uint32_t slot)
{
    const typeof(ctx->entries[0]) *e = &ctx->entries[slot];
    size_t len = fdb_record_size(e->dim);

    uint8_t *buf = (uint8_t *)hal_mem_alloc(len, MEM_LARGE);
    if (!buf) {
        LOG_CORE_ERROR("FaceDB: entry buffer alloc failed (%lu B)", (unsigned long)len);
        return AICAM_FALSE;
    }
    face_db_record_t rec;
    rec.magic = FACE_DB_MAGIC;
    rec.version = FACE_DB_VERSION;
    rec.id = e->id;
    rec.created_s = e->created_s;
    rec.dim = e->dim;
    memcpy(rec.name, e->name, FACE_DB_NAME_MAX);
    memcpy(buf, &rec, sizeof(rec));
    memcpy(buf + sizeof(rec), e->template, sizeof(float) * e->dim);
    uint32_t crc = fdb_crc32(buf, len - 4u);
    memcpy(buf + len - 4u, &crc, 4u);

    char final_path[32], tmp_path[32];
    fdb_entry_path(final_path, sizeof(final_path), e->id, 0);
    fdb_entry_path(tmp_path, sizeof(tmp_path), e->id, 1);

    aicam_bool_t ok = AICAM_FALSE;
    void *fd = disk_file_fopen(FS_FLASH, tmp_path, "w");
    if (fd) {
        int written = disk_file_fwrite(FS_FLASH, fd, buf, len);
        disk_file_fflush(FS_FLASH, fd);
        disk_file_fclose(FS_FLASH, fd);
        if (written == (int)len && disk_file_rename(FS_FLASH, tmp_path, final_path) == 0) {
            ok = AICAM_TRUE;
        } else {
            disk_file_remove(FS_FLASH, tmp_path);
            LOG_CORE_ERROR("FaceDB: entry %lu file commit failed", (unsigned long)e->id);
        }
    } else {
        LOG_CORE_ERROR("FaceDB: entry %lu file open failed", (unsigned long)e->id);
    }
    hal_mem_free(buf);
    return ok;
}

/* Load one entry file into RAM slot `slot`. */
static aicam_bool_t fdb_load_entry_file(face_db_ctx_t *ctx, uint32_t slot, uint32_t id)
{
    char path[32];
    fdb_entry_path(path, sizeof(path), id, 0);

    uint8_t *buf = (uint8_t *)hal_mem_alloc(fdb_record_size(FACE_DB_EMBED_DIM_MAX), MEM_LARGE);
    if (!buf) {
        return AICAM_FALSE;
    }
    aicam_bool_t ok = AICAM_FALSE;
    void *fd = disk_file_fopen(FS_FLASH, path, "r");
    if (fd) {
        int got = disk_file_fread(FS_FLASH, fd, buf, fdb_record_size(FACE_DB_EMBED_DIM_MAX));
        disk_file_fclose(FS_FLASH, fd);
        face_db_record_t rec;
        if (got >= (int)sizeof(face_db_record_t)) {
            memcpy(&rec, buf, sizeof(rec));
            if (rec.magic == FACE_DB_MAGIC && rec.version == FACE_DB_VERSION &&
                rec.id == id && rec.dim > 0 && rec.dim <= FACE_DB_EMBED_DIM_MAX &&
                got >= (int)fdb_record_size(rec.dim)) {
                size_t len = fdb_record_size(rec.dim);
                uint32_t crc_stored, crc_calc;
                memcpy(&crc_stored, buf + len - 4u, 4u);
                crc_calc = fdb_crc32(buf, len - 4u);
                if (crc_stored == crc_calc) {
                    float *tpl = hal_mem_alloc(sizeof(float) * FACE_DB_EMBED_DIM_MAX, MEM_LARGE);
                    if (tpl) {
                        memcpy(tpl, buf + sizeof(face_db_record_t), sizeof(float) * rec.dim);
                        ctx->entries[slot].id = rec.id;
                        ctx->entries[slot].created_s = rec.created_s;
                        ctx->entries[slot].dim = rec.dim;
                        memcpy(ctx->entries[slot].name, rec.name, FACE_DB_NAME_MAX);
                        ctx->entries[slot].name[FACE_DB_NAME_MAX - 1u] = '\0';
                        ctx->entries[slot].template = tpl;
                        ok = AICAM_TRUE;
                    }
                } else {
                    LOG_CORE_WARN("FaceDB: entry %lu CRC mismatch - skipped", (unsigned long)id);
                }
            }
        }
    }
    hal_mem_free(buf);
    return ok;
}

/* readdir fills an lfs_info (type + size + name[256], 264B) - the buffer
 * must be at least that big (same union trick as the CLI's ls). */
typedef union {
    struct lfs_info info;
    char _pad[300];
} fdb_dir_entry_t;

/* Parse "e<id>.fr" -> id; -1 when the name is not an entry file. */
static long fdb_parse_entry_name(const char *name)
{
    size_t n = strlen(name);
    /* "e1.fr" is the shortest valid name: 'e' + 1 digit + ".fr" = 5. */
    if (n < 5 || name[0] != 'e' || strcmp(name + n - 3, ".fr") != 0) {
        return -1;
    }
    char digits[12];
    size_t dl = n - 4u; /* between 'e' and ".fr" */
    if (dl == 0 || dl >= sizeof(digits)) {
        return -1;
    }
    memcpy(digits, name + 1, dl);
    digits[dl] = '\0';
    char *end = NULL;
    long id = strtol(digits, &end, 10);
    if (end == digits || *end != '\0' || id <= 0) {
        return -1;
    }
    return id;
}

/* One-shot migration from the NVS layouts (v2 per-entry keys, v1 whole-db
 * blob) into files. Runs only when /faces has no entry files. */
static void fdb_migrate_from_nvs(face_db_ctx_t *ctx)
{
    face_db_meta_t *meta = (face_db_meta_t *)hal_mem_alloc(sizeof(face_db_meta_t), MEM_LARGE);
    uint8_t *buf = (uint8_t *)hal_mem_alloc(sizeof(face_db_record_t) +
                                            sizeof(float) * FACE_DB_EMBED_DIM_MAX + 4u, MEM_LARGE);
    if (!meta || !buf) {
        hal_mem_free(meta);
        hal_mem_free(buf);
        return;
    }

    /* v2: per-entry keys referenced by the meta directory */
    uint32_t migrated = 0;
    int ret = storage_nvs_read(NVS_USER, FACE_DB_META_KEY, meta, sizeof(*meta));
    if (ret >= (int)sizeof(face_db_meta_t) && meta->magic == FACE_DB_MAGIC &&
        meta->version == 2u && meta->count <= FACE_DB_MAX_ENTRIES) {
        for (uint32_t i = 0; i < meta->count && ctx->count < FACE_DB_MAX_ENTRIES; i++) {
            uint32_t id = meta->ids[i];
            char key[16];
            snprintf(key, sizeof(key), FACE_DB_ENTRY_FMT_NV, (unsigned long)id);
            int got = storage_nvs_read(NVS_USER, key, buf,
                                       sizeof(face_db_record_t) +
                                       sizeof(float) * FACE_DB_EMBED_DIM_MAX);
            if (got < (int)(sizeof(face_db_record_t) - 4u)) {
                continue; /* NVS v2 records had no CRC: header + floats */
            }
            face_db_record_t rec;
            memcpy(&rec, buf, sizeof(rec));
            if (rec.id != id || rec.dim == 0 || rec.dim > FACE_DB_EMBED_DIM_MAX) {
                continue;
            }
            uint32_t slot = ctx->count;
            ctx->entries[slot].id = rec.id;
            ctx->entries[slot].created_s = rec.created_s;
            ctx->entries[slot].dim = rec.dim;
            memcpy(ctx->entries[slot].name, rec.name, FACE_DB_NAME_MAX);
            float *tpl = hal_mem_alloc(sizeof(float) * FACE_DB_EMBED_DIM_MAX, MEM_LARGE);
            if (!tpl) {
                ctx->entries[slot].id = 0;
                break;
            }
            memcpy(tpl, buf + sizeof(face_db_record_t), sizeof(float) * rec.dim);
            ctx->entries[slot].template = tpl;
            ctx->count++;
            if (id >= ctx->next_id) {
                ctx->next_id = id + 1u;
            }
            migrated++;
        }
    }

    if (migrated == 0) {
        /* v1: whole-db blob */
        face_db_legacy_header_t legacy_head;
        ret = storage_nvs_read(NVS_USER, FACE_DB_LEGACY_KEY, &legacy_head, sizeof(legacy_head));
        if (ret >= (int)sizeof(legacy_head) && legacy_head.magic == FACE_DB_MAGIC &&
            legacy_head.version == 1u && legacy_head.count <= FACE_DB_MAX_ENTRIES) {
            size_t worst = sizeof(face_db_legacy_header_t) +
                           (sizeof(face_db_record_t) - 4u +
                            sizeof(float) * FACE_DB_EMBED_DIM_MAX) * legacy_head.count;
            uint8_t *blob = (uint8_t *)hal_mem_alloc(worst, MEM_LARGE);
            if (blob) {
                int got = storage_nvs_read(NVS_USER, FACE_DB_LEGACY_KEY, blob, worst);
                if (got > (int)sizeof(face_db_legacy_header_t)) {
                    const uint8_t *cursor = blob + sizeof(face_db_legacy_header_t);
                    const uint8_t *end = blob + got;
                    for (uint32_t i = 0; i < legacy_head.count && ctx->count < FACE_DB_MAX_ENTRIES; i++) {
                        if (cursor + sizeof(face_db_record_t) - 4u > end) {
                            break;
                        }
                        face_db_record_t rec;
                        memcpy(&rec, cursor, sizeof(rec));
                        size_t reclen = sizeof(rec) - 4u + sizeof(float) * rec.dim;
                        if (rec.dim == 0 || rec.dim > FACE_DB_EMBED_DIM_MAX ||
                            cursor + reclen > end) {
                            break;
                        }
                        uint32_t slot = ctx->count;
                        ctx->entries[slot].id = rec.id;
                        ctx->entries[slot].created_s = rec.created_s;
                        ctx->entries[slot].dim = rec.dim;
                        memcpy(ctx->entries[slot].name, rec.name, FACE_DB_NAME_MAX);
                        float *tpl = hal_mem_alloc(sizeof(float) * FACE_DB_EMBED_DIM_MAX, MEM_LARGE);
                        if (!tpl) {
                            ctx->entries[slot].id = 0;
                            break;
                        }
                        memcpy(tpl, cursor + sizeof(rec), sizeof(float) * rec.dim);
                        ctx->entries[slot].template = tpl;
                        ctx->count++;
                        if (rec.id >= ctx->next_id) {
                            ctx->next_id = rec.id + 1u;
                        }
                        migrated++;
                        cursor += reclen;
                    }
                    if (legacy_head.next_id > ctx->next_id) {
                        ctx->next_id = legacy_head.next_id;
                    }
                }
                hal_mem_free(blob);
            }
        }
    }

    hal_mem_free(meta);
    hal_mem_free(buf);

    if (migrated > 0) {
        /* Commit every migrated entry as a file, then drop the NVS keys. */
        for (uint32_t i = 0; i < ctx->count; i++) {
            if (!fdb_save_entry(ctx, i)) {
                LOG_CORE_WARN("FaceDB: migration could not persist entry %lu",
                              (unsigned long)ctx->entries[i].id);
            }
        }
        for (uint32_t i = 0; i < ctx->count; i++) {
            char key[16];
            snprintf(key, sizeof(key), FACE_DB_ENTRY_FMT_NV, (unsigned long)ctx->entries[i].id);
            storage_nvs_delete(NVS_USER, key);
        }
        storage_nvs_delete(NVS_USER, FACE_DB_META_KEY);
        storage_nvs_delete(NVS_USER, FACE_DB_LEGACY_KEY);
        LOG_CORE_INFO("FaceDB: migrated %lu entries from NVS to /faces",
                      (unsigned long)migrated);
    }
}

/* ==================== API Implementation ==================== */

aicam_result_t face_db_init(void)
{
    if (g_fdb.initialized) {
        return AICAM_OK;
    }

    g_fdb.mutex = osMutexNew(NULL);
    if (!g_fdb.mutex) {
        LOG_CORE_ERROR("FaceDB: mutex creation failed");
        return AICAM_ERROR;
    }

    g_fdb.next_id = 1;
    g_fdb.count = 0;

    disk_file_mkdir(FS_FLASH, FACE_DB_DIR); /* EEXIST is fine */

    void *dd = disk_file_opendir(FS_FLASH, FACE_DB_DIR);
    if (!dd) {
        /* FS not up yet (registration order race on some boots): DO NOT
         * lock the empty database - leave uninitialized so the next lazy
         * call rescans. Locking here made enrolled faces "vanish" until the
         * next reboot even though the files were intact. */
        LOG_CORE_WARN("FaceDB: /faces not readable yet - deferring load");
        return AICAM_OK;
    }
    {
        fdb_dir_entry_t e;
        while (disk_file_readdir(FS_FLASH, dd, (char *)&e) > 0) {
            if (g_fdb.count >= FACE_DB_MAX_ENTRIES) {
                break;
            }
            const char *fname = e.info.name;
            if (fname == NULL || fname[0] == '\0') {
                continue;
            }
            long id = fdb_parse_entry_name(fname);
            if (id < 0) {
                continue;
            }
            if (fdb_load_entry_file(&g_fdb, g_fdb.count, (uint32_t)id)) {
                g_fdb.count++;
                if ((uint32_t)id >= g_fdb.next_id) {
                    g_fdb.next_id = (uint32_t)id + 1u;
                }
            } else {
                /* Load failed: KEEP the file (never delete user-enrolled data
                 * from a boot-time read hiccup); it just stays invisible this
                 * boot. A future boot (or a fixed read path) picks it up. */
                LOG_CORE_WARN("FaceDB: entry %lu not loaded this boot (kept on disk)",
                              (unsigned long)id);
            }
        }
        disk_file_closedir(FS_FLASH, dd);
    }

    if (g_fdb.count == 0) {
        fdb_migrate_from_nvs(&g_fdb);
    }

    g_fdb.initialized = AICAM_TRUE;
    LOG_CORE_INFO("FaceDB: %lu entries loaded from %s (next_id=%lu)",
                  (unsigned long)g_fdb.count, FACE_DB_DIR, (unsigned long)g_fdb.next_id);
    return AICAM_OK;
}

aicam_result_t face_db_list(face_db_list_t *out)
{
    if (!out) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    fdb_ensure_init();

    if (osMutexAcquire(g_fdb.mutex, 200) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    out->count = g_fdb.count;
    out->entries = NULL;
    if (g_fdb.count > 0) {
        out->entries = (face_db_summary_t *)hal_mem_alloc(
            sizeof(face_db_summary_t) * g_fdb.count, MEM_LARGE);
        if (out->entries) {
            for (uint32_t i = 0; i < g_fdb.count; i++) {
                out->entries[i].id = g_fdb.entries[i].id;
                out->entries[i].dim = g_fdb.entries[i].dim;
                out->entries[i].created_s = g_fdb.entries[i].created_s;
                memcpy(out->entries[i].name, g_fdb.entries[i].name, FACE_DB_NAME_MAX);
            }
        } else {
            out->count = 0;
            osMutexRelease(g_fdb.mutex);
            return AICAM_ERROR_NO_MEMORY;
        }
    }
    osMutexRelease(g_fdb.mutex);
    return AICAM_OK;
}

void face_db_list_free(face_db_list_t *list)
{
    if (list && list->entries) {
        hal_mem_free(list->entries);
        list->entries = NULL;
        list->count = 0;
    }
}

uint32_t face_db_count(void)
{
    fdb_ensure_init();
    return g_fdb.count;
}

aicam_result_t face_db_add(const char *name, uint32_t dim, const float *template,
                           uint32_t *id_out)
{
    if (!name || !template || dim == 0 || dim > FACE_DB_EMBED_DIM_MAX) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    fdb_ensure_init();

    if (osMutexAcquire(g_fdb.mutex, 500) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }

    if (g_fdb.count >= FACE_DB_MAX_ENTRIES) {
        osMutexRelease(g_fdb.mutex);
        return AICAM_ERROR_NO_MEMORY;
    }

    float *tpl = (float *)hal_mem_alloc(sizeof(float) * FACE_DB_EMBED_DIM_MAX, MEM_LARGE);
    if (!tpl) {
        osMutexRelease(g_fdb.mutex);
        return AICAM_ERROR_NO_MEMORY;
    }
    memcpy(tpl, template, sizeof(float) * dim);

    uint32_t slot = g_fdb.count;
    g_fdb.entries[slot].id = g_fdb.next_id;
    g_fdb.entries[slot].created_s = (uint32_t)rtc_get_timeStamp();
    g_fdb.entries[slot].dim = dim;
    memset(g_fdb.entries[slot].name, 0, FACE_DB_NAME_MAX);
    strncpy(g_fdb.entries[slot].name, name, FACE_DB_NAME_MAX - 1u);
    g_fdb.entries[slot].template = tpl;

    /* Atomic file commit: on failure nothing persistent changed. */
    if (!fdb_save_entry(&g_fdb, slot)) {
        hal_mem_free(tpl);
        g_fdb.entries[slot].template = NULL;
        osMutexRelease(g_fdb.mutex);
        return AICAM_ERROR;
    }
    g_fdb.count++;
    g_fdb.next_id++;

    uint32_t new_id = g_fdb.entries[slot].id;
    osMutexRelease(g_fdb.mutex);

    if (id_out) {
        *id_out = new_id;
    }
    LOG_CORE_INFO("FaceDB: enrolled '%s' as id %lu (%lu total)",
                  name, (unsigned long)new_id, (unsigned long)face_db_count());
    return AICAM_OK;
}

aicam_result_t face_db_rename(uint32_t id, const char *name)
{
    if (!name) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    fdb_ensure_init();

    if (osMutexAcquire(g_fdb.mutex, 500) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }

    int32_t idx = fdb_find_index(&g_fdb, id);
    if (idx < 0) {
        osMutexRelease(g_fdb.mutex);
        return AICAM_ERROR_NOT_FOUND;
    }
    memset(g_fdb.entries[idx].name, 0, FACE_DB_NAME_MAX);
    strncpy(g_fdb.entries[idx].name, name, FACE_DB_NAME_MAX - 1u);

    aicam_bool_t saved = fdb_save_entry(&g_fdb, (uint32_t)idx);
    osMutexRelease(g_fdb.mutex);
    return saved ? AICAM_OK : AICAM_ERROR;
}

aicam_result_t face_db_delete(uint32_t id)
{
    fdb_ensure_init();

    if (osMutexAcquire(g_fdb.mutex, 500) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }

    int32_t idx = fdb_find_index(&g_fdb, id);
    if (idx < 0) {
        osMutexRelease(g_fdb.mutex);
        return AICAM_ERROR_NOT_FOUND;
    }

    hal_mem_free(g_fdb.entries[idx].template);
    /* Compact: shift the tail down */
    for (uint32_t i = (uint32_t)idx; i + 1 < g_fdb.count; i++) {
        g_fdb.entries[i] = g_fdb.entries[i + 1];
    }
    g_fdb.count--;
    memset(&g_fdb.entries[g_fdb.count], 0, sizeof(g_fdb.entries[0]));

    char path[32];
    fdb_entry_path(path, sizeof(path), id, 0);
    disk_file_remove(FS_FLASH, path);
    osMutexRelease(g_fdb.mutex);
    return AICAM_OK;
}

aicam_result_t face_db_clear(void)
{
    fdb_ensure_init();

    if (osMutexAcquire(g_fdb.mutex, 500) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    for (uint32_t i = 0; i < g_fdb.count; i++) {
        char path[32];
        fdb_entry_path(path, sizeof(path), g_fdb.entries[i].id, 0);
        disk_file_remove(FS_FLASH, path);
    }
    fdb_free_entries(&g_fdb);
    osMutexRelease(g_fdb.mutex);
    return AICAM_OK;
}

aicam_result_t face_db_match(uint32_t dim, const float *vec, float threshold,
                             face_db_match_t *out)
{
    if (!vec || !out) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    fdb_ensure_init();
    memset(out, 0, sizeof(*out));

    if (osMutexAcquire(g_fdb.mutex, 100) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }

    float best = -1.0f;
    float second = -1.0f;
    int32_t best_idx = -1;
    for (uint32_t i = 0; i < g_fdb.count; i++) {
        if (g_fdb.entries[i].dim != dim) {
            continue; /* stale template from another embedding model */
        }
        float dot = 0.0f;
        for (uint32_t k = 0; k < dim; k++) {
            dot += g_fdb.entries[i].template[k] * vec[k];
        }
        if (dot > best) {
            second = best;
            best = dot;
            best_idx = (int32_t)i;
        } else if (dot > second) {
            second = dot;
        }
    }
    uint32_t best_id = (best_idx >= 0) ? g_fdb.entries[best_idx].id : 0;
    char best_name[FACE_DB_NAME_MAX];
    if (best_idx >= 0) {
        memcpy(best_name, g_fdb.entries[best_idx].name, FACE_DB_NAME_MAX);
    } else {
        best_name[0] = '\0';
    }
    osMutexRelease(g_fdb.mutex);

    if (best_idx >= 0) {
        out->matched = (best >= threshold) ? AICAM_TRUE : AICAM_FALSE;
        /* Ambiguity guard (the "gap heuristic" the second_similarity field
         * was designed for): when the best and runner-up templates are
         * nearly tied, the probe sits BETWEEN two identities - guessing the
         * marginally higher one labels everyone as whichever template
         * happens to drift closest (a mixed-pose average does exactly that).
         * Report unmatched and let the caller show unknown. */
        if (out->matched && second > 0.0f && (best - second) < 0.06f) {
            out->matched = AICAM_FALSE;
        }
        out->id = best_id;
        strncpy(out->name, best_name, FACE_DB_NAME_MAX - 1u);
        out->similarity = best;
        out->second_similarity = (second < 0.0f) ? 0.0f : second;
    }
    return AICAM_OK;
}

void face_db_average_templates(uint32_t dim, const float *samples,
                               uint32_t sample_count, float *out)
{
    if (!samples || !out || dim == 0 || sample_count == 0) {
        return;
    }

    memset(out, 0, sizeof(float) * dim);
    for (uint32_t s = 0; s < sample_count; s++) {
        const float *src = samples + (size_t)s * dim;
        for (uint32_t k = 0; k < dim; k++) {
            out[k] += src[k];
        }
    }

    /* Normalize the mean so it stays a unit vector (mean of unit vectors is
     * not unit itself) */
    double norm2 = 0.0;
    for (uint32_t k = 0; k < dim; k++) {
        norm2 += (double)out[k] * (double)out[k];
    }
    float norm = (float)sqrt(norm2);
    if (norm > 1e-12f) {
        float inv = 1.0f / norm;
        for (uint32_t k = 0; k < dim; k++) {
            out[k] *= inv;
        }
    }
}
