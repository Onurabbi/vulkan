#pragma once

#define INVALID_TEXTURE 0xFFFFFFFF

struct Vertex {
    half vx, vy, vz, vw;
    uint8_t nx, ny, nz, nw;
    uint8_t tx, ty, tz, tw;
    half u, v;
};

struct MeshLod {
    uint indexOffset;
    uint indexCount;
    float error;
};

struct Mesh {
    float3 center;
    float radius;
    uint vertexOffset;
    uint vertexCount;
    uint lodCount;
    MeshLod meshLods[8];
};

struct DrawData {
    float4 orientation;
    float3 position;
    float scale;
    uint meshIndex;
    uint materialIndex;
    uint postPass;
};

struct Globals {
    float P00, P11, near, far;
    float4 frustum;
    float4x4 projection;
    float4x4 view;
    float3 lightDir;
    float3 camPos;
    uint selected;
    uint drawCount;
    float lodTarget;
};

struct DrawCommand {
    uint drawId;
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    uint vertexOffset;
    uint firstInstance;
};

struct Material {
    uint albedo;
    uint metalRoughness;
    uint normal;
    uint emissive;
    uint ao;
    float4 diffuseFactor;
    float3 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float aoStrength;
    float normalScale;
};

// push constant
struct PBRData {
    Globals     *globals;
    Mesh        *meshes;
    DrawCommand *drawCommands;
    uint        *drawCommandCount;
    DrawData    *drawData;
    Material    *materials;
    Vertex      *vertices;
};

// push constant
struct SkyboxData {
    float4x4 projection;
    float4x4 model;
    float4x4 view;
    Vertex *vertices;
    // we need to know this because we are using a storage buffer for vertices
    uint vertexOffset;
    // index offset and index count are set by the draw call
    uint indexOffset;
    uint indexCount;
    uint textureIndex;
    float roughness;
};
