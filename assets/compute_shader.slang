struct InputData
{
    uint *drawCount;
};

[[vk::push_constant]] InputData inputData;

[shader("compute")]
[numthreads(1,1,1)]
void main(uint3 id : SV_DispatchThreadId)
{
    inputData.drawCount[id.x] = 1;
}
