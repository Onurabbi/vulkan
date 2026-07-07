#include "mesh.glsl"

[[vk::push_constant]] SkyboxData skyboxData;

struct VSOutput
{
    float4 position : SV_Position;
    float3 uvw;
};

SamplerCube skyboxTexture;

[shader("vertex")]
VSOutput main(uint vi: SV_VertexID)
{
    VSOutput output;

    Vertex *vertices = skyboxData.vertices;
    uint vertexIndex = skyboxData.vertexOffset + vi;

    float3 pos = float3(vertices[vertexIndex].vx, vertices[vertexIndex].vy, vertices[vertexIndex].vz);
    output.uvw = pos;
    //output.uvw.xy *= -1.0;

    // Remove translation from view matrix
    float4x4 view = skyboxData.model;
    view[0][3] = 0.0;
    view[1][3] = 0.0;
    view[2][3] = 0.0;

    output.position = mul(skyboxData.projection, mul(view, float4(pos, 1.0)));
    return output;
}

[shader("fragment")]
float4 main(VSOutput input)
{
    return skyboxTexture.Sample(input.uvw);
}

