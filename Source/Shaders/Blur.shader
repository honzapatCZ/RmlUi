#include "./Flax/GUICommon.hlsl"

#define BLUR_SIZE 5 // Define as needed
#define BLUR_NUM_WEIGHTS 3 // Define as needed

struct BasicVertex 
{
    float2 Position : POSITION0;
    float2 TexCoord : TEXCOORD0;
};
struct VSOUT
{
    float4 Position : SV_POSITION;
    float2 TexCoord[BLUR_SIZE] : TEXCOORD0;
};

META_CB_BEGIN(0, Data)
float2 _texelOffset;
float2 Dummy;

float2 _texCoordMin;
float2 _texCoordMax;
META_CB_END

Texture2D _tex : register(t0);
StructuredBuffer<float> _weights : register(t1);

META_VS(true, FEATURE_LEVEL_ES2)
META_VS_IN_ELEMENT(POSITION, 0, R32G32_FLOAT,       0, ALIGN, PER_VERTEX, 0, true)
META_VS_IN_ELEMENT(TEXCOORD, 0, R16G16_FLOAT,       0, ALIGN, PER_VERTEX, 0, true)
VSOUT VS(BasicVertex input)
{
    VSOUT output;

    output.Position = float4(input.Position, 0, 1);
    for (int i = 0; i < BLUR_SIZE; i++) {
        float2 texPos = float2((input.Position.x + 1.0f) * 0.5f, 1 - (input.Position.y + 1.0f) * 0.5f);
        output.TexCoord[i] = texPos - (float(i - BLUR_NUM_WEIGHTS + 1) * _texelOffset);
    }     

    return output;
}

META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_Blur(VSOUT input) : SV_Target0
{
    float4 color = float4(0.0, 0.0, 0.0, 0.0);
    float appliedWeight = 0;

    for (int i = 0; i < BLUR_SIZE; i++) {
        float2 clampedCoord = clamp(input.TexCoord[i], _texCoordMin, _texCoordMax);

        float4 sampleColor = _tex.Sample(SamplerLinearClamp, clampedCoord);
        float weight = _weights[abs(i - BLUR_NUM_WEIGHTS + 1)];

        color += sampleColor * weight;
    }

    return color;
}