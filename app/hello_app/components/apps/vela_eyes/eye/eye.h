#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EYE_HAPPY, EYE_SMILE, EYE_CURIOUS, EYE_SURPRISED, EYE_FOCUS,
    EYE_SHY, EYE_SLEEP, EYE_EXCITED, EYE_WINK, EYE_SAD
} eye_state_t;

void eye_init(lv_obj_t *parent);
void eye_deinit(void);
void eye_set_state(eye_state_t state);
void eye_blink(void);
void eye_look_at(int x, int y);

#ifdef __cplusplus
}
#endif
