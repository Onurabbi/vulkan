#include "mesh.h"

[[vk::push_constant]] ShaderData shaderData;

[shader("compute")]
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadId)
{
    Mesh* meshes = shaderData.meshes;
    DrawData *drawData = shaderData.drawData;
    DrawCommand *drawCommands = shaderData.drawCommands;
    Globals *globals = shaderData.globals;

    uint drawCount = globals.drawCount;
    if (id.x >= drawCount) {
        return;
    }

    DrawData meshDraw = drawData[id.x];
#if 0    
    Mesh mesh = meshes[meshDraw.meshIndex];
#else
    float3 center = float3(0.0f, 0.0f, 0.0f);
    float radius = 1.0f;
    uint32_t vertexCount = 11808;
    uint32_t vertexOffset = 0;
#endif
    center = rotateQuat(center, meshDraw.orientation) * meshDraw.scale + meshDraw.position;
    center = mul(globals.view, float4(center, 1.0f)).xyz;
    radius = radius * meshDraw.scale;

    bool visible = true;
    visible = visible && center.z * globals.frustum[1] - abs(center.x) * globals.frustum[0] > -radius;
    visible = visible && center.z * globals.frustum[3] - abs(center.y) * globals.frustum[2] > -radius;
    visible = visible && center.z + radius > globals.near && center.z - radius < globals.far;

    if (visible) {
        uint dci = __atomic_add(shaderData.drawCommandCount[0], 1);

        drawCommands[dci].indexCount = vertexCount;
        drawCommands[dci].drawId = id.x;
        drawCommands[dci].instanceCount = 1;
        drawCommands[dci].firstIndex = 0;
        drawCommands[dci].vertexOffset = vertexOffset;
        drawCommands[dci].firstInstance = 0;
    }
}
