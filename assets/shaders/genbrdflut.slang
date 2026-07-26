#include "includes/pbr_helpers.slang"

static const uint NUM_SAMPLES = 1024u;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOutput main(uint vi: SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vi << 1) & 2, vi & 2);
    output.position = float4(output.uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

float2 BRDF(float ndotv, float roughness)
{
    // Normal always points along z-axis for the 2D lookup
    const float3 n = float3(0.0, 0.0, 1.0);
    float3 v = float3(sqrt(1.0 - ndotv * ndotv), 0.0, ndotv);

    float2 lut = float2(0.0);
    for (uint i = 0; i < NUM_SAMPLES; i++) {
        float2 xi = Hammersley2d(i, NUM_SAMPLES);
        float3 h = ImportanceSample_GGX(xi, roughness, n);
        float3 l = 2.0 * dot(v, h) * h - v;

        float dotnl = max(dot(n, l), 0.0);
        float dotnv = max(dot(n, v), 0.0);
        float dotvh = max(dot(v, h), 0.0);
        float dotnh = max(dot(h, n), 0.0);

        if (dotnl > 0.0) {
            float g = G_SchlickSmith_GGX(dotnl, dotnv, roughness);
            float gvis = (g * dotvh) / (dotnh * dotnv);
            float fc = pow(1.0 - dotvh, 5.0);
            lut += float2((1.0 - fc) * gvis, fc * gvis);
        }
    }
    return lut / float(NUM_SAMPLES);
}

[shader("fragment")]
float4 main(VSOutput input) : SV_Target
{
    float ndotv = max(input.uv.x, 0.001);
    float roughness = max(1.0 - input.uv.y, 0.001);
    return float4(BRDF(input.uv.x, 1.0 - input.uv.y), 0.0, 1.0);
}
