#include "mesh.glsl"
#include "math.glsl"

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

[shader("fragment")]
float4 main(VSOutput input) : SV_Target
{
    float3 n     = normalize(input.uvw);
    float3 up    = abs(n.y) > 0.999f ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    float3 right = normalize(cross(up, n));
    up           = normalize(cross(n, right));

    const float TWO_PI      = PI * 2.0f;
    const float HALF_PI     = PI * 0.5f;
    const float DELTA_PHI   = TWO_PI / 180.0f;
    const float DELTA_THETA = HALF_PI / 64.0f;

    float3 color     = float3(0.0);
    uint sampleCount = 0;
    
    for (float phi = 0.0; phi < TWO_PI; phi += DELTA_PHI) {
        for (float theta = 0.0; theta < HALF_PI; theta += DELTA_THETA) {
            float3 temp = mul(cos(phi), right) + mul(sin(phi), up);
            float3 sampleVector = normalize(mul(cos(theta), n) + mul(sin(theta), temp));
            color += textures[NonUniformResourceIndex(skyboxData.textureIndex)].Sample(sampleVector).rgb * cos(theta) * sin(theta);
            sampleCount++;
        }
    }

    color = PI * color * (1.0 / float(sampleCount));
    return float4(color, 1.0);
}
