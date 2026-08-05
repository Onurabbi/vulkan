#include "includes/structs.slang"
#include "includes/math.slang"

static const float G   = 9.81f;
static const uint N    = 1024;

[[vk::push_constant]] InitialOceanSpectrumData initialOceanParams;
[[vk::binding(1, 0)]] [format("rgba32f")] RWTexture2D<float4> storageImages[];

float Dispersion(float kMag)
{
    return sqrt(G * kMag * tanh(min(kMag * initialOceanParams.depth, 20)));
}

float DispersionDerivative(float kMag) 
{
    float depth = initialOceanParams.depth;
    float th = tanh(min(kMag * depth, 20));
    float ch = cosh(kMag * depth);
    return G * (depth * kMag / ch / ch + th) / Dispersion(kMag) / 2.0f;
}

float NormalizationFactor(float s) {
    float s2 = s * s;
    float s3 = s2 * s;
    float s4 = s3 * s;
    if (s < 5) return -0.000564f * s4 + 0.00776f * s3 - 0.044f * s2 + 0.192f * s + 0.163f;
    else return -4.80e-08f * s4 + 1.07e-05f * s3 - 9.53e-04f * s2 + 5.90e-02f * s + 3.93e-01f;
}

float DonelanBannerBeta(float x) {
	if (x < 0.95f) return 2.61f * pow(abs(x), 1.3f);
	if (x < 1.6f) return 2.28f * pow(abs(x), -1.3f);

	float p = -0.4f + 0.8393f * exp(-0.567f * log(x * x));
	return pow(10.0f, p);
}

float DonelanBanner(float theta, float omega, float peakOmega) {
	float beta = DonelanBannerBeta(omega / peakOmega);
	float sech = 1.0f / cosh(beta * theta);
	return beta / 2.0f / tanh(beta * 3.1416f) * sech * sech;
}

float Cosine2s(float theta, float s) {
	return NormalizationFactor(s) * pow(abs(cos(0.5f * theta)), 2.0f * s);
}

float SpreadPower(float omega, float peakOmega) {
	if (omega > peakOmega)
		return 9.77f * pow(abs(omega / peakOmega), -2.5f);
	else
		return 6.97f * pow(abs(omega / peakOmega), 5.0f);
}

float DirectionSpectrum(float theta, float omega, JonswapData spectrum) {
	float s = SpreadPower(omega, spectrum.peakOmega) + 16 * tanh(min(omega / spectrum.peakOmega, 20)) * spectrum.swell * spectrum.swell;
	return lerp(2.0f / 3.1415f * cos(theta) * cos(theta), Cosine2s(theta - spectrum.angle, s), spectrum.spreadBlend);
}

float TMACorrection(float omega) {
	float omegaH = omega * sqrt(initialOceanParams.depth / G);
	if (omegaH <= 1.0f)
		return 0.5f * omegaH * omegaH;
	if (omegaH < 2.0f)
		return 1.0f - 0.5f * (2.0f - omegaH) * (2.0f - omegaH);

	return 1.0f;
}

float JONSWAP(float omega, JonswapData spectrum) {
	float sigma = (omega <= spectrum.peakOmega) ? 0.07f : 0.09f;

	float r = exp(-(omega - spectrum.peakOmega) * (omega - spectrum.peakOmega) / 2.0f / sigma / sigma / spectrum.peakOmega / spectrum.peakOmega);
	
	float oneOverOmega = 1.0f / omega;
	float peakOmegaOverOmega = spectrum.peakOmega / omega;
	return spectrum.scale * TMACorrection(omega) * spectrum.alpha * G * G
		* oneOverOmega * oneOverOmega * oneOverOmega * oneOverOmega * oneOverOmega
		* exp(-1.25f * peakOmegaOverOmega * peakOmegaOverOmega * peakOmegaOverOmega * peakOmegaOverOmega)
		* pow(abs(spectrum.gamma), r);
}

float ShortWavesFade(float kLength, JonswapData spectrum) {
	return exp(-spectrum.shortWavesFade * spectrum.shortWavesFade * kLength * kLength);
}

[shader("compute")]
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadId)
{
    JonswapData *jonswaps = initialOceanParams.jonswap;

    uint seed = id.x + N * id.y + N;
    seed += initialOceanParams.seed;

    for (uint i = 0; i < 4; i++) {
        float halfN = float(N) / 2.0f;

        float deltaK = 2.0f * PI / initialOceanParams.lengthScale[i];
        float2 K = (id.xy - halfN) * deltaK;
        float kLength = length(K);

        seed += i + hash(seed) * 10;
        float4 uniformRandSamples = float4(hash(seed), hash(seed * 2), hash(seed * 3), hash(seed * 4));
        float2 gauss1 = UniformToGaussian(uniformRandSamples.x, uniformRandSamples.y);
        float2 gauss2 = UniformToGaussian(uniformRandSamples.z, uniformRandSamples.w);

        if (initialOceanParams.lowCutoff <= kLength && kLength <= initialOceanParams.highCutoff) {
            float kAngle = atan2(K.y, K.x);
            float omega = Dispersion(kLength);

            float dOmegadk = DispersionDerivative(kLength);
            float spectrum = JONSWAP(omega, jonswaps[i * 2 + 0]) * DirectionSpectrum(kAngle, omega, jonswaps[i * 2]) * ShortWavesFade(kLength, jonswaps[i * 2]);
            if (jonswaps[i * 2 + 1].scale > 0) {
                spectrum += JONSWAP(omega, jonswaps[i * 2 + 1]) * DirectionSpectrum(kAngle, omega, jonswaps[i * 2 + 1]) * ShortWavesFade(kLength, jonswaps[i * 2 + 1]);
            }
            storageImages[NonUniformResourceIndex(initialOceanParams.textureIndices[i])][id.xy] = float4(float2(gauss2.x, gauss2.y) * sqrt(2 * spectrum * abs(dOmegadk) / kLength * deltaK * deltaK), 0.0f, 0.0f);
        } else {
            storageImages[NonUniformResourceIndex(initialOceanParams.textureIndices[i])][id.xy] = 0.0f;
        }
    }
}
