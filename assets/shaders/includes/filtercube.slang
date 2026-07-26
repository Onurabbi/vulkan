#pragma once

#include "mesh.slang"

struct VSOutput
{
    float4 position : SV_Position;
    float3 uvw;
};

SamplerCube textures[];

[[vk::push_constant]] SkyboxData skyboxData;

[shader("vertex")]
VSOutput main(uint vi: SV_VertexID)
{
    VSOutput output;

    Vertex *vertices = skyboxData.vertices;
    uint vertexIndex = skyboxData.vertexOffset + vi;

    float3 pos = float3(vertices[vertexIndex].vx, vertices[vertexIndex].vy, vertices[vertexIndex].vz);
    output.uvw.x = pos.x;
    output.uvw.y = -pos.z;
    output.uvw.z = -pos.y;

    output.position = mul(skyboxData.projection, mul(skyboxData.view, mul(skyboxData.model, float4(pos, 1.0))));
    return output;
}
