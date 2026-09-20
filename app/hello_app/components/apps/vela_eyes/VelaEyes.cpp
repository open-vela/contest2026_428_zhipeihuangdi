#include "VelaEyes.hpp"
#include "eye/eye.h"

LV_IMG_DECLARE(img_app_calculator);

VelaEyes::VelaEyes():
    ESP_Brookesia_PhoneApp("Vela Eyes", &img_app_calculator, true)
{
}

VelaEyes::~VelaEyes()
{
}

bool VelaEyes::init(void)
{
    return true;
}

bool VelaEyes::run(void)
{
    lv_area_t area = getVisualArea();
    const lv_coord_t width = area.x2 - area.x1 + 1;
    const lv_coord_t height = area.y2 - area.y1 + 1;

    root_ = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(root_, area.x1, area.y1);
    lv_obj_set_size(root_, width, height);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    eye_init(root_);
    return true;
}

bool VelaEyes::back(void)
{
    notifyCoreClosed();
    return true;
}

bool VelaEyes::close(void)
{
    eye_deinit();
    root_ = nullptr;
    return true;
}
