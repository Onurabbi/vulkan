#include "mesh.glsl"
#include "math.glsl"

struct VSInput {
    float3 Pos;
    float3 Normal;
    float3 Tangent;
	float2 UV;
};

Sampler2D textures[];

[[vk::push_constant]] ShaderData shaderData;

struct VSOutput {
    float4 Pos : SV_POSITION;
    float3 Normal;
    float2 UV;
    float3 wPos;
    uint drawID;
};

[shader("vertex")]
VSOutput main(VSInput input, uint instanceIndex : SV_VulkanInstanceID, uint drawID : SV_DrawIndex)
{
    VSOutput output;

    DrawCommand *drawCommands = shaderData.drawCommands;
    DrawData *drawData = shaderData.drawData;
    Globals *globals = shaderData.globals;
    DrawCommand cmd = drawCommands[drawID];
    DrawData meshDraw = drawData[cmd.drawId];
    Mesh *meshes = shaderData.meshes;
    Mesh mesh = meshes[meshDraw.meshIndex];

    float3 wpos = rotateQuat(input.Pos, meshDraw.orientation) * meshDraw.scale + meshDraw.position;
    output.Pos = mul(globals.projection, mul(globals.view, float4(wpos, 1.0f)));
    output.Normal = rotateQuat(input.Normal, meshDraw.orientation);
    output.UV = input.UV;
    output.wPos = wpos;
    output.drawID = drawID;
    return output;
}

[shader("fragment")]
float4 main(VSOutput input, float4 fragCoord : SV_Position) {
    DrawData drawData = shaderData.drawData[input.drawID];
    Material material = shaderData.materials[drawData.materialIndex];

    float deband = gradientNoise(fragCoord.xy) * 2 - 1;

    float4 albedo = material.diffuseFactor;
    if (material.albedo != -1) {
        albedo *= fromsrgb(textures[NonUniformResourceIndex(material.albedo)].Sample(input.UV));
    }

    float3 nmap = float3(0, 0, 1);
    if (material.normal != -1) {
        nmap = textures[NonUniformResourceIndex(material.normal)].Sample(input.UV).rgb * 2 - 1;
    }

    float4 specgloss = material.specularFactor;
    if (material.specular != -1) {
        specgloss *= fromsrgb(textures[NonUniformResourceIndex(material.specular)]).Sample(input.UV);
    }

    float3 emissive = material.emissiveFactor;
    if (material.emissive != -1) {
        emissive *= fromsrgb(textures[NonUniformResourceIndex(material.emissive)]).Sample(input.UV).rgb;
    }

    // Sample from texture
    float3 color = textures[NonUniformResourceIndex(input.textureIndex)].Sample(input.UV).rgb;
    return float4(color, 1.0);
}
