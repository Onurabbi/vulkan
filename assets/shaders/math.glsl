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
