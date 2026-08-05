#include "includes/structs.slang"
#include "includes/math.slang"
#include "includes/pbr_helpers.slang"
#include "includes/tonemapping.slang"

[[vk::binding(0, 0)]] Sampler2D textures[];
[[vk::binding(0, 0)]] SamplerCube cubemaps[];

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
    output.normal   = rotateQuat(normal, meshDraw.orientation);
    output.tangent  = float4(rotateQuat(tangent.xyz, meshDraw.orientation), tangent.w);
    output.uv       = uv;
    output.wPos     = wpos;
    output.drawID   = cmd.drawId;

    return output;
}

float3 GetNormalFromMap(Sampler2D normalMap, float3 normal, float4 tangent, float2 uv, float scale)
{
    float3 tangentNormal = normalMap.Sample(uv).rgb * 2.0 - 1.0;
    // gltf scales only the tangent-space x/y, leaving z alone
    tangentNormal.xy *= scale;

    float3 n = normalize(normal);
    // gram-schmidt: interpolation across the triangle skews t off the normal plane
    float3 t = normalize(tangent.xyz - n * dot(n, tangent.xyz));
    // gltf stores the bitangent handedness in tangent.w
    float3 b = cross(n, t) * tangent.w;

    // slang's float3x3(t, b, n) fills ROWS (unlike glsl's mat3, which fills columns),
    // so tangent->world is mul(v, tbn) -- mul(tbn, v) would apply the transpose
    float3x3 tbn = float3x3(t, b, n);

    return normalize(mul(tangentNormal, tbn));
}

static const uint NUM_LIGHTS = 1;

[shader("fragment")]
float4 main(VSOutput input) {
    DrawData drawData = pbrData.drawData[input.drawID];
    Material material = pbrData.materials[drawData.materialIndex];
    Globals *globals = pbrData.globals;

    float3 lightColor = float3(1.0, 1.0, 1.0);

    float4 albedo = material.diffuseFactor;
    albedo *= fromsrgb(textures[NonUniformResourceIndex(material.albedo)].Sample(input.uv));

    float ao = textures[NonUniformResourceIndex(material.ao)].Sample(input.uv).r;
    ao = 1.0 + material.aoStrength * (ao - 1.0);

    float3 metallicRoughness = textures[NonUniformResourceIndex(material.metalRoughness)].Sample(input.uv).rgb;
    float metallic = metallicRoughness.b * material.metallicFactor;
    float roughness = metallicRoughness.g * material.roughnessFactor;

    float3 v = normalize(globals.camPos - input.wPos);
    float3 n = GetNormalFromMap(textures[NonUniformResourceIndex(material.normal)], input.normal, input.tangent, input.uv, material.normalScale);
    float3 r = reflect(-v, n);
    
    // calculate the reflectance at normal incidence. If dia-electric use f0 of 0.04 and if it's metal use 
    // the albedo color as f0
    float3 f0 = float3(0.04);
    f0 = lerp(f0, albedo.rgb, metallic);

    // reflectance equation
    float3 l0 = float3(0.0);
    float3 l = normalize(globals.lightDir);
    float3 h = normalize(l + v);

    float ndotv = clamp(abs(dot(n, v)), 0.01, 1.0);
    float ndotl = clamp(dot(n, globals.lightDir), 0.001, 1.0);
    float ndoth = clamp(dot(n, h), 0.0, 1.0);
    float ldoth = clamp(dot(l, h), 0.0, 1.0);
    float vdoth = clamp(dot(v, h), 0.0, 1.0);

    for (int i = 0; i < 1; i++) {
        //float distance = length(lightPositions[i] - worldPos);
        //float attenuation = 1.0 / (distance * distance);
        //float radiance  = lightColors[i] * attenuation;
        float3 radiance = lightColor; 

        // cook-torrance brdf
        float ndf = TrowbridgeReitz(ndotv, roughness);
        float g   = G_SchlickSmith_GGX(ndotl, ndotv, roughness);
        float3 f  = FresnelSchlick(vdoth, f0);

        float3 numerator  = ndf * g * f;
        // same ndotv as g, otherwise a normal-mapped backfacing pixel divides by
        // the epsilon alone and the highlight explodes
        float denominator = 4.0 * ndotv * max(dot(n, l), 0.0) + 0.0001;
        float3 specular   = numerator / denominator;

        // kS is equal to fresnel
        float3 kS = f;

        //for energy conservation, the diffuse and specular light can't be above 1.0 (unless the surface emits light)
        float3 kD = float3(1.0) - kS;

        // multiply kD by the inverse metalness so that only non-metals have diffuse lighting or a linear blend if partly metal
        // pure metals have no diffuse light
        kD *= 1.0 - metallic;

        float ndotl = clamp(dot(n, l), 0.0, 1.0);

        //add to outgoing reflectance
        l0 += (kD * albedo.rgb / PI + specular) * radiance * ndotl;
    }

    // ambient lighting (we now use IBL as the ambient term)
    float3 f = FresnelSchlickRoughness(ndotv, f0, roughness);

    float3 kS = f;
    float3 kD = 1.0 - kS;
    kD *= 1.0 - metallic;

    // explicit LOD 0: the map is already fully convolved, and n is normal-mapped, so
    // implicit derivatives would pick a tiny mip of an already-64x64 cubemap
    float3 irradiance = cubemaps[NonUniformResourceIndex(globals.diffuseIrradianceIndex)].SampleLevel(n, 0.0).rgb;
    float3 diffuse = irradiance * albedo.rgb;

    // the bake writes roughness = level / (mipCount - 1), so the lookup has to
    // span the whole chain -- not a hardcoded LOD count
    uint prefilteredW, prefilteredH, prefilteredMips;
    cubemaps[NonUniformResourceIndex(globals.prefilteredEnvIndex)].GetDimensions(0, prefilteredW, prefilteredH, prefilteredMips);
    float lod = roughness * float(prefilteredMips - 1);
    float3 prefilteredColor = cubemaps[NonUniformResourceIndex(globals.prefilteredEnvIndex)].SampleLevel(r, lod).rgb;
    float2 brdf = textures[NonUniformResourceIndex(globals.brdflutIndex)].Sample(float2(ndotv, roughness)).rg;
    float3 specular = prefilteredColor * (f * brdf.x + brdf.y);
    float3 ambient = (kD * diffuse + specular) * ao;
    float3 color = ambient + l0;

    float3 emissive = material.emissiveFactor.rgb;
    emissive *= fromsrgb(textures[NonUniformResourceIndex(material.emissive)].Sample(input.uv)).rgb;
    color += emissive;

    return float4(ACESFitted(color), 1.0);
}
