float3 rotateQuat(float3 v, float4 q)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

// http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare
float gradientNoise(vec2 uv)
{
    return fract(52.9829189 * fract(dot(uv, vec2(0.06711056, 0.00583715))));
}

vec3 fromsrgb(vec3 c)
{
    return pow(c.xyz, vec3(2.2));
}

vec4 fromsrgb(vec4 c)
{
    return vec4(pow(c.xyz, vec3(2.2)), c.w);
}
