#include "eye_expression_internal.h"

eye_pose_t eye_curious_pose(void)
{
    return (eye_pose_t) {
        .gaze_x = 22,
        .gaze_y = -7,
        .left_brow_y_offset = -3,
        .right_brow_y_offset = -13,
        .eye_size = 230,
        .iris_size = 142,
        .pupil_size = 104,
    };
}
