#pragma once

#include "esp_brookesia.hpp"
#include "lvgl.h"

class VelaEyes: public ESP_Brookesia_PhoneApp
{
public:
    VelaEyes();
    ~VelaEyes();

    bool run(void);
    bool back(void);
    bool close(void);
    bool init(void) override;

private:
    lv_obj_t *root_ = nullptr;
};
