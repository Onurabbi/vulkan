#include "camera.h"

const f32 YAW         = -90.0f;
const f32 PITCH       = 0.0f;
const f32 SPEED       = 0.5f;
const f32 SENSITIVITY = 0.1f;
const f32 ZOOM        = 45.0f;
const vec3_t FRONT    = { 0.0f, 0.0f, -1.0f };
const f32 NEAR        = 0.1f;
const f32 FAR         = 256.0f;

void UpdateCameraVectors(camera_t *camera)
{
    vec3_t front;
    front.X = cosf(HMM_AngleDeg(camera->yaw)) * cosf(HMM_AngleDeg(camera->pitch));
    front.Y = sinf(HMM_AngleDeg(camera->pitch));
    front.Z = sinf(HMM_AngleDeg(camera->yaw)) * cosf(HMM_AngleDeg(camera->pitch));
    camera->front = HMM_Norm(front);
    camera->right = HMM_Norm(HMM_Cross(camera->front, camera->worldUp));
    camera->up    = HMM_Norm(HMM_Cross(camera->right, camera->front));
}

void CameraMove(camera_t *camera, vec3_t cameraMovement, f32 offsetX, f32 offsetY)
{
    if (HMM_LenSqrV3(cameraMovement) > EPSILON * EPSILON) {
        cameraMovement = HMM_Norm(cameraMovement);
    }

    vec3_t dRight = {0};
    if (FLOAT_NOT_EQUAL_TO_ZERO(cameraMovement.X)) {
        dRight = HMM_Mul(camera->right, cameraMovement.X * camera->movementSpeed);
    }

    vec3_t dFront = {0};
    if (FLOAT_NOT_EQUAL_TO_ZERO(cameraMovement.Y)) {
        dFront = HMM_Mul(camera->front, cameraMovement.Y * camera->movementSpeed);
    }

    vec3_t dUp = {0};
    if (FLOAT_NOT_EQUAL_TO_ZERO(cameraMovement.Z)) {
        dUp = HMM_Mul(camera->up, cameraMovement.Z * camera->movementSpeed);
    }

    camera->position = HMM_Add(camera->position, HMM_Add(dUp, HMM_Add(dFront, dRight)));

    f32 yaw = camera->yaw + offsetX * camera->mouseSensitivity;
    f32 pitch = camera->pitch + offsetY * camera->mouseSensitivity;

    if (pitch > 89.0f) {
        pitch = 89.0f;
    }

    if (pitch < -89.0f) {
        pitch = -89.0f;
    }
    
    camera->yaw   = yaw;
    camera->pitch = pitch;

    UpdateCameraVectors(camera);
}

void CameraInit(camera_t *camera, vec3_t position, vec3_t up, f32 aspect)
{
    camera->position         = position;
    camera->worldUp          = up;
    camera->yaw              = YAW;
    camera->pitch            = PITCH;
    camera->front            = FRONT;
    camera->movementSpeed    = SPEED;
    camera->mouseSensitivity = SENSITIVITY;
    camera->zoom             = ZOOM;
    camera->aspect           = aspect;
    camera->near             = NEAR;
    camera->far              = FAR;

    UpdateCameraVectors(camera);
}

const mat4_t CameraGetViewMatrix(const camera_t *camera)
{
    return HMM_LookAt_RH(camera->position, HMM_Add(camera->position, camera->front), camera->up);
}

const mat4_t CameraGetProjectionMatrix(const camera_t *camera)
{
    return HMM_Perspective_RH_ZO(HMM_AngleDeg(camera->zoom), camera->aspect, camera->near, camera->far);
}
