#pragma once
#include "math.slang"

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

// a perfect mirror (roughness 0) makes the ggx terms collapse to 0/0 -- the
// visibility term at grazing angles, the lobe at ndoth == 1. clamp perceptual
// roughness the way filament does; 0.045 is the smallest value that stays
// finite in fp32
static const float MIN_PERCEPTUAL_ROUGHNESS = 0.045;

// Geometric shadowing function
float G_SchlickSmith_GGX(float dotnl, float dotnv, float roughness)
{
    float a = max(roughness, MIN_PERCEPTUAL_ROUGHNESS);
    // k stays > 0 after the clamp, so dotnl/dotnv of 0 give 0 instead of 0/0
    float k = (a * a) / 2.0f;
    float gl = dotnl / (dotnl * (1.0 - k) + k);
    float gv = dotnv / (dotnv * (1.0 - k) + k);
    return gl * gv;
}

// Normal Distribution function
float D_GGX(float dotNH, float roughness)
{
	float alpha = max(roughness, MIN_PERCEPTUAL_ROUGHNESS);
	alpha = alpha * alpha;
	float alpha2 = alpha * alpha;
	float denom = dotNH * dotNH * (alpha2 - 1.0) + 1.0;
	return alpha2 / max(PI * denom * denom, 1e-7);
}

float TrowbridgeReitz(float ndoth, float roughness)
{
    float a = max(roughness, MIN_PERCEPTUAL_ROUGHNESS);
    a = a * a;
    float a2 = a * a;
    float ndoth2 = ndoth * ndoth;
    float nom = a2;
    float denom = (ndoth2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return nom / max(denom, 1e-7);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 f0, float roughness)
{
    return f0 + (max(float3(1.0 - roughness), f0) - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0f - f0) * pow(1.0f - cosTheta, 5.0f);
}
