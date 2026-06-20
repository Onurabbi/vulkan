#include "mesh.glsl"

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
  
    Mesh mesh = meshes[meshDraw.meshIndex];
    float3 center = mesh.center;
    float radius = mesh.radius;
    uint32_t vertexCount = mesh.vertexCount;
    uint32_t vertexOffset = mesh.vertexOffset;

    center = rotateQuat(center, meshDraw.orientation) * meshDraw.scale + meshDraw.position;
    center = mul(globals.view, float4(center, 1.0f)).xyz;
    radius = radius * meshDraw.scale;

    bool visible = true;
    visible = visible && center.z * globals.frustum[1] - abs(center.x) * globals.frustum[0] > -radius;
    visible = visible && center.z * globals.frustum[3] - abs(center.y) * globals.frustum[2] > -radius;
    visible = visible && center.z + radius > globals.near && center.z - radius < globals.far;

    if (visible) {
        uint dci = __atomic_add(shaderData.drawCommandCount[0], 1);

        uint lodIndex = 0;
        float distance = max(length(center) - radius, 0);
        float threshold = distance * globals.lodTarget / meshDraw.scale;

        for (uint i = 0; i < mesh.lodCount; i++) {
            if (mesh.meshLods[i].error < threshold) {
                lodIndex = i;
            }
        }

        MeshLod lod = mesh.meshLods[lodIndex];

        drawCommands[dci].indexCount = lod.indexCount;
        drawCommands[dci].drawId = id.x;
        drawCommands[dci].instanceCount = 1;
        drawCommands[dci].firstIndex = lod.indexOffset;
        drawCommands[dci].vertexOffset = vertexOffset;
        drawCommands[dci].firstInstance = 0;
    }
}
