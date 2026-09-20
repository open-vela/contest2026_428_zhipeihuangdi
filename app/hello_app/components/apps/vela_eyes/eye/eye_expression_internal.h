#pragma once

typedef struct {
    int gaze_x;
    int gaze_y;
    int left_brow_y_offset;
    int right_brow_y_offset;
    int eye_size;
    int iris_size;
    int pupil_size;
} eye_pose_t;

eye_pose_t eye_curious_pose(void);
eye_pose_t eye_surprised_pose(void);
