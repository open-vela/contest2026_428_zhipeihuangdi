#include "eye.h"
#include "eye_expression_internal.h"
#include <stdbool.h>

static lv_obj_t *root, *left_eye, *right_eye, *left_iris, *right_iris;
static lv_obj_t *left_pupil, *right_pupil, *left_glint, *right_glint;
static lv_obj_t *left_glint_small, *right_glint_small;
static lv_obj_t *eyebrow_lines[6];
static const int left_brow_base_x[3] = {236, 264, 292};
static const int left_brow_base_y[3] = {132, 124, 116};
static const int right_brow_base_x[3] = {720, 748, 776};
static const int right_brow_base_y[3] = {124, 132, 140};
static eye_state_t current_state = EYE_HAPPY;
static int gaze_x, gaze_y;
static int requested_look_x, requested_look_y;
static int current_iris_size = 142;
static int current_pupil_size = 104;
static lv_timer_t *blink_timer_handle;
static bool blink_closed;

static void style_round(lv_obj_t *obj, lv_coord_t w, lv_coord_t h, lv_color_t color)
{
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void animate_axis(lv_obj_t *obj, bool x_axis, lv_coord_t target)
{
    lv_anim_exec_xcb_t exec_cb = x_axis ? (lv_anim_exec_xcb_t)lv_obj_set_x
                                        : (lv_anim_exec_xcb_t)lv_obj_set_y;
    lv_coord_t current = x_axis ? lv_obj_get_x(obj) : lv_obj_get_y(obj);
    if (current == target) return;

    /* Face tracking is updated frequently. Replace the previous movement
     * instead of accumulating hundreds of overlapping LVGL animations. */
#if LVGL_VERSION_MAJOR >= 9
    lv_anim_delete(obj, exec_cb);
#else
    lv_anim_del(obj, exec_cb);
#endif

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, current, target);
#if LVGL_VERSION_MAJOR >= 9
    lv_anim_set_duration(&anim, 180);
#else
    lv_anim_set_time(&anim, 180);
#endif
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, exec_cb);
    lv_anim_start(&anim);
}

static void move_part(lv_obj_t *obj, lv_coord_t x, lv_coord_t y)
{
    animate_axis(obj, true, x);
    animate_axis(obj, false, y);
}

static void place_pupils(void)
{
    const int left_center_x = 335 + gaze_x;
    const int right_center_x = 689 + gaze_x;
    const int center_y = 285 + gaze_y;
    move_part(left_iris, left_center_x - current_iris_size / 2, center_y - current_iris_size / 2);
    move_part(right_iris, right_center_x - current_iris_size / 2, center_y - current_iris_size / 2);
    move_part(left_pupil, left_center_x - current_pupil_size / 2, center_y - current_pupil_size / 2);
    move_part(right_pupil, right_center_x - current_pupil_size / 2, center_y - current_pupil_size / 2);
    move_part(left_glint, left_center_x - current_pupil_size / 4, center_y - current_pupil_size / 3);
    move_part(right_glint, right_center_x - current_pupil_size / 4, center_y - current_pupil_size / 3);
    move_part(left_glint_small, left_center_x + current_pupil_size / 3, center_y + current_pupil_size / 5);
    move_part(right_glint_small, right_center_x + current_pupil_size / 3, center_y + current_pupil_size / 5);
}

static void set_eye_parts_hidden(bool hidden)
{
    lv_obj_t *parts[] = { left_eye, right_eye, left_iris, right_iris, left_pupil,
                          right_pupil, left_glint, right_glint, left_glint_small,
                          right_glint_small };
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
        if (hidden) lv_obj_add_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_eyebrow_offsets(int left_y, int right_y)
{
    for (int i = 0; i < 3; ++i) {
        lv_obj_set_pos(eyebrow_lines[i], left_brow_base_x[i], left_brow_base_y[i] + left_y);
        lv_obj_set_pos(eyebrow_lines[i + 3], right_brow_base_x[i], right_brow_base_y[i] + right_y);
    }
}

static void set_eyebrows_hidden(bool hidden)
{
    for (int i = 0; i < 6; ++i) {
        if (hidden) lv_obj_add_flag(eyebrow_lines[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(eyebrow_lines[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void blink_timer(lv_timer_t *timer)
{
    if (current_state == EYE_SLEEP) return;
    blink_closed = !blink_closed;
    set_eye_parts_hidden(blink_closed);
    if (timer) lv_timer_set_period(timer, blink_closed ? 170 : 3200);
}

void eye_init(lv_obj_t *parent)
{
    eye_deinit();
    root = parent;
    blink_closed = false;
    lv_obj_set_style_bg_color(root, lv_color_make(48, 55, 68), 0);

    left_eye = lv_obj_create(root);
    right_eye = lv_obj_create(root);
    style_round(left_eye, 230, 230, lv_color_make(232, 249, 255));
    style_round(right_eye, 230, 230, lv_color_make(232, 249, 255));
    lv_obj_set_pos(left_eye, 220, 185);
    lv_obj_set_pos(right_eye, 574, 185);

    left_iris = lv_obj_create(root);
    right_iris = lv_obj_create(root);
    style_round(left_iris, 142, 142, lv_color_make(42, 92, 118));
    style_round(right_iris, 142, 142, lv_color_make(42, 92, 118));

    left_pupil = lv_obj_create(root);
    right_pupil = lv_obj_create(root);
    style_round(left_pupil, 104, 104, lv_color_make(8, 20, 32));
    style_round(right_pupil, 104, 104, lv_color_make(8, 20, 32));

    left_glint = lv_obj_create(root);
    right_glint = lv_obj_create(root);
    style_round(left_glint, 28, 34, lv_color_white());
    style_round(right_glint, 28, 34, lv_color_white());

    left_glint_small = lv_obj_create(root);
    right_glint_small = lv_obj_create(root);
    style_round(left_glint_small, 12, 12, lv_color_white());
    style_round(right_glint_small, 12, 12, lv_color_white());

    /* Restore the original light, outward-fanning eyebrow accents. */
    for (int i = 0; i < 3; ++i) {
        eyebrow_lines[i] = lv_obj_create(root);
        eyebrow_lines[i + 3] = lv_obj_create(root);
        style_round(eyebrow_lines[i], 11, 30, lv_color_make(145, 232, 248));
        style_round(eyebrow_lines[i + 3], 11, 30, lv_color_make(145, 232, 248));

        lv_obj_set_pos(eyebrow_lines[i], left_brow_base_x[i], left_brow_base_y[i]);
        lv_obj_set_pos(eyebrow_lines[i + 3], right_brow_base_x[i], right_brow_base_y[i]);

        lv_obj_set_style_transform_angle(
            eyebrow_lines[i], (i == 0) ? 330 : (i == 1 ? 0 : 25), 0);
        lv_obj_set_style_transform_angle(
            eyebrow_lines[i + 3], (i == 0) ? 335 : (i == 1 ? 0 : 30), 0);
    }

    place_pupils();
    blink_timer_handle = lv_timer_create(blink_timer, 3200, NULL);
    eye_set_state(current_state);
}

void eye_deinit(void)
{
    if (blink_timer_handle) {
#if LVGL_VERSION_MAJOR >= 9
        lv_timer_delete(blink_timer_handle);
#else
        lv_timer_del(blink_timer_handle);
#endif
        blink_timer_handle = NULL;
    }
    root = NULL;
    left_eye = right_eye = NULL;
    left_iris = right_iris = NULL;
    left_pupil = right_pupil = NULL;
    left_glint = right_glint = NULL;
    left_glint_small = right_glint_small = NULL;
    for (int i = 0; i < 6; ++i) eyebrow_lines[i] = NULL;
}

void eye_set_state(eye_state_t state)
{
    current_state = state;
    /* The emotion state is persistent even while the Vela Eyes application
     * is closed. AI, voice and sensor modules can therefore select the next
     * expression safely; eye_init() applies it when the face is opened. */
    if (!root) return;
    gaze_x = 0;
    gaze_y = 0;
    requested_look_x = 0;
    requested_look_y = 0;
    int left_eye_w = 230, left_eye_h = 230;
    int right_eye_w = 230, right_eye_h = 230;
    current_iris_size = 142;
    current_pupil_size = 104;
    set_eyebrow_offsets(0, 0);
    set_eyebrows_hidden(false);

    switch (state) {
    case EYE_HAPPY:
        break;

    case EYE_SMILE:
        left_eye_h = 200; right_eye_h = 200;
        gaze_y = 5;
        set_eyebrow_offsets(-2, -2);
        break;

    case EYE_CURIOUS: {
        const eye_pose_t pose = eye_curious_pose();
        gaze_x = pose.gaze_x;
        gaze_y = pose.gaze_y;
        current_iris_size = pose.iris_size;
        current_pupil_size = pose.pupil_size;
        set_eyebrow_offsets(pose.left_brow_y_offset, pose.right_brow_y_offset);
        break;
    }

    case EYE_SURPRISED:
        left_eye_w = 280; left_eye_h = 280;
        right_eye_w = 280; right_eye_h = 280;
        current_iris_size = 175;
        current_pupil_size = 130;
        gaze_y = -8;
        set_eyebrow_offsets(-25, -25);
        break;

    case EYE_FOCUS:
        left_eye_w = 215; left_eye_h = 215;
        right_eye_w = 165; right_eye_h = 165;
        current_iris_size = 108;
        current_pupil_size = 68;
        gaze_y = 4;
        set_eyebrow_offsets(5, 12);
        break;

    case EYE_SHY:
        left_eye_w = 190; left_eye_h = 190;
        right_eye_w = 190; right_eye_h = 190;
        current_iris_size = 110;
        current_pupil_size = 75;
        gaze_x = -22;
        gaze_y = 18;
        set_eyebrow_offsets(-5, 4);
        break;

    case EYE_SLEEP:
        left_eye_w = 150; left_eye_h = 80;
        right_eye_w = 150; right_eye_h = 80;
        current_iris_size = 50;
        current_pupil_size = 28;
        gaze_y = 18;
        set_eyebrow_offsets(3, 3);
        break;

    case EYE_EXCITED:
        left_eye_w = 260; left_eye_h = 260;
        right_eye_w = 260; right_eye_h = 260;
        current_iris_size = 160;
        current_pupil_size = 120;
        set_eyebrow_offsets(-18, -18);
        break;

    case EYE_WINK:
        lv_obj_add_flag(left_eye, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(left_iris, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(left_pupil, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(left_glint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(left_glint_small, LV_OBJ_FLAG_HIDDEN);
        break;

    case EYE_SAD:
        left_eye_w = 185; left_eye_h = 185;
        right_eye_w = 185; right_eye_h = 185;
        current_iris_size = 105;
        current_pupil_size = 70;
        gaze_y = 22;
        set_eyebrow_offsets(6, 10);
        break;

    default:
        break;
    }

    lv_obj_set_size(left_eye, left_eye_w, left_eye_h);
    lv_obj_set_size(right_eye, right_eye_w, right_eye_h);
    lv_obj_set_pos(left_eye, 335 - left_eye_w / 2, 300 - left_eye_h / 2);
    lv_obj_set_pos(right_eye, 689 - right_eye_w / 2, 300 - right_eye_h / 2);
    lv_obj_set_size(left_iris, current_iris_size, current_iris_size);
    lv_obj_set_size(right_iris, current_iris_size, current_iris_size);
    lv_obj_set_size(left_pupil, current_pupil_size, current_pupil_size);
    lv_obj_set_size(right_pupil, current_pupil_size, current_pupil_size);
    set_eye_parts_hidden(false);
    place_pupils();
}

void eye_blink(void) { blink_timer(NULL); }

void eye_look_at(int x, int y)
{
    if (!root) return;
    if (x < -100) x = -100;
    if (x > 100) x = 100;
    if (y < -100) y = -100;
    if (y > 100) y = 100;
    /* Ignore tiny detector jitter, but make real face movement visible. */
    if (x > -4 && x < 4) x = 0;
    if (y > -4 && y < 4) y = 0;
    if (x == requested_look_x && y == requested_look_y) return;

    requested_look_x = x;
    requested_look_y = y;
    gaze_x = x * 44 / 100;
    gaze_y = y * 34 / 100;
    place_pupils();
}
