/* Copyright (c) 2025-2026, Sascha Willems
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "mesh.h"

struct VSInput {
    float3 Pos;
    float3 Normal;
	float2 UV;
};

Sampler2D textures[];

[[vk::push_constant]] ShaderData shaderData;

struct VSOutput {
    float4 Pos : SV_POSITION;
    float3 Normal;
    float2 UV;
    float3 wPos;
    uint textureIndex;
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
    output.textureIndex = mesh.textureIndex;
    return output;
}

[shader("fragment")]
float4 main(VSOutput input) {
    // Sample from texture
    float3 color = textures[NonUniformResourceIndex(input.textureIndex)].Sample(input.UV).rgb;
    return float4(color, 1.0);
}
