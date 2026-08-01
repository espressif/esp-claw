/* claw_hw_registry — see include/claw_hw_registry.h.
 *
 * A claim occupies one primary row + one row per sub_resource, all sharing
 * the same lease_id so release removes them atomically. The handle exposed
 * to callers is (opaque)(uintptr_t)lease_id; the actual rows live in the
 * table. Release callbacks fire outside the registry lock. */

#include "claw_hw_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "claw_hw";

#define CLAW_HW_KEY_BUF_MIN       32
#define CLAW_HW_INITIAL_CAPACITY  32

typedef struct {
    uint32_t             lease_id;
    char                *resource;
    char                *owner_tag;
    claw_hw_mode_t       mode;
    /* on_release / user_ctx live on the primary row only. */
    bool                 is_primary;
    claw_hw_release_cb_t on_release;
    void                *user_ctx;
} claw_hw_row_t;

typedef struct {
    SemaphoreHandle_t mutex;
    claw_hw_row_t   **rows;
    size_t            count;
    size_t            capacity;
    uint32_t          next_lease_id;
    bool              initialized;
} claw_hw_registry_state_t;

static claw_hw_registry_state_t s_state;
static portMUX_TYPE             s_init_spinlock = portMUX_INITIALIZER_UNLOCKED;

static const char *mode_str(claw_hw_mode_t mode)
{
    switch (mode) {
    case CLAW_HW_MODE_EXCLUSIVE:   return "EXCLUSIVE";
    case CLAW_HW_MODE_SHARED_READ: return "SHARED_READ";
    default:                       return "UNKNOWN";
    }
}

static void row_free(claw_hw_row_t *row)
{
    if (row == NULL) {
        return;
    }
    free(row->resource);
    free(row->owner_tag);
    free(row);
}

static void lock(void)
{
    xSemaphoreTakeRecursive(s_state.mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGiveRecursive(s_state.mutex);
}

static bool ensure_capacity_locked(size_t needed)
{
    if (needed <= s_state.capacity) {
        return true;
    }
    size_t new_cap = s_state.capacity ? s_state.capacity : CLAW_HW_INITIAL_CAPACITY;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    claw_hw_row_t **new_rows = realloc(s_state.rows, new_cap * sizeof(*new_rows));
    if (new_rows == NULL) {
        return false;
    }
    s_state.rows     = new_rows;
    s_state.capacity = new_cap;
    return true;
}

static int find_row_locked(const char *resource)
{
    for (size_t i = 0; i < s_state.count; ++i) {
        if (strcmp(s_state.rows[i]->resource, resource) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* SHARED_READ + SHARED_READ is allowed; any EXCLUSIVE on either side is not. */
static esp_err_t check_conflict_locked(const char *resource,
                                       claw_hw_mode_t mode,
                                       const char **out_holder)
{
    for (size_t i = 0; i < s_state.count; ++i) {
        claw_hw_row_t *row = s_state.rows[i];
        if (strcmp(row->resource, resource) != 0) {
            continue;
        }
        if (mode == CLAW_HW_MODE_EXCLUSIVE ||
            row->mode == CLAW_HW_MODE_EXCLUSIVE) {
            if (out_holder != NULL) {
                *out_holder = row->owner_tag;
            }
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}

static claw_hw_row_t *make_row(uint32_t lease_id,
                               const char *resource,
                               const char *owner_tag,
                               claw_hw_mode_t mode,
                               bool is_primary,
                               claw_hw_release_cb_t on_release,
                               void *user_ctx)
{
    claw_hw_row_t *row = calloc(1, sizeof(*row));
    if (row == NULL) {
        return NULL;
    }
    row->resource = strdup(resource);
    row->owner_tag = strdup(owner_tag);
    if (row->resource == NULL || row->owner_tag == NULL) {
        row_free(row);
        return NULL;
    }
    row->lease_id  = lease_id;
    row->mode      = mode;
    row->is_primary = is_primary;
    row->on_release = on_release;
    row->user_ctx   = user_ctx;
    return row;
}

/* Swap-with-last removal; ownership transfers to the caller. */
static claw_hw_row_t *remove_row_at_locked(size_t index)
{
    claw_hw_row_t *row = s_state.rows[index];
    size_t last = s_state.count - 1;
    if (index != last) {
        s_state.rows[index] = s_state.rows[last];
    }
    s_state.rows[last] = NULL;
    s_state.count--;
    return row;
}

esp_err_t claw_hw_registry_init(void)
{
    portENTER_CRITICAL(&s_init_spinlock);
    bool already = s_state.initialized;
    portEXIT_CRITICAL(&s_init_spinlock);
    if (already) {
        return ESP_OK;
    }

    /* Allocate outside the spinlock — FreeRTOS APIs must not run in one. */
    SemaphoreHandle_t mutex = xSemaphoreCreateRecursiveMutex();
    if (mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    claw_hw_row_t **rows = calloc(CLAW_HW_INITIAL_CAPACITY, sizeof(*rows));
    if (rows == NULL) {
        vSemaphoreDelete(mutex);
        return ESP_ERR_NO_MEM;
    }

    /* Publish under the spinlock; discard our allocations on a lost race. */
    portENTER_CRITICAL(&s_init_spinlock);
    if (!s_state.initialized) {
        s_state.mutex         = mutex;
        s_state.rows          = rows;
        s_state.capacity      = CLAW_HW_INITIAL_CAPACITY;
        s_state.count         = 0;
        s_state.next_lease_id = 1;
        s_state.initialized   = true;
        mutex = NULL;
        rows  = NULL;
    }
    portEXIT_CRITICAL(&s_init_spinlock);

    if (rows  != NULL) free(rows);
    if (mutex != NULL) vSemaphoreDelete(mutex);
    return ESP_OK;
}

/* lease_id starts at 1 so NULL is never a valid handle. */
static claw_hw_lease_handle_t handle_from_id(uint32_t id)
{
    return (claw_hw_lease_handle_t)(uintptr_t)id;
}

static uint32_t id_from_handle(claw_hw_lease_handle_t h)
{
    return (uint32_t)(uintptr_t)h;
}

esp_err_t claw_hw_claim(const claw_hw_claim_config_t *config,
                        claw_hw_lease_handle_t *out_lease)
{
    if (config == NULL || out_lease == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->resource == NULL || config->resource[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->owner_tag == NULL || config->owner_tag[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t sub_count = 0;
    if (config->sub_resources != NULL) {
        for (const char *const *p = config->sub_resources; *p != NULL; ++p) {
            if ((*p)[0] == '\0') {
                return ESP_ERR_INVALID_ARG;
            }
            sub_count++;
        }
    }

    lock();

    const char *holder = NULL;
    esp_err_t err = check_conflict_locked(config->resource, config->mode, &holder);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "claw_hw: %s already held by %s (requesting %s)",
                 config->resource,
                 holder != NULL ? holder : "?",
                 config->owner_tag);
        unlock();
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < sub_count; ++i) {
        const char *sub = config->sub_resources[i];
        holder = NULL;
        err = check_conflict_locked(sub, config->mode, &holder);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "claw_hw: %s already held by %s (requesting %s) [sub of %s]",
                     sub,
                     holder != NULL ? holder : "?",
                     config->owner_tag,
                     config->resource);
            unlock();
            return ESP_ERR_INVALID_STATE;
        }
    }

    /* Reserve capacity for main + subs so partial inserts on OOM cannot
     * strand the primary row in the table. */
    if (!ensure_capacity_locked(s_state.count + 1 + sub_count)) {
        unlock();
        return ESP_ERR_NO_MEM;
    }

    uint32_t lease_id = s_state.next_lease_id++;

    claw_hw_row_t *main_row = make_row(lease_id,
                                       config->resource,
                                       config->owner_tag,
                                       config->mode,
                                       true,
                                       config->on_release,
                                       config->user_ctx);
    if (main_row == NULL) {
        s_state.next_lease_id--;
        unlock();
        return ESP_ERR_NO_MEM;
    }

    size_t first_inserted = s_state.count;
    s_state.rows[s_state.count++] = main_row;

    for (size_t i = 0; i < sub_count; ++i) {
        claw_hw_row_t *sub_row = make_row(lease_id,
                                          config->sub_resources[i],
                                          config->owner_tag,
                                          config->mode,
                                          false,
                                          NULL,
                                          NULL);
        if (sub_row == NULL) {
            for (size_t j = s_state.count; j > first_inserted; --j) {
                claw_hw_row_t *r = s_state.rows[j - 1];
                s_state.rows[j - 1] = NULL;
                s_state.count--;
                row_free(r);
            }
            s_state.next_lease_id--;
            unlock();
            return ESP_ERR_NO_MEM;
        }
        s_state.rows[s_state.count++] = sub_row;
    }

    *out_lease = handle_from_id(lease_id);
    ESP_LOGD(TAG, "claw_hw: claim id=%u %s by %s (%s, subs=%u)",
             (unsigned)lease_id,
             config->resource,
             config->owner_tag,
             mode_str(config->mode),
             (unsigned)sub_count);
    unlock();
    return ESP_OK;
}

/* Detach all rows for lease_id and hand them back so on_release can fire
 * outside the mutex. */
static esp_err_t take_rows_by_lease_id_locked(uint32_t lease_id,
                                              claw_hw_row_t ***out_rows,
                                              size_t *out_n)
{
    size_t matches = 0;
    for (size_t i = 0; i < s_state.count; ++i) {
        if (s_state.rows[i]->lease_id == lease_id) {
            matches++;
        }
    }
    if (matches == 0) {
        *out_rows = NULL;
        *out_n    = 0;
        return ESP_ERR_NOT_FOUND;
    }

    claw_hw_row_t **taken = calloc(matches, sizeof(*taken));
    if (taken == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Iterate backwards so swap-with-last removal does not skip elements. */
    size_t idx = 0;
    for (size_t i = s_state.count; i > 0; --i) {
        size_t pos = i - 1;
        if (s_state.rows[pos]->lease_id == lease_id) {
            taken[idx++] = remove_row_at_locked(pos);
        }
    }

    *out_rows = taken;
    *out_n    = matches;
    return ESP_OK;
}

esp_err_t claw_hw_release(claw_hw_lease_handle_t lease)
{
    if (lease == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t lease_id = id_from_handle(lease);

    lock();
    claw_hw_row_t **taken = NULL;
    size_t n_taken = 0;
    esp_err_t err = take_rows_by_lease_id_locked(lease_id, &taken, &n_taken);
    unlock();

    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < n_taken; ++i) {
        claw_hw_row_t *row = taken[i];
        if (row->is_primary && row->on_release != NULL) {
            row->on_release(row->resource, row->owner_tag, row->user_ctx);
        }
        ESP_LOGD(TAG, "claw_hw: release id=%u %s by %s",
                 (unsigned)lease_id, row->resource, row->owner_tag);
        row_free(row);
    }
    free(taken);
    return ESP_OK;
}

esp_err_t claw_hw_release_by_tag(const char *owner_tag)
{
    if (owner_tag == NULL || owner_tag[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Collect distinct lease ids under the lock, then release them via the
     * standard path so callbacks fire outside the mutex. */
    lock();
    uint32_t *ids = NULL;
    size_t ids_n = 0;
    size_t ids_cap = 0;
    for (size_t i = 0; i < s_state.count; ++i) {
        claw_hw_row_t *row = s_state.rows[i];
        if (strcmp(row->owner_tag, owner_tag) != 0) {
            continue;
        }
        bool dup = false;
        for (size_t j = 0; j < ids_n; ++j) {
            if (ids[j] == row->lease_id) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        if (ids_n == ids_cap) {
            size_t new_cap = ids_cap ? ids_cap * 2 : 8;
            uint32_t *new_ids = realloc(ids, new_cap * sizeof(*new_ids));
            if (new_ids == NULL) {
                free(ids);
                unlock();
                return ESP_ERR_NO_MEM;
            }
            ids = new_ids;
            ids_cap = new_cap;
        }
        ids[ids_n++] = row->lease_id;
    }
    unlock();

    for (size_t i = 0; i < ids_n; ++i) {
        (void)claw_hw_release(handle_from_id(ids[i]));
    }
    free(ids);
    return ESP_OK;
}

esp_err_t claw_hw_query(const char *resource, const char **out_tag)
{
    if (resource == NULL || resource[0] == '\0' || out_tag == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    lock();
    int idx = find_row_locked(resource);
    if (idx < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    *out_tag = s_state.rows[idx]->owner_tag;
    unlock();
    return ESP_OK;
}

typedef struct {
    char          *resource;
    char          *owner_tag;
    claw_hw_mode_t mode;
} claw_hw_snapshot_entry_t;

static void free_snapshot(claw_hw_snapshot_entry_t *snap, size_t n)
{
    if (snap == NULL) {
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        free(snap[i].resource);
        free(snap[i].owner_tag);
    }
    free(snap);
}

esp_err_t claw_hw_foreach(claw_hw_iter_cb_t cb, void *user_ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    lock();
    size_t n = s_state.count;
    claw_hw_snapshot_entry_t *snap = NULL;
    if (n > 0) {
        snap = calloc(n, sizeof(*snap));
        if (snap == NULL) {
            unlock();
            return ESP_ERR_NO_MEM;
        }
        for (size_t i = 0; i < n; ++i) {
            snap[i].resource  = strdup(s_state.rows[i]->resource);
            snap[i].owner_tag = strdup(s_state.rows[i]->owner_tag);
            snap[i].mode      = s_state.rows[i]->mode;
            if (snap[i].resource == NULL || snap[i].owner_tag == NULL) {
                free_snapshot(snap, i + 1);
                unlock();
                return ESP_ERR_NO_MEM;
            }
        }
    }
    unlock();

    for (size_t i = 0; i < n; ++i) {
        cb(snap[i].resource, snap[i].owner_tag, snap[i].mode, user_ctx);
    }
    free_snapshot(snap, n);
    return ESP_OK;
}

const char *claw_hw_key_gpio(char *buf, size_t buflen, int pin)
{
    if (buf == NULL || buflen < CLAW_HW_KEY_BUF_MIN) {
        return NULL;
    }
    snprintf(buf, buflen, "gpio:%d", pin);
    return buf;
}

const char *claw_hw_key_i2c(char *buf, size_t buflen, int port, uint8_t addr7)
{
    if (buf == NULL || buflen < CLAW_HW_KEY_BUF_MIN) {
        return NULL;
    }
    snprintf(buf, buflen, "i2c:%d/0x%02x", port, (unsigned)addr7);
    return buf;
}

const char *claw_hw_key_spi(char *buf, size_t buflen, int host, int cs_pin)
{
    if (buf == NULL || buflen < CLAW_HW_KEY_BUF_MIN) {
        return NULL;
    }
    snprintf(buf, buflen, "spi:%d/cs%d", host, cs_pin);
    return buf;
}

const char *claw_hw_key_i2s(char *buf, size_t buflen, int port, bool tx)
{
    if (buf == NULL || buflen < CLAW_HW_KEY_BUF_MIN) {
        return NULL;
    }
    snprintf(buf, buflen, "i2s:%d/%s", port, tx ? "tx" : "rx");
    return buf;
}

const char *claw_hw_key_rmt(char *buf, size_t buflen, int channel)
{
    if (buf == NULL || buflen < CLAW_HW_KEY_BUF_MIN) {
        return NULL;
    }
    snprintf(buf, buflen, "rmt:%d", channel);
    return buf;
}

const char *claw_hw_key_adc(char *buf, size_t buflen, int unit, int channel)
{
    if (buf == NULL || buflen < CLAW_HW_KEY_BUF_MIN) {
        return NULL;
    }
    snprintf(buf, buflen, "adc:%d/ch%d", unit, channel);
    return buf;
}

const char *claw_hw_key_device(char *buf, size_t buflen, const char *name)
{
    if (buf == NULL || buflen < CLAW_HW_KEY_BUF_MIN || name == NULL) {
        return NULL;
    }
    snprintf(buf, buflen, "dev:%s", name);
    return buf;
}
