#include "includes/mesh.slang"
#include "includes/math.slang"

[[vk::binding(0, 0)]] Sampler2D textures[];

[[vk::push_constant]] PBRData pbrData;

struct VSOutput {
    float4 position : SV_Position;
    float3 normal;
    float4 tangent;
    float2 uv;
    float3 wPos;
    uint drawID;
};

[shader("vertex")]
VSOutput main(uint instanceIndex : SV_VulkanInstanceID, uint drawID : SV_DrawIndex, uint vi : SV_VertexID)
{
    VSOutput output;

    DrawCommand *drawCommands = pbrData.drawCommands;
    DrawData *drawData        = pbrData.drawData;
    Globals *globals          = pbrData.globals;
    DrawCommand cmd           = drawCommands[drawID];
    DrawData meshDraw         = drawData[cmd.drawId];
    Mesh mesh                 = pbrData.meshes[meshDraw.meshIndex];
    Vertex *vertices          = pbrData.vertices;

    uint vertexIndex = vi + cmd.vertexOffset;
    float3 pos      = float3(vertices[vertexIndex].vx, vertices[vertexIndex].vy, vertices[vertexIndex].vz);
    float3 normal   = float3(int(vertices[vertexIndex].nx), int(vertices[vertexIndex].ny), int(vertices[vertexIndex].nz)) / 127.0 - 1.0;
    float2 uv       = float2(vertices[vertexIndex].u, vertices[vertexIndex].v);
    float4 tangent  = float4(int(vertices[vertexIndex].tx), int(vertices[vertexIndex].ty), int(vertices[vertexIndex].tz), int(vertices[vertexIndex].tw)) / 127.0 - 1.0; 
    float3 wpos     = rotateQuat(pos, meshDraw.orientation) * meshDraw.scale + meshDraw.position;

    output.position = mul(globals.projection, mul(globals.view, float4(wpos, 1.0f)));
    output.normal   = normal;
    output.tangent  = tangent;
    output.uv       = uv;
    output.wPos     = wpos;
    output.drawID   = cmd.drawId;

    return output;
}

float GeometrySchlickGGX(float ndotv, float k)
{
    float nom = ndotv;
    float denom = ndotv * (1.0f - k) + k;

    return nom / denom;
}

float GeometrySmith(float3 normal, float3 view, float3 light, float k)
{
    float ndotv = max(dot(normal, view), 0.0f);
    float ndotl = max(dot(normal, light), 0.0);
    float ggx1 = GeometrySchlickGGX(ndotv, k);
    float ggx2 = GeometrySchlickGGX(ndotl, k);

    return ggx1 * ggx2;
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0f - f0) * pow(1.0f - cosTheta, 5.0f);
}

float TrowbridgeReitzDistribution(float3 normal, float3 halfway, float a)
{
    float ndoth = max(dot(normal, halfway), 0.0f);
    float a2    = a * a;
    float nom   = a2;
    float denom = (ndoth * ndoth * (a2 - 1.0f) + 1.0f);
    denom *= denom * PI;

    return nom / denom;
}

float3 GetNormalFromMap(Sampler2D normalMap, float3 normal, float2 uv, float3 worldPos)
{
    float3 tangentNormal = normalMap.Sample(uv).rgb * 2.0 - 1.0;
    float3 q1  = ddx(worldPos);
    float3 q2 = ddy(worldPos);
    float2 st1 = ddx(uv);
    float2 st2 = ddy(uv);

    float3 n = normalize(normal);
    float3 t = normalize(q1 * st2.y - q2 * st1.y);
    float3 b = -normalize(cross(n, t));
    float3x3 tbn = float3x3(t, b, n);

    return normalize(mul(tbn, tangentNormal));
}

[shader("fragment")]
float4 main(VSOutput input) {
    DrawData drawData = pbrData.drawData[input.drawID];
    Material material = pbrData.materials[drawData.materialIndex];
    Globals *globals = pbrData.globals;

    float3 lightColor = float3(220.0, 255.0, 110.0);

    float3 camPos = globals.camPos;
    float3 view = normalize(camPos - input.wPos);

    float4 albedo = material.diffuseFactor;
    albedo *= fromsrgb(textures[NonUniformResourceIndex(material.albedo)].Sample(input.uv));

    float3 normal = GetNormalFromMap(textures[NonUniformResourceIndex(material.normal)], input.normal, input.uv, input.wPos);

    float3 metallicRoughness = textures[NonUniformResourceIndex(material.metalRoughness)].Sample(input.uv).rgb;
    float metallic = metallicRoughness.b;
    float roughness = metallicRoughness.g;

    float ao = textures[NonUniformResourceIndex(material.ao)].Sample(input.uv).r;
    ao = 1.0 + material.aoStrength * (ao - 1.0);

    float3 f0 = float3(0.04);
    f0 = lerp(f0, albedo.rgb, metallic);

    float3 lo = float3(0.0);

    float3 lightdir = globals.lightDir;
    float3 halfway = normalize(view + lightdir);
    float3 radiance = lightColor;

    // Cook-Torrance BRDF
    float ndf = TrowbridgeReitzDistribution(normal, halfway, roughness);
    float g   = GeometrySmith(normal, view, lightdir, roughness);
    float3 f  = FresnelSchlick(max(dot(halfway, view), 0.0), f0);

    float3 numerator = ndf * g * f;
    float denominator = 4.0 * max(dot(normal, view), 0.0) * max(dot(normal, lightdir), 0.0) + 0.0001;
    float3 specular = numerator / denominator;

    // kS is equal to fresnel term
    float3 kS = f;
    /**
     * for energy conservation, the diffuse and specular light can't be above 1.0
     * (unless the surface emits light); to preserve this relationship the diffuse component kD should
     * be equal to 1.0 - kS
     */
    float3 kD = float3(1.0) - kS;
    // nullifying diffuse light because metallic surfaces don't refract light, thus have no diffuse reflections
    kD *= 1.0 - metallic;

    float ndotl = max(dot(normal, lightdir), 0.0);
    lo += (kD * albedo.rgb / PI + specular) * radiance * ndotl;

    float3 ambient = float3(0.03) * albedo.rgb * ao;

    float3 color = ambient + lo;

    return float4(color, 1.0);
}
