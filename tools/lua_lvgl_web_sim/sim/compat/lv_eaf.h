#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "lvgl.h"

lv_obj_t *lv_eaf_create(lv_obj_t *parent);
void lv_eaf_set_src(lv_obj_t *obj, const char *src);
void lv_eaf_set_src_data(lv_obj_t *obj, const void *data, size_t size);
void lv_eaf_restart(lv_obj_t *obj);
void lv_eaf_pause(lv_obj_t *obj);
void lv_eaf_resume(lv_obj_t *obj);
bool lv_eaf_is_loaded(lv_obj_t *obj);
int32_t lv_eaf_get_total_frames(lv_obj_t *obj);
int32_t lv_eaf_get_current_frame(lv_obj_t *obj);
void lv_eaf_set_loop_count(lv_obj_t *obj, int32_t count);
int32_t lv_eaf_get_loop_count(lv_obj_t *obj);
void lv_eaf_set_loop_enabled(lv_obj_t *obj, bool enabled);
bool lv_eaf_get_loop_enabled(lv_obj_t *obj);
void lv_eaf_set_frame_delay(lv_obj_t *obj, uint32_t delay_ms);
uint32_t lv_eaf_get_frame_delay(lv_obj_t *obj);
