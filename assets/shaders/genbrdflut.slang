
#include "math.glsl"

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

// Based omn http://byteblacksmith.com/improvements-to-the-canonical-one-liner-glsl-rand-for-opengl-es-2-0/
float random(float2 co)
{
    float a = 12.9898;
    float b = 78.233;
    float c = 43758.5453;
    float dt = dot(co.xy, float2(a, b));
    float sn = fmod(dt, 3.14);
    return fract(sin(sn) * c);
}

float2 Hammersley2d(uint i, uint N)
{
    // Radical inverse based on http://holger.dammertz.org/stuff/notes_HammersleyOnHemisphere.html
    uint bits = (i << 16u) | (i >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    float rdi = float(bits) * 2.3283064365386963e-10;
    return float2(float(i) / float(N), rdi);
}

// Based on http://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_slides.pdf
float3 ImportanceSample_GGX(float2 Xi, float roughness, float3 normal)
{
    // Maps a 2D point to a hemisphere with spread based on roughness
    float alpha = roughness * roughness;
    float phi = 2.0 * PI * Xi.x + random(normal.xz) * 0.1;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (alpha * alpha - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float3 H = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    // Tangent space
    float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangentX = normalize(cross(up, normal));
    float3 tangentY = normalize(cross(normal, tangentX));

    // Convert to world Space
    return normalize(tangentX * H.x + tangentY * H.y + normal * H.z);
}

// Geometric shadowing function
float G_SchlickSmith_GGX(float dotnl, float dotnv, float roughness)
{
    float k = (roughness * roughness) / 2.0f;
    float gl = dotnl / (dotnl * (1.0 - k) + k);
    float gv = dotnv / (dotnv * (1.0 - k) + k);
    return gl * gv;
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
