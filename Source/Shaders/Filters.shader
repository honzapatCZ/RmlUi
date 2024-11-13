#include "./Flax/GUICommon.hlsl"

struct BasicVertex
{
    float2 Position : POSITION0;
    float2 TexCoord : TEXCOORD0;
};
struct VSOUT
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

META_CB_BEGIN(0, Data)
float4x4 ViewProjection;
float4x4 Model;
float2 Offset;
float2 UVScale;

float4x4 _color_matrix;

float2 _texCoordMin;
float2 _texCoordMax;
float4 _color;
META_CB_END

Texture2D _tex : register(t0);
Texture2D _texMask : register(t1);

META_VS(true, FEATURE_LEVEL_ES2)
META_VS_IN_ELEMENT(POSITION, 0, R32G32_FLOAT,       0, ALIGN, PER_VERTEX, 0, true)
META_VS_IN_ELEMENT(TEXCOORD, 0, R32G32_FLOAT,       0, ALIGN, PER_VERTEX, 0, true)
VSOUT VS(BasicVertex input)
{
    VSOUT output;

    output.Position = float4(input.Position, 0, 1);
    output.TexCoord = input.TexCoord;

    return output;
}

META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_PassThrough(VSOUT input) : SV_Target0
{
    return _tex.Sample(SamplerLinearWrap , input.TexCoord * UVScale+(Offset*float2(1,-1)));
}
META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_DropShadow(VSOUT input) : SV_Target0
{
    float2 in_region = step(_texCoordMin, input.TexCoord * UVScale+(Offset*float2(1,-1))) * step(input.TexCoord, _texCoordMax);
    return _tex.Sample(SamplerLinearWrap , input.TexCoord * UVScale+(Offset*float2(1,-1))).a * in_region.x * in_region.y * _color;
}
META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_ColorMatrix(VSOUT input) : SV_Target0
{
    float4 texColor = _tex.Sample(SamplerLinearWrap , input.TexCoord * UVScale+(Offset*float2(1,-1)));
    float3 transformedColor = mul(_color_matrix, texColor).xyz;
    return float4(transformedColor, texColor.a);
}
META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_MaskImage(VSOUT input) : SV_Target0
{
    float4 texColor = _tex.Sample(SamplerLinearWrap , input.TexCoord * UVScale+(Offset*float2(1,-1)));
    float maskAlpha = _texMask.Sample(SamplerLinearWrap , input.TexCoord * UVScale+(Offset*float2(1,-1))).a;
    return texColor * maskAlpha;
}