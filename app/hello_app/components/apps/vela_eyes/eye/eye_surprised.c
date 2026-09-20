#include "eye_expression_internal.h"

eye_pose_t eye_surprised_pose(void)
{
    return (eye_pose_t) {
        .gaze_x = 0,
        .gaze_y = 0,
        .left_brow_y_offset = -22,
        .right_brow_y_offset = -22,
        .eye_size = 270,
        .iris_size = 150,
        .pupil_size = 94,
    };
}
