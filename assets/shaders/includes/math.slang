#pragma once

static const float PI = 3.1415926535897932384626433832795;
static const float exposure = 1.0f;
static const float gamma = 2.2f;

float3 rotateQuat(float3 v, float4 q)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

// http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare
float gradientNoise(float2 uv)
{
    return fract(52.9829189 * fract(dot(uv, float2(0.06711056, 0.00583715))));
}

float3 fromsrgb(float3 c)
{
    return pow(c.xyz, float3(2.2));
}

float4 fromsrgb(float4 c)
{
    return float4(pow(c.xyz, float3(2.2)), c.w);
}

float hash(uint n) {
    // integer hash copied from Hugo Elias
    n = (n << 13U) ^ n;
    n = n * (n * n * 15731U + 0x789221U) + 0x13763125U;
    return float(n & uint(0x7fffffffU)) / float(0x7fffffff);
}

float2 UniformToGaussian(float u1, float u2) {
    float R = sqrt(-2.0f * log(u1));
    float theta = 2.0f * PI * u2;
    
    return float2(R * cos(theta), R * sin(theta));
}
