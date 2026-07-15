#ifndef CAMERA_H
#define CAMERA_H

#include "HandmadeMath.h"
#include "common.h"

typedef enum 
{
    CAMERA_MOVEMENT_FORWARD,
    CAMERA_MOVEMENT_BACKWARD,
    CAMERA_MOVEMENT_LEFT,
    CAMERA_MOVEMENT_RIGHT
} camera_movement_t;

typedef struct
{
    vec3_t position;
    vec3_t front;
    vec3_t up;
    vec3_t right;
    vec3_t worldUp;
    f32    yaw;
    f32    pitch;
    f32    movementSpeed;
    f32    mouseSensitivity;
    f32    zoom;
    f32    aspect;
    f32    near;
    f32    far;
} camera_t;

void   CameraInit(camera_t *camera, vec3_t position, vec3_t up, f32 aspect);
mat4_t CameraGetViewMatrix(const camera_t *camera);
mat4_t CameraGetProjectionMatrix(const camera_t *camera);
void   CameraMove(camera_t *camera, vec3_t cameraMovement, f32 mouseOffsetX, f32 mouseOffsetY);
#endif
