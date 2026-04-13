
struct DrawData {

};

struct DrawCommand {

};

struct CullData {
    DrawData *draws;
    uint32_t drawCount;
    DrawCommand *drawCommands;
    uint32_t drawCommandCount;
    float4x4 view;
    float P00,P11,znear,zfar;
    float4 frustum;
};

[[vk::push_constant]] CullData cullData;

[shader("compute")]
[numthreads(32,1,1)]
void computeMain(uint3 id : SV_DispatchThreadId)
{
    if (id.x >= cullData.drawCount) {
        return;
    }

    DrawData drawData = cullData.draws[id.x];

}