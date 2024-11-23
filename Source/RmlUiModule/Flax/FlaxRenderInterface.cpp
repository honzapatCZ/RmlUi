#include "RmlUiPlugin.h"

// Conflicts with both Flax and RmlUi Math.h
#undef RadiansToDegrees
#undef DegreesToRadians
#undef NormaliseAngle

#include "FlaxFontEngineInterface.h"
#include "FlaxRenderInterface.h"
#include "../RmlUiHelpers.h"
#include "StaticIndexBuffer.h"
#include "StaticVertexBuffer.h"

#include <ThirdParty/RmlUi/Core/Context.h>
#include <ThirdParty/RmlUi/Core/Core.h>
#include <ThirdParty/RmlUi/Core/FontEngineInterface.h>
#include <ThirdParty/RmlUi/Core/DecorationTypes.h>

#include <Engine/Content/Assets/MaterialBase.h>
#include <Engine/Content/Assets/Shader.h>
#include <Engine/Content/Assets/Texture.h>
#include <Engine/Content/Content.h>
#include <Engine/Core/Collections/Array.h>
#include <Engine/Core/Collections/Dictionary.h>
#include <Engine/Core/Collections/HashSet.h>
#include <Engine/Core/Log.h>
#include <Engine/Core/Math/Matrix.h>
#include <Engine/Graphics/GPUContext.h>
#include <Engine/Graphics/GPUDevice.h>
#include <Engine/Graphics/GPUPipelineState.h>
#include <Engine/Graphics/Async/GPUTask.h>
#include <Engine/Graphics/Models/Types.h>
#include <Engine/Graphics/RenderTask.h>
#include <Engine/Graphics/Shaders/GPUShader.h>
#include <Engine/Graphics/Textures/GPUTexture.h>
#include <Engine/Profiler/Profiler.h>
#include <Engine/Render2D/FontManager.h>
#include <Engine/Graphics/RenderTargetPool.h>

//MSAA
static constexpr int NUM_MSAA_SAMPLES = 2;

//Blur
#define BLUR_SIZE 7
#define BLUR_NUM_WEIGHTS ((BLUR_SIZE + 1) / 2)

//Gradients
#define MAX_NUM_STOPS 16

#pragma region Types
struct BasicVertex
{
    Float2 Position;
    Float2 TexCoord;
    Color Color;
    Float2 ClipOrigin;
    Float4 ClipExtents;
};

struct CompiledGeometry
{
public:
    CompiledGeometry()
        : reserved(true), vertexBuffer(512, sizeof(BasicVertex), TEXT("RmlUI.VB")), indexBuffer(64, sizeof(uint32), TEXT("RmlUI.IB"))
    //, isFont(false)
    {
    }

    ~CompiledGeometry()
    {
        Dispose(false);
    }

    void Dispose(bool preserveBuffers = true)
    {
        reserved = false;
        if (preserveBuffers)
        {
            vertexBuffer.Clear();
            indexBuffer.Clear();
        }
        else
        {
            vertexBuffer.Dispose();
            indexBuffer.Dispose();
        }
        // isFont = false;
    }

    bool reserved;
    StaticVertexBuffer vertexBuffer;
    StaticIndexBuffer indexBuffer;
    // bool isFont;
};


enum class FilterType
{
    Invalid = 0,
    Passthrough,
    Blur,
    DropShadow,
    ColorMatrix,
    MaskImage
};
struct CompiledFilter
{
public:
    CompiledFilter()
        : reserved(true),
        type(FilterType::Invalid),
        blend_factor(1),
        sigma(0),
        offset(Float2(0.0f)),
        color(Color().Pink),
        color_matrix(Matrix().Identity)
    //, isFont(false)
    {
    }

    ~CompiledFilter()
    {
        Dispose();
    }

    void Dispose()
    {
        type = FilterType::Invalid;
        reserved = false;
        // isFont = false;
    }

    bool reserved;
    FilterType type;

    // Passthrough
    float blend_factor;
    // Blur
    float sigma;

    // Drop shadow
    Float2 offset;
    Color color;

    // ColorMatrix
    Matrix color_matrix;
};
enum class CompiledShaderType
{
    Invalid = 0,
    Gradient,
    Creation
};
enum class ShaderGradientFunction
{
    Linear = 0,
    Radial,
    Conic,
    RepeatingLinear,
    RepeatingRadial,
    RepeatingConic
}; // Must match shader definitions below.
struct CompiledShader
{
    CompiledShader()
        : reserved(true)
    //, isFont(false)
    {
    }

    ~CompiledShader()
    {
        Dispose();
    }

    void Dispose()
    {
        reserved = false;
        // isFont = false;
    }

    bool reserved;

    CompiledShaderType type;

    // Gradient
    ShaderGradientFunction gradient_function;
    Float2 p;
    Float2 v;
    Array<float> stop_positions;
    Array<Color> stop_colors;

    // Shader
    Float2 dimensions;
};
PACK_STRUCT(struct BlurCustomData {
    Float2 _texelOffset;
    Float2 Dummy;
    
    Float2 _texCoordMin;
    Float2 _texCoordMax;
});
PACK_STRUCT(struct FilterCustomData {
    Matrix ViewProjection;
    Matrix Model;
    Float2 Offset = Float2(0,0);
    Float2 UVScale = Float2(1, 1);

    Matrix _color_matrix;

    Float2 _texCoordMin;
    Float2 _texCoordMax;
    Color _color;
});
PACK_STRUCT(struct RmlShaderCustomData {
    ShaderGradientFunction GradientFunction;
    int NumStops;
    Float2 P;
    Float2 V;


    Float2 Offset;
    Matrix ViewProjection;
    Matrix Model;

    Color Colors[MAX_NUM_STOPS];
    float ColorStops[MAX_NUM_STOPS];
});

PACK_STRUCT(struct CustomData {
    Matrix ViewProjection;
    Matrix Model;
    Float2 Offset;
    Float2 Dummy;
});
PACK_STRUCT(struct BlitData {
    Float4 sourceRect; 
    Float4 targetRect;
});
#pragma endregion

namespace
{
    RenderContext *CurrentRenderContext = nullptr;
    GPUContext *CurrentGPUContext = nullptr;
    Viewport CurrentViewport;

    Rectangle CurrentScissor;
    Matrix CurrentTransform;
    Matrix ViewProjection;
    bool UseScissor = false;
    bool UseStencil = false;

    AssetReference<Shader> BasicShader;
    AssetReference<Shader> GUIShader;
    AssetReference<Shader> RmlShaderShader; 
    AssetReference<Shader> FiltersShader;
    AssetReference<Shader> BlurShader;
    AssetReference<Shader> BlitShader;

    GPUPipelineState *FontPipeline = nullptr;
    GPUPipelineState *ImagePipeline = nullptr;
    GPUPipelineState *ColorPipeline = nullptr;

    GPUPipelineState* SetStencilPipeline = nullptr;
    GPUPipelineState* IntersectStencilPipeline = nullptr;
    GPUPipelineState* SetStencilFSTPipeline = nullptr;

    GPUPipelineState* PassThroughPipeline = nullptr;
    GPUPipelineState* PassThroughPipelineBlend = nullptr;
    GPUPipelineState* BlurPipeline = nullptr;
    GPUBuffer* BlurWeightsBuffer = nullptr;
    GPUPipelineState* DropShadowPipeline = nullptr;
    GPUPipelineState* ColorMatrixPipeline = nullptr;
    GPUPipelineState* MaskImagePipeline = nullptr;

    GPUPipelineState *GradientPipeline = nullptr;
    GPUBuffer* GradientColorsBuffer = nullptr;
    GPUBuffer* GradientColorStopsBuffer = nullptr;

    GPUPipelineState* BlitPipeline = nullptr;


    Array<CompiledGeometry *> GeometryCache(2);
    Array<CompiledFilter *> FilterCache(2);
    Array<CompiledShader *> ShaderCache(2);

    Dictionary<GPUTexture *, AssetReference<Texture>> LoadedTextureAssets(32);
    Array<GPUTexture *> LoadedTextures(32);
    Array<GPUTexture *> AllocatedTextures(32);
    HashSet<GPUTexture *> FontTextures(32);
}

FlaxRenderInterface::FlaxRenderInterface() : RenderInterface()
{
    UseScissor = true;

    Guid basicShaderGuid;
    Guid::Parse(StringAnsiView(RMLUI_PLUGIN_BASIC_SHADER), basicShaderGuid);
    BasicShader = Content::Load<Shader>(basicShaderGuid);
    if (!BasicShader)
        LOG(Error, "RmlUi: Failed to load shader with id {0}", basicShaderGuid.ToString());
    BasicShader.Get()->OnReloading.Bind<FlaxRenderInterface, &FlaxRenderInterface::InvalidateShaders>(this);

    GUIShader = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/GUI"));
    if (!GUIShader)
        LOG(Error, "RmlUi: Failed to load shader with id {0}", basicShaderGuid.ToString());
    GUIShader.Get()->OnReloading.Bind<FlaxRenderInterface, &FlaxRenderInterface::InvalidateShaders>(this);

    Guid rmlShaderShaderGuid;
    Guid::Parse(StringAnsiView(RMLUI_PLUGIN_RMLSHADER_SHADER), rmlShaderShaderGuid);
    RmlShaderShader = Content::Load<Shader>(rmlShaderShaderGuid);
    if (!RmlShaderShader)
        LOG(Error, "RmlUi: Failed to load shader with id {0}", rmlShaderShaderGuid.ToString());
    RmlShaderShader.Get()->OnReloading.Bind<FlaxRenderInterface, &FlaxRenderInterface::InvalidateShaders>(this);

    Guid filtersShaderGuid;
    Guid::Parse(StringAnsiView(RMLUI_PLUGIN_FILTERS_SHADER), filtersShaderGuid);
    FiltersShader = Content::Load<Shader>(filtersShaderGuid);
    if (!FiltersShader)
        LOG(Error, "RmlUi: Failed to load shader with id {0}", filtersShaderGuid.ToString());
    FiltersShader.Get()->OnReloading.Bind<FlaxRenderInterface, &FlaxRenderInterface::InvalidateShaders>(this);

    Guid blurShaderGuid;
    Guid::Parse(StringAnsiView(RMLUI_PLUGIN_BLUR_SHADER), blurShaderGuid);
    BlurShader = Content::Load<Shader>(blurShaderGuid);
    if (!BlurShader)
        LOG(Error, "RmlUi: Failed to load shader with id {0}", blurShaderGuid.ToString());
    BlurShader.Get()->OnReloading.Bind<FlaxRenderInterface, &FlaxRenderInterface::InvalidateShaders>(this);

    Guid blitShaderGuid;
    Guid::Parse(StringAnsiView(RMLUI_PLUGIN_BLIT_SHADER), blitShaderGuid);
    BlitShader = Content::Load<Shader>(blitShaderGuid);
    if (!BlitShader)
        LOG(Error, "RmlUi: Failed to load shader with id {0}", blitShaderGuid.ToString());
    BlitShader.Get()->OnReloading.Bind<FlaxRenderInterface, &FlaxRenderInterface::InvalidateShaders>(this);

    // Handles with value of 0 are invalid, reserve the first slot in the arrays
    LoadedTextures.Add(nullptr);
    GeometryCache.Add(nullptr);
    FilterCache.Add(nullptr);
    ShaderCache.Add(nullptr);
}

FlaxRenderInterface::~FlaxRenderInterface()
{
    BasicShader.Get()->OnReloading.Unbind<FlaxRenderInterface, &FlaxRenderInterface::InvalidateShaders>(this);
    GUIShader.Get()->OnReloading.Unbind<FlaxRenderInterface, &FlaxRenderInterface::InvalidateShaders>(this);
    InvalidateShaders();
}

#pragma region BasicShaders
void FlaxRenderInterface::InvalidateShaders(Asset* obj)
{
    SAFE_DELETE_GPU_RESOURCE(FontPipeline);
    SAFE_DELETE_GPU_RESOURCE(ImagePipeline);
    SAFE_DELETE_GPU_RESOURCE(ColorPipeline);

    SAFE_DELETE_GPU_RESOURCE(SetStencilPipeline);
    SAFE_DELETE_GPU_RESOURCE(SetStencilFSTPipeline);
    SAFE_DELETE_GPU_RESOURCE(IntersectStencilPipeline);

    SAFE_DELETE_GPU_RESOURCE(BlitPipeline);

    SAFE_DELETE_GPU_RESOURCE(PassThroughPipeline);
    SAFE_DELETE_GPU_RESOURCE(PassThroughPipelineBlend);
    SAFE_DELETE_GPU_RESOURCE(BlurPipeline);
    SAFE_DELETE_GPU_RESOURCE(ColorMatrixPipeline);
    SAFE_DELETE_GPU_RESOURCE(DropShadowPipeline);
    SAFE_DELETE_GPU_RESOURCE(MaskImagePipeline);

    SAFE_DELETE_GPU_RESOURCE(GradientPipeline);
}
bool FlaxRenderInterface::InitShaders()
{
    if (!BasicShader->IsLoaded() && BasicShader->WaitForLoaded())
        return false;
    if (!GUIShader->IsLoaded() && GUIShader->WaitForLoaded())
        return false;
    if (!RmlShaderShader->IsLoaded() && RmlShaderShader->WaitForLoaded())
        return false;

    BlendingMode PremultipliedBlend = {
            false, // AlphaToCoverageEnable
            true, // BlendEnable
            BlendingMode::Blend::BlendFactor, // SrcBlend
            BlendingMode::Blend::InvSrcAlpha, // DestBlend
            BlendingMode::Operation::Add, // BlendOp
            BlendingMode::Blend::BlendFactor, // SrcBlendAlpha
            BlendingMode::Blend::InvSrcAlpha, // DestBlendAlpha
            BlendingMode::Operation::Add, // BlendOpAlpha
            BlendingMode::ColorWrite::All, // RenderTargetWriteMask
    };

    // Setup pipelines
    if (FontPipeline == nullptr || ImagePipeline == nullptr || ColorPipeline == nullptr)
    {
        GPUPipelineState::Description desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        desc.VS = BasicShader->GetShader()->GetVS("VS");
        desc.CullMode = CullMode::TwoSided;

        desc.DepthEnable = true;
        desc.DepthWriteEnable = false;
        desc.DepthClipEnable = false;
        desc.DepthFunc = ComparisonFunc::Always;
        desc.StencilEnable = true;
        desc.StencilFunc = ComparisonFunc::LessEqual;

        desc.BlendMode = BlendingMode::AlphaBlend;
        desc.PS = BasicShader->GetShader()->GetPS("PS_Font");
        FontPipeline = GPUDevice::Instance->CreatePipelineState();
        if (FontPipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create font pipeline state");
            return false;
        }

        desc.BlendMode = PremultipliedBlend;
        desc.PS = BasicShader->GetShader()->GetPS("PS_Image");
        ImagePipeline = GPUDevice::Instance->CreatePipelineState();
        if (ImagePipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create image pipeline state");
            return false;
        }

        desc.PS = BasicShader->GetShader()->GetPS("PS_Color");
        ColorPipeline = GPUDevice::Instance->CreatePipelineState();
        if (ColorPipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create color pipeline state");
            return false;
        }
    }
    if (SetStencilPipeline == nullptr || IntersectStencilPipeline == nullptr || SetStencilFSTPipeline == nullptr) {

        GPUPipelineState::Description desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        desc.CullMode = CullMode::TwoSided;
        desc.DepthWriteEnable = true;
        desc.DepthEnable = true;
        desc.DepthClipEnable = false;
        desc.DepthFunc = ComparisonFunc::Always;

        desc.BlendMode ={
            false, // AlphaToCoverageEnable
            false, // BlendEnable
            BlendingMode::Blend::One, // SrcBlend
            BlendingMode::Blend::InvSrcAlpha, // DestBlend
            BlendingMode::Operation::Add, // BlendOp
            BlendingMode::Blend::One, // SrcBlendAlpha
            BlendingMode::Blend::InvSrcAlpha, // DestBlendAlpha
            BlendingMode::Operation::Add, // BlendOpAlpha
            BlendingMode::ColorWrite::None, // RenderTargetWriteMask
        };

        desc.StencilEnable = true;
        //desc.StencilReadMask =
        //desc.StencilWriteMask
        desc.VS = BasicShader->GetShader()->GetVS("VS");
        desc.PS = BasicShader->GetShader()->GetPS("PS_Color");

        desc.StencilFailOp = StencilOperation::Zero;
        desc.StencilDepthFailOp = StencilOperation::Keep;
        desc.StencilPassOp = StencilOperation::Keep;
        desc.StencilFunc = ComparisonFunc::Equal;

        IntersectStencilPipeline = GPUDevice::Instance->CreatePipelineState();
        if (IntersectStencilPipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create depth pipeline state");
            return false;
        }

        desc.StencilFailOp = StencilOperation::Keep;
        desc.StencilDepthFailOp = StencilOperation::Keep;
        desc.StencilPassOp = StencilOperation::Replace;
        desc.StencilFunc = ComparisonFunc::Always;

        SetStencilPipeline = GPUDevice::Instance->CreatePipelineState();
        if (SetStencilPipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create depth pipeline state");
            return false;
        }

        desc.PS = GPUPipelineState::Description::DefaultFullscreenTriangle.PS;
        desc.VS = GPUPipelineState::Description::DefaultFullscreenTriangle.VS;
        SetStencilFSTPipeline = GPUDevice::Instance->CreatePipelineState();
        if (SetStencilFSTPipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create depth pipeline state");
            return false;
        }
    }

    if (PassThroughPipeline == nullptr ||  DropShadowPipeline == nullptr || ColorMatrixPipeline == nullptr || MaskImagePipeline == nullptr || PassThroughPipelineBlend==nullptr)
    {
        bool useDepth = false;
        GPUPipelineState::Description desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        desc.VS = FiltersShader->GetShader()->GetVS("VS");
        desc.CullMode = CullMode::TwoSided;

        desc.DepthEnable = true;
        desc.DepthWriteEnable = false;
        desc.DepthClipEnable = false;
        desc.DepthFunc = ComparisonFunc::Always;
        desc.StencilEnable = true;
        desc.StencilFunc = ComparisonFunc::LessEqual;


        desc.BlendMode = PremultipliedBlend;
        desc.PS = FiltersShader->GetShader()->GetPS("PS_PassThrough");
        PassThroughPipelineBlend = GPUDevice::Instance->CreatePipelineState();
        if (PassThroughPipelineBlend->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create color pipeline state");
            return false;
        }
        
        //All the pipelines below do not use blending
        desc.BlendMode = BlendingMode::Opaque;
        desc.PS = FiltersShader->GetShader()->GetPS("PS_PassThrough");
        PassThroughPipeline = GPUDevice::Instance->CreatePipelineState();
        if (PassThroughPipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create color pipeline state");
            return false;
        }

        desc.PS = FiltersShader->GetShader()->GetPS("PS_DropShadow");
        DropShadowPipeline = GPUDevice::Instance->CreatePipelineState();
        if (DropShadowPipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create color pipeline state");
            return false;
        }

        desc.PS = FiltersShader->GetShader()->GetPS("PS_ColorMatrix");
        ColorMatrixPipeline = GPUDevice::Instance->CreatePipelineState();
        if (ColorMatrixPipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create color pipeline state");
            return false;
        }

        desc.PS = FiltersShader->GetShader()->GetPS("PS_MaskImage");
        MaskImagePipeline = GPUDevice::Instance->CreatePipelineState();
        if (MaskImagePipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create color pipeline state");
            return false;
        }
    }
    if (BlurPipeline == nullptr) {
        bool useDepth = false;
        GPUPipelineState::Description desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        desc.DepthEnable = desc.DepthWriteEnable = useDepth;
        desc.DepthWriteEnable = false;
        desc.DepthClipEnable = false;
        desc.VS = BlurShader->GetShader()->GetVS("VS");
        desc.CullMode = CullMode::TwoSided;

        desc.BlendMode = BlendingMode::Opaque;
        desc.PS = BlurShader->GetShader()->GetPS("PS_Blur");
        BlurPipeline = GPUDevice::Instance->CreatePipelineState();
        if (BlurPipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create color pipeline state");
            return false;
        }
    }
    if (BlurWeightsBuffer == nullptr)
    {
        BlurWeightsBuffer = GPUDevice::Instance->CreateBuffer(TEXT("BlurWeightsBuffer Buffer"));
        BlurWeightsBuffer->Init(GPUBufferDescription::Structured(BLUR_NUM_WEIGHTS, sizeof(float)));
    }

    if (GradientPipeline == nullptr)
    {
        bool useDepth = false;
        GPUPipelineState::Description desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        desc.DepthEnable = desc.DepthWriteEnable = useDepth;
        desc.DepthWriteEnable = false;
        desc.DepthClipEnable = false;
        desc.VS = RmlShaderShader->GetShader()->GetVS("VS");
        desc.CullMode = CullMode::TwoSided;

        desc.BlendMode = PremultipliedBlend;
        desc.PS = RmlShaderShader->GetShader()->GetPS("PS_Gradient");
        GradientPipeline = GPUDevice::Instance->CreatePipelineState();
        if (GradientPipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create color pipeline state");
            return false;
        }
    }
    if (BlitPipeline == nullptr) {
        bool useDepth = false;
        GPUPipelineState::Description desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        desc.DepthEnable = desc.DepthWriteEnable = useDepth;
        desc.DepthWriteEnable = false;
        desc.DepthClipEnable = false;
        desc.VS = BlitShader->GetShader()->GetVS("VS");
        desc.CullMode = CullMode::TwoSided;

        desc.BlendMode = BlendingMode::Opaque;
        desc.PS = BlitShader->GetShader()->GetPS("PS_Main");
        BlitPipeline = GPUDevice::Instance->CreatePipelineState();
        if (BlitPipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create color pipeline state");
            return false;
        }
    }

    if (GradientColorStopsBuffer == nullptr)
        GradientColorStopsBuffer = GPUDevice::Instance->CreateBuffer(TEXT("GradientColorStops Buffer"));
    if (GradientColorsBuffer == nullptr)
        GradientColorsBuffer = GPUDevice::Instance->CreateBuffer(TEXT("GradientColors Buffer"));

    return true;
}
#pragma endregion

#pragma region Geometry
CompiledGeometry *ReserveGeometry(Rml::CompiledGeometryHandle &geometryHandle)
{
    // Cache geometry structures in order to reduce allocations and recreating buffers
    for (int i = 1; i < GeometryCache.Count(); i++)
    {
        if (GeometryCache[i]->reserved)
            continue;

        GeometryCache[i]->reserved = true;
        geometryHandle = Rml::CompiledGeometryHandle(i);
        return GeometryCache[i];
    }

    CompiledGeometry *geometry = New<CompiledGeometry>();
    geometryHandle = Rml::CompiledGeometryHandle(GeometryCache.Count());
    GeometryCache.Add(geometry);
    return geometry;
}

void FlaxRenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle handle)
{
    if ((int)handle == 0)
        return;
    GeometryCache[(int)handle]->Dispose();
}
Rml::CompiledGeometryHandle FlaxRenderInterface::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
    Rml::CompiledGeometryHandle geometryHandle;
    CompiledGeometry *compiledGeometry = ReserveGeometry(geometryHandle);
    CompileGeometry(compiledGeometry, vertices.begin(), (int)vertices.size(), indices.begin(), (int)indices.size());
    return (Rml::CompiledGeometryHandle)geometryHandle;
}

void FlaxRenderInterface::CompileGeometry(CompiledGeometry *compiledGeometry, const Rml::Vertex *vertices, int num_vertices, const int *indices, int num_indices)
{
    PROFILE_GPU_CPU("RmlUi.CompileGeometry");

    const Rectangle defaultBounds(CurrentViewport.Location, CurrentViewport.Size);
    const RotatedRectangle defaultMask(defaultBounds);

    compiledGeometry->vertexBuffer.Data.EnsureCapacity((int32)(num_vertices * sizeof(BasicVertex)));
    compiledGeometry->indexBuffer.Data.EnsureCapacity((int32)(num_indices * sizeof(uint32)));

    for (int i = 0; i < num_vertices; i++)
    {
        BasicVertex vb0;
        vb0.Position = (Float2)vertices[i].position;
        vb0.TexCoord = (Float2)vertices[i].tex_coord;
        vb0.Color = Color(Color32(vertices[i].colour.red, vertices[i].colour.green, vertices[i].colour.blue, vertices[i].colour.alpha));
        vb0.ClipOrigin = defaultMask.TopLeft;
        vb0.ClipExtents = Float4(defaultMask.ExtentX, defaultMask.ExtentY);

        compiledGeometry->vertexBuffer.Write(vb0);
    }
    for (int i = 0; i < num_indices; i++)
        compiledGeometry->indexBuffer.Write((uint32)indices[i]);
}

void FlaxRenderInterface::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture)
{
    CompiledGeometry *compiledGeometry = GeometryCache[(int)geometry];
    if (compiledGeometry == nullptr)
        return;

    RenderCompiledGeometry(compiledGeometry, translation, texture);
}

void FlaxRenderInterface::RenderCompiledGeometry(CompiledGeometry *compiledGeometry, const Rml::Vector2f &translation, Rml::TextureHandle texture_handle)
{
    PROFILE_GPU_CPU("RmlUi.RenderCompiledGeometry");

    if (!InitShaders())
        return;

    auto texture = LoadedTextures.At((int32)texture_handle);

    GPUPipelineState *pipeline;
    if (texture == nullptr)
        pipeline = ColorPipeline;
    else if (FontTextures.Contains(texture))
        pipeline = FontPipeline;
    else
        pipeline = ImagePipeline;

    CurrentGPUContext->SetBlendFactor(Float4(1, 1, 1, 1));

    RenderGeometryWithPipeline(compiledGeometry, translation, texture, pipeline);
}

void FlaxRenderInterface::RenderGeometryWithPipeline(CompiledGeometry* compiledGeometry, const Rml::Vector2f& translation, GPUTexture* texture, GPUPipelineState* pipeline)
{
    PROFILE_GPU("RmlUi.RenderGeometryWithPipeline");

    compiledGeometry->vertexBuffer.Flush(CurrentGPUContext);
    compiledGeometry->indexBuffer.Flush(CurrentGPUContext);

    GPUConstantBuffer* constantBuffer = BasicShader->GetShader()->GetCB(0);
    GPUBuffer* vb = compiledGeometry->vertexBuffer.GetBuffer();
    GPUBuffer* ib = compiledGeometry->indexBuffer.GetBuffer();

    if (vb == nullptr || ib == nullptr)
        return;

    SetupRenderTarget(render_layers.GetTopLayer());
    CurrentGPUContext->FlushState();

    // Update constant buffer data
    CustomData data;
    Matrix::Transpose(ViewProjection, data.ViewProjection);
    Matrix::Transpose(CurrentTransform, data.Model);
    data.Offset = (Float2)translation;
    CurrentGPUContext->UpdateCB(constantBuffer, &data);

    // State and bindings
    CurrentGPUContext->BindCB(0, constantBuffer);
    if (texture != nullptr)
        CurrentGPUContext->BindSR(0, texture);
    CurrentGPUContext->BindVB(Span<GPUBuffer*>(&vb, 1));
    CurrentGPUContext->BindIB(ib);
    CurrentGPUContext->SetState(pipeline);

    CurrentGPUContext->DrawIndexed(compiledGeometry->indexBuffer.Data.Count() / sizeof(uint32));
}
#pragma endregion

void FlaxRenderInterface::HookGenerateTexture(Rml::TextureHandle textureHandle)
{
    _generateTextureOverride = textureHandle;
}

void FlaxRenderInterface::EnableScissorRegion(bool enable)
{
    PROFILE_GPU("RmlUi.EnableScissorRegion");
    UseScissor = enable;
    CurrentGPUContext->SetScissor(enable ? CurrentScissor : CurrentViewport.GetBounds());
}

void FlaxRenderInterface::SetScissorRegion(Rml::Rectanglei region)
{
    PROFILE_GPU("RmlUi.SetScissorRegion");
    // LOG(Info, "Set Scissor: {0} {1} {2} {3}", CurrentScissor.GetX(), CurrentScissor.GetY(), CurrentScissor.GetWidth(), CurrentScissor.GetHeight());
    SetScissor(Rectangle((float)region.Position().x, (float)region.Position().y, (float)region.Size().x, (float)region.Size().y));
}
void FlaxRenderInterface::SetViewport(Viewport view)
{
    if (CurrentViewport != view) {
        CurrentViewport = view;
        CurrentGPUContext->SetViewport(CurrentViewport);
        EnableScissorRegion(UseScissor);
    }
}
void FlaxRenderInterface::SetViewport(int width, int height)
{
    SetViewport(Viewport(0, 0, width, height));
}

void FlaxRenderInterface::SetScissor(Rectangle scissor)
{
    if (scissor != CurrentScissor) {
        CurrentScissor = scissor;
        EnableScissorRegion(true);
    }
}

void FlaxRenderInterface::EnableClipMask(bool enable)
{
    UseStencil = enable;
    if (enable) {
        PROFILE_GPU("RmlUi.EnableClipMask(100)");
        CurrentGPUContext->SetStencilRef(100);
    }else {
        PROFILE_GPU("RmlUi.EnableClipMask(0)");
        CurrentGPUContext->SetStencilRef(0);
    }
}

void FlaxRenderInterface::RenderToClipMask(Rml::ClipMaskOperation mask_operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation)
{
    PROFILE_GPU("RmlUi.RenderToClipMask");

    if (!InitShaders())
        return;

    const bool clear_stencil = (mask_operation == Rml::ClipMaskOperation::Set || mask_operation == Rml::ClipMaskOperation::SetInverse);
    if (clear_stencil)
    {
        PROFILE_GPU("RmlUi.RenderToClipMask.Clear");

        switch (mask_operation)
        {
        case Rml::ClipMaskOperation::Set:
        {
            PROFILE_GPU("RmlUi.SetStencilRef(0)");
            CurrentGPUContext->SetStencilRef(0);
            break;
        }
        case Rml::ClipMaskOperation::SetInverse:
        {
            PROFILE_GPU("RmlUi.SetStencilRef(1)");
            CurrentGPUContext->SetStencilRef(100);
            break;
        }
        }

        CurrentGPUContext->SetBlendFactor(Float4(0.0f));

        CurrentGPUContext->SetState(SetStencilFSTPipeline);

        SetupRenderTarget(render_layers.GetTopLayer());
        CurrentGPUContext->FlushState();

        CurrentGPUContext->DrawFullscreenTriangle();
        // @performance Increment the reference value instead of clearing each time.
        //glClear(GL_STENCIL_BUFFER_BIT);
    }

    switch (mask_operation)
    {
    case Rml::ClipMaskOperation::Intersect:
    case Rml::ClipMaskOperation::Set:
    {
        PROFILE_GPU("RmlUi.SetStencilRef(1)");
        CurrentGPUContext->SetStencilRef(100);
        break;
    }
    case Rml::ClipMaskOperation::SetInverse:
    {
        PROFILE_GPU("RmlUi.SetStencilRef(0)");
        CurrentGPUContext->SetStencilRef(0);
        break;
    }
    }

    GPUPipelineState* pipeline;
    switch (mask_operation)
    {
    case Rml::ClipMaskOperation::Set:
    case Rml::ClipMaskOperation::SetInverse:
    {
        PROFILE_GPU("RmlUi.SetPipeline(SetStencilPipeline)");
        pipeline = SetStencilPipeline;
        break;
    }
    case Rml::ClipMaskOperation::Intersect:
    {
        PROFILE_GPU("RmlUi.SetPipeline(IntersectStencilPipeline)");
        pipeline = IntersectStencilPipeline;
        break;
    }
    }

    CompiledGeometry* compiledGeometry = GeometryCache[(int)geometry];
    if (compiledGeometry == nullptr)
        return;

    CurrentGPUContext->SetBlendFactor(Float4(0.0f));
    CurrentGPUContext->FlushState();

    RenderGeometryWithPipeline(compiledGeometry, translation, nullptr, pipeline);

    EnableClipMask(UseStencil);
    CurrentGPUContext->SetBlendFactor(Float4(1));
}

#pragma region Texture
Rml::TextureHandle FlaxRenderInterface::LoadTexture(Rml::Vector2i &texture_dimensions, const Rml::String &source)
{
    PROFILE_GPU("RmlUi.LoadTexture");
    String contentPath = String(StringUtils::GetPathWithoutExtension(String(source.c_str()))) + ASSET_FILES_EXTENSION_WITH_DOT;
    AssetReference<Texture> textureAsset = Content::Load<Texture>(contentPath);
    if (textureAsset == nullptr)
        return false;

    GPUTexture *texture = textureAsset.Get()->GetTexture();
    LoadedTextureAssets.Add(texture, textureAsset);

    Float2 textureSize = textureAsset->Size();
    texture_dimensions.x = (int)textureSize.X;
    texture_dimensions.y = (int)textureSize.Y;

    return RegisterTexture(texture);
}

Rml::TextureHandle FlaxRenderInterface::GenerateTexture(Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions)
{
    LOG(Info, "GenerateTexture");

    Rml::TextureHandle texture_handle;
    if (source_data.size() == 0 && _generateTextureOverride != 0)
    {
        // HACK: Return the previously generated texture handle here instead for font texture atlases...
        texture_handle = _generateTextureOverride;
        _generateTextureOverride = 0;
        return texture_handle;
    }

    GPUTextureDescription desc = GPUTextureDescription::New2D(source_dimensions.x, source_dimensions.y, PixelFormat::B8G8R8A8_UNorm);
    GPUTexture *texture = GPUDevice::Instance->CreateTexture();
    if (texture->Init(desc))
        return (Rml::TextureHandle) nullptr;

    texture_handle = RegisterTexture(texture);
    AllocatedTextures.Add(texture);

    if (source_data.size() != 0) {
        BytesContainer data(source_data.data(), source_dimensions.x * source_dimensions.y * 4);
        auto task = texture->UploadMipMapAsync(data, 0, true);
        if (task)
            task->Start();
    }

    return texture_handle;
}

void FlaxRenderInterface::ReleaseTexture(Rml::TextureHandle texture_handle)
{
    GPUTexture *texture = LoadedTextures.At((int)texture_handle);
    AssetReference<Texture> textureAssetRef;
    if (LoadedTextureAssets.TryGet(texture, textureAssetRef))
    {
        textureAssetRef->DeleteObject();
        LoadedTextureAssets.Remove(texture);
    }
}
#pragma endregion

void FlaxRenderInterface::SetTransform(const Rml::Matrix4f *transform_)
{
    PROFILE_GPU("RmlUi.SetTransform");
    // We assume the library is not built with row-major matrices enabled
    CurrentTransform = transform_ != nullptr ? *(const Matrix *)transform_->data() : Matrix::Identity;
}

#pragma region Filters

static void SigmaToParameters(const float desired_sigma, int& out_pass_level, float& out_sigma)
{
    constexpr int max_num_passes = 10;
    static_assert(max_num_passes < 31, "");
    constexpr float max_single_pass_sigma = 3.0f;
    out_pass_level = Rml::Math::Clamp(Rml::Math::Log2(int(desired_sigma * (2.f / max_single_pass_sigma))), 0, max_num_passes);
    out_sigma = Rml::Math::Clamp(desired_sigma / float(1 << out_pass_level), 0.0f, max_single_pass_sigma);
}
void FlaxRenderInterface::BlitTexturesUV(GPUTextureView* sourceView, Float4 source, GPUTextureView* destinationView, Float4 destination) {
    PROFILE_GPU("RmlUi.BlitTextures");
    CurrentGPUContext->SetState(BlitPipeline);

    GPUConstantBuffer* constantBuffer = BlitShader->GetShader()->GetCB(0);

    // Update constant buffer data
    BlitData data;
    data.sourceRect = source;
    data.targetRect = destination;

    CurrentGPUContext->ResetCB();
    CurrentGPUContext->ResetRenderTarget();
    CurrentGPUContext->ResetUA();
    CurrentGPUContext->ResetSR();

    CurrentGPUContext->UpdateCB(constantBuffer, &data);
    CurrentGPUContext->SetRenderTarget(destinationView);
    CurrentGPUContext->BindSR(0, sourceView);
    CurrentGPUContext->BindCB(0, constantBuffer);
    CurrentGPUContext->FlushState();

    CurrentGPUContext->DrawFullscreenTriangle();
}
void FlaxRenderInterface::BlitTextures(FramebufferData sourceData, Rectangle source, FramebufferData destinationData, Rectangle destination) {
    auto sourceRect = Float4(source.GetUpperLeft().X / sourceData.width, source.GetUpperLeft().Y / sourceData.height, source.GetBottomRight().X / sourceData.width, source.GetBottomRight().Y / sourceData.height);
    auto targetRect = Float4(destination.GetUpperLeft().X / destinationData.width, destination.GetUpperLeft().Y / destinationData.height, destination.GetBottomRight().X / destinationData.width, destination.GetBottomRight().Y / destinationData.height);

    CurrentGPUContext->SetViewport(Viewport(0, 0, destinationData.width, destinationData.height));
    CurrentGPUContext->SetScissor(destination);

    BlitTexturesUV(sourceData, sourceRect, destinationData, targetRect);

    CurrentGPUContext->SetViewport(CurrentViewport);
    EnableScissorRegion(UseScissor);
}
void FlaxRenderInterface::BlitTextures(FramebufferData sourceData, FramebufferData destinationData) {
    CurrentGPUContext->SetViewportAndScissors(Viewport(0, 0, destinationData.width, destinationData.height));

    BlitTexturesUV(sourceData, Float4(0, 0, 1, 1), destinationData, Float4(0, 0, 1, 1));

    CurrentGPUContext->SetViewport(CurrentViewport);
    EnableScissorRegion(UseScissor);
}
void FlaxRenderInterface::RenderBlur(float sigma, const FramebufferData& source_destination, const FramebufferData& temp, const Rectangle window_flipped) {
    RMLUI_ASSERT(&source_destination != &temp && source_destination.width == temp.width && source_destination.height == temp.height);
    RMLUI_ASSERT(window_flipped.Valid());
    PROFILE_GPU("RmlUi.RenderBlur");

    int pass_level = 0;
    CurrentGPUContext->ResetCB();
    SigmaToParameters(sigma, pass_level, sigma);

    const Rectangle original_scissor = CurrentScissor;
    const bool original_use_scissor = UseScissor;

    // Begin by downscaling so that the blur pass can be done at a reduced resolution for large sigma.
    Rectangle scissor = window_flipped;

    CurrentGPUContext->SetState(PassThroughPipeline);

    GPUConstantBuffer* pConstantBuffer = FiltersShader->GetShader()->GetCB(0);

    SetScissor(scissor);

    // Downscale by iterative half-scaling with bilinear filtering, to reduce aliasing.
    SetViewport(source_destination.width / 2, source_destination.height / 2);
    CurrentGPUContext->FlushState();

    // Scale UVs if we have even dimensions, such that texture fetches align perfectly between texels, thereby producing a 50% blend of
    // neighboring texels.
    const Float2 uv_scaling = { (source_destination.width % 2 == 1) ? (1.f - 1.f / float(source_destination.width)) : 1.f,
        (source_destination.height % 2 == 1) ? (1.f - 1.f / float(source_destination.height)) : 1.f };

    pass_level += 1;

    for (int i = 0; i < pass_level; i++)
    {
        PROFILE_GPU("RmlUi.RenderBlur.DownScale");
        const auto TopLeft = (scissor.GetUpperLeft() + Float2(1)) / 2.f;
        scissor = Rectangle(TopLeft, scissor.GetBottomRight() - TopLeft);
        scissor = Rectangle(TopLeft, Math::Max(scissor.GetBottomRight() / 2.0f, scissor.GetUpperLeft()) - TopLeft);
        const bool from_source = (i % 2 == 0);

        FilterCustomData pData;
        pData.UVScale = uv_scaling;
        Matrix::Transpose(ViewProjection, pData.ViewProjection);
        Matrix::Transpose(CurrentTransform, pData.Model);

        CurrentGPUContext->ResetRenderTarget();
        CurrentGPUContext->Clear((from_source ? temp : source_destination).framebuffer, Color().Transparent);

        SetScissor(scissor);
        CurrentGPUContext->SetRenderTarget((from_source ? temp : source_destination).framebuffer);
        CurrentGPUContext->BindSR(0, from_source ? source_destination.framebuffer : temp.framebuffer);
        CurrentGPUContext->UpdateCB(pConstantBuffer, &pData);
        CurrentGPUContext->BindCB(0, pConstantBuffer);
        CurrentGPUContext->FlushState();

        CurrentGPUContext->DrawFullscreenTriangle();
        //DrawFullscreenQuad({}, uv_scaling);
    }


    SetViewport(Viewport(0, 0, source_destination.width, source_destination.height));

    SetScissor(scissor);
    // Ensure texture data end up in the temp buffer. Depending on the last downscaling, we might need to move it from the source_destination buffer.
    const bool transfer_to_temp_buffer = (pass_level % 2 == 0);
    if (transfer_to_temp_buffer)
    {
        FilterCustomData pData;
        Matrix::Transpose(ViewProjection, pData.ViewProjection);
        Matrix::Transpose(CurrentTransform, pData.Model);

        CurrentGPUContext->ResetRenderTarget();
        CurrentGPUContext->Clear(temp.framebuffer, Color().Transparent);
        CurrentGPUContext->ResetUA();
        CurrentGPUContext->ResetSR();

        CurrentGPUContext->SetRenderTarget(temp.framebuffer);
        CurrentGPUContext->UpdateCB(pConstantBuffer, &pData);
        CurrentGPUContext->BindCB(0, pConstantBuffer);
        CurrentGPUContext->BindSR(0, source_destination.framebuffer);
        CurrentGPUContext->FlushState();

        CurrentGPUContext->DrawFullscreenTriangle();
    }

    // Set up uniforms.
    CurrentGPUContext->SetState(BlurPipeline);

    // Update constant buffer data
    BlurCustomData data;

    constexpr int num_weights = BLUR_NUM_WEIGHTS;
    float weights[num_weights];
    float normalization = 0.0f;
    for (int i = 0; i < num_weights; i++)
    {
        if (Rml::Math::Absolute(sigma) < 0.1f)
            weights[i] = float(i == 0);
        else
            weights[i] = Rml::Math::Exp(-float(i * i) / (2.0f * sigma * sigma)) / (Rml::Math::SquareRoot(2.f * Rml::Math::RMLUI_PI) * sigma);

        normalization += (i == 0 ? 1.f : 2.0f) * weights[i];
    }
    for (int i = 0; i < num_weights; i++)
        weights[i] /= normalization;

    data._texCoordMin = (scissor.GetUpperLeft() + Float2(0.5)) / Float2(source_destination.width, source_destination.height);
    data._texCoordMax = (scissor.GetBottomRight() - Float2(0.5)) / Float2(source_destination.width, source_destination.height);

    auto SetTexelOffset = [&data](Float2 blur_direction, int texture_dimension) {
        const Float2 texel_offset = blur_direction * (1.0f / float(texture_dimension));
        data._texelOffset = texel_offset;
        };


    // Blur render pass - vertical.
    SetScissor(scissor);
    CurrentGPUContext->ResetRenderTarget();
    CurrentGPUContext->ResetCB();
    CurrentGPUContext->ResetUA();
    CurrentGPUContext->ResetSR();

    CurrentGPUContext->SetRenderTarget(source_destination.framebuffer);
    CurrentGPUContext->BindSR(0, temp.framebuffer);
    CurrentGPUContext->BindSR(1, BlurWeightsBuffer->View());

    GPUConstantBuffer* constantBuffer = BlurShader->GetShader()->GetCB(0);

    SetTexelOffset({ 0.f, 1.f }, temp.height);
    CurrentGPUContext->BindCB(0, constantBuffer);
    CurrentGPUContext->UpdateCB(constantBuffer, &data);

    CurrentGPUContext->UpdateBuffer(BlurWeightsBuffer, weights, num_weights * sizeof(float));

    CurrentGPUContext->FlushState();
    CurrentGPUContext->DrawFullscreenTriangle();

    // Add a 1px transparent border around the blur region by first clearing with a padded scissor. This helps prevent
    // artifacts when upscaling the blur result in the later step. On Intel and AMD, we have observed that during
    // blitting with linear filtering, pixels outside the 'src' region can be blended into the output. On the other
    // hand, it looks like Nvidia clamps the pixels to the source edge, which is what we really want. Regardless, we
    // work around the issue with this extra step.
    /*SetScissor(scissor.Extend(1), true);
    glClear(GL_COLOR_BUFFER_BIT);*/
    SetScissor(scissor.MakeExpanded(1));
    CurrentGPUContext->Clear(temp.framebuffer, Color().Transparent);


    // Blur render pass - horizontal.
    CurrentGPUContext->ResetRenderTarget();
    CurrentGPUContext->ResetCB();
    CurrentGPUContext->ResetUA();
    CurrentGPUContext->ResetSR();

    SetScissor(scissor);

    CurrentGPUContext->SetRenderTarget(temp.framebuffer);
    CurrentGPUContext->BindSR(0, source_destination.framebuffer);
    CurrentGPUContext->BindSR(1, BlurWeightsBuffer->View());

    SetTexelOffset({ 1.f, 0.f }, source_destination.width);
    CurrentGPUContext->BindCB(0, constantBuffer);
    CurrentGPUContext->UpdateCB(constantBuffer, &data);
    CurrentGPUContext->FlushState();
    CurrentGPUContext->DrawFullscreenTriangle();

    // Blit the blurred image to the scissor region with upscaling.
    SetScissor(window_flipped);
    CurrentGPUContext->FlushState();
    //glBindFramebuffer(GL_READ_FRAMEBUFFER, temp.framebuffer);
    //glBindFramebuffer(GL_DRAW_FRAMEBUFFER, source_destination.framebuffer);


    const Int2 src_min = scissor.GetUpperLeft();
    const Int2 src_max = scissor.GetBottomRight();
    const Int2 dst_min = window_flipped.GetUpperLeft();
    const Int2 dst_max = window_flipped.GetBottomRight();
    BlitTextures(temp, scissor, source_destination, window_flipped);
    //glBlitFramebuffer(src_min.x, src_min.y, src_max.x, src_max.y, dst_min.x, dst_min.y, dst_max.x, dst_max.y, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    // The above upscale blit might be jittery at low resolutions (large pass levels). This is especially noticeable when moving an element with
    // backdrop blur around or when trying to click/hover an element within a blurred region since it may be rendered at an offset. For more stable
    // and accurate rendering we next upscale the blur image by an exact power-of-two. However, this may not fill the edges completely so we need to
    // do the above first. Note that this strategy may sometimes result in visible seams. Alternatively, we could try to enlarge the window to the
    // next power-of-two size and then downsample and blur that.
    const Int2 target_min = src_min * (1 << pass_level);
    const Int2 target_max = src_max * (1 << pass_level);
    if (target_min != dst_min || target_max != dst_max)
    {
        BlitTextures(temp, scissor, source_destination, Rectangle(target_min, target_max - target_min));
        //glBlitFramebuffer(src_min.x, src_min.y, src_max.x, src_max.y, target_min.x, target_min.y, target_max.x, target_max.y, GL_COLOR_BUFFER_BIT,            GL_LINEAR);
    }

    // Restore render state.
    SetScissor(original_scissor);
    EnableScissorRegion(original_use_scissor);
}

CompiledFilter* ReserveFilter(Rml::CompiledFilterHandle& filterHandle)
{
    // Cache geometry structures in order to reduce allocations and recreating buffers
    for (int i = 1; i < FilterCache.Count(); i++)
    {
        if (FilterCache[i]->reserved)
            continue;

        FilterCache[i]->reserved = true;
        filterHandle = Rml::CompiledFilterHandle(i);
        return FilterCache[i];
    }

    CompiledFilter* geometry = New<CompiledFilter>();
    filterHandle = Rml::CompiledFilterHandle(FilterCache.Count());
    FilterCache.Add(geometry);
    return geometry;
}
Rml::CompiledFilterHandle FlaxRenderInterface::CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters)
{
    PROFILE_GPU("RmlUi.CompileFilter");

    Rml::CompiledFilterHandle filterHandle;
    CompiledFilter* filter = ReserveFilter(filterHandle);

    if (name == "opacity")
    {
        filter->type = FilterType::Passthrough;
        filter->blend_factor = Rml::Get(parameters, "value", 1.0f);
    }
    else if (name == "blur")
    {
        filter->type = FilterType::Blur;
        filter->sigma = 0.5f * Rml::Get(parameters, "sigma", 1.0f);
    }
    else if (name == "drop-shadow")
    {
        filter->type = FilterType::DropShadow;
        filter->sigma = Rml::Get(parameters, "sigma", 0.f);
        auto color = Rml::Get(parameters, "color", Rml::Colourb()).ToPremultiplied();
        filter->color = Color(Color32(color.red, color.green, color.blue, color.alpha));
        filter->offset = (Float2)Rml::Get(parameters, "offset", Rml::Vector2f(0.f));
    }
    else if (name == "brightness")
    {
        filter->type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        filter->color_matrix = *(const Matrix*)Rml::Matrix4f::Diag(value, value, value, 1.f).data();
    }
    else if (name == "contrast")
    {
        filter->type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float grayness = 0.5f - 0.5f * value;
        filter->color_matrix = *(const Matrix*)Rml::Matrix4f::Diag(value, value, value, 1.f).data();
        filter->color_matrix.SetColumn4(Float4(grayness, grayness, grayness, 1.f));
    }
    else if (name == "invert")
    {
        filter->type = FilterType::ColorMatrix;
        const float value = Rml::Math::Clamp(Rml::Get(parameters, "value", 1.0f), 0.f, 1.f);
        const float inverted = 1.f - 2.f * value;
        filter->color_matrix = *(const Matrix*)Rml::Matrix4f::Diag(inverted, inverted, inverted, 1.f).data();
        filter->color_matrix.SetColumn4(Float4(value, value, value, 1.f));
    }
    else if (name == "grayscale")
    {
        filter->type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float rev_value = 1.f - value;
        const Rml::Vector3f gray = value * Rml::Vector3f(0.2126f, 0.7152f, 0.0722f);
        // clang-format off
        filter->color_matrix = Matrix(
            gray.x + rev_value, gray.y,             gray.z,             0.f,
            gray.x,             gray.y + rev_value, gray.z,             0.f,
            gray.x,             gray.y,             gray.z + rev_value, 0.f,
            0.f,                0.f,                0.f,                1.f
        );
        // clang-format on
    }
    else if (name == "sepia")
    {
        filter->type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float rev_value = 1.f - value;
        const Rml::Vector3f r_mix = value * Rml::Vector3f(0.393f, 0.769f, 0.189f);
        const Rml::Vector3f g_mix = value * Rml::Vector3f(0.349f, 0.686f, 0.168f);
        const Rml::Vector3f b_mix = value * Rml::Vector3f(0.272f, 0.534f, 0.131f);
        // clang-format off
        filter->color_matrix = Matrix(
            r_mix.x + rev_value, r_mix.y,             r_mix.z,             0.f,
            g_mix.x,             g_mix.y + rev_value, g_mix.z,             0.f,
            b_mix.x,             b_mix.y,             b_mix.z + rev_value, 0.f,
            0.f,                 0.f,                 0.f,                 1.f
        );
        // clang-format on
    }
    else if (name == "hue-rotate")
    {
        // Hue-rotation and saturation values based on: https://www.w3.org/TR/filter-effects-1/#attr-valuedef-type-huerotate
        filter->type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float s = Rml::Math::Sin(value);
        const float c = Rml::Math::Cos(value);
        // clang-format off
        filter->color_matrix = Matrix(
            0.213f + 0.787f * c - 0.213f * s,  0.715f - 0.715f * c - 0.715f * s,  0.072f - 0.072f * c + 0.928f * s,  0.f,
            0.213f - 0.213f * c + 0.143f * s,  0.715f + 0.285f * c + 0.140f * s,  0.072f - 0.072f * c - 0.283f * s,  0.f,
            0.213f - 0.213f * c - 0.787f * s,  0.715f - 0.715f * c + 0.715f * s,  0.072f + 0.928f * c + 0.072f * s,  0.f,
            0.f,                               0.f,                               0.f,                               1.f
        );
        // clang-format on
    }
    else if (name == "saturate")
    {
        filter->type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        // clang-format off
        filter->color_matrix = Matrix(
            0.213f + 0.787f * value,  0.715f - 0.715f * value,  0.072f - 0.072f * value,  0.f,
            0.213f - 0.213f * value,  0.715f + 0.285f * value,  0.072f - 0.072f * value,  0.f,
            0.213f - 0.213f * value,  0.715f - 0.715f * value,  0.072f + 0.928f * value,  0.f,
            0.f,                      0.f,                      0.f,                      1.f
        );
        // clang-format on
    }

    if (filter->type != FilterType::Invalid)
        return filterHandle;

    Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported filter type '%s'.", name.c_str());
    return {};
}
void FlaxRenderInterface::RenderFilters(Rml::Span<const Rml::CompiledFilterHandle> filter_handles)
{
    PROFILE_GPU("RmlUi.RenderFilters");

    if (!InitShaders())
        return;

    for (const Rml::CompiledFilterHandle filter_handle : filter_handles)
    {
        const CompiledFilter* filter = FilterCache[(int)filter_handle];
        if (filter == nullptr)
            return;

        SetupRenderTarget(render_layers.GetTopLayer());
        CurrentGPUContext->FlushState();

        const FilterType type = filter->type;

        switch (type)
        {
        case FilterType::Passthrough:
        {
            /*
            UseProgram(ProgramId::Passthrough);
            glBlendFunc(GL_CONSTANT_ALPHA, GL_ZERO);
            glBlendColor(0.0f, 0.0f, 0.0f, filter.blend_factor);

            const auto source = render_layers.GetPostprocessPrimary();
            const auto destination = render_layers.GetPostprocessSecondary();
            Gfx::BindTexture(source);
            glBindFramebuffer(GL_FRAMEBUFFER, destination.framebuffer);

            DrawFullscreenQuad();

            render_layers.SwapPostprocessPrimarySecondary();
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);*/
            GPUPipelineState* pipeline = PassThroughPipelineBlend;
            const auto source = render_layers.GetPostprocessPrimary();
            const auto destination = render_layers.GetPostprocessSecondary();

            GPUConstantBuffer* constantBuffer = FiltersShader->GetShader()->GetCB(0);

            CurrentGPUContext->SetBlendFactor(Float4(filter->blend_factor));
            // Update constant buffer data
            FilterCustomData data;

            Matrix::Transpose(ViewProjection, data.ViewProjection);
            Matrix::Transpose(CurrentTransform, data.Model);
            CurrentGPUContext->UpdateCB(constantBuffer, &data);

            // State and bindings
            CurrentGPUContext->BindSR(0, source.framebuffer);
            CurrentGPUContext->SetRenderTarget(destination.framebuffer);

            CurrentGPUContext->BindCB(0, constantBuffer);


            CurrentGPUContext->SetState(pipeline);
            CurrentGPUContext->DrawFullscreenTriangle();

            CurrentGPUContext->SetBlendFactor(Float4(1));

            render_layers.SwapPostprocessPrimarySecondary();
        }
        break;
        case FilterType::Blur:
        {
            PROFILE_GPU("RmlUi.RenderFilters.Blur");
            /*
            glDisable(GL_BLEND);*/

            const auto source_destination = render_layers.GetPostprocessPrimary();
            const auto temp = render_layers.GetPostprocessSecondary();

            const Rectangle window_flipped = CurrentScissor;
            RenderBlur(filter->sigma, source_destination, temp, window_flipped);
            /*glEnable(GL_BLEND); */
        }
        break;
        case FilterType::DropShadow:
        {
            PROFILE_GPU("RmlUi.RenderFilters.DropShadow");
            /*
            UseProgram(ProgramId::DropShadow);
            glDisable(GL_BLEND);

            Rml::Colourf color = ConvertToColorf(filter.color);
            glUniform4fv(GetUniformLocation(UniformId::Color), 1, &color[0]);

            const Gfx::FramebufferData& primary = render_layers.GetPostprocessPrimary();
            const Gfx::FramebufferData& secondary = render_layers.GetPostprocessSecondary();
            Gfx::BindTexture(primary);
            glBindFramebuffer(GL_FRAMEBUFFER, secondary.framebuffer);

            const Rml::Rectanglei window_flipped = VerticallyFlipped(scissor_state, viewport_height);
            SetTexCoordLimits(GetUniformLocation(UniformId::TexCoordMin), GetUniformLocation(UniformId::TexCoordMax), window_flipped,
                {primary.width, primary.height});

            const Rml::Vector2f uv_offset = filter.offset / Rml::Vector2f(-(float)viewport_width, (float)viewport_height);
            DrawFullscreenQuad(uv_offset);

            if (filter.sigma >= 0.5f)
            {
                const Gfx::FramebufferData& tertiary = render_layers.GetPostprocessTertiary();
                RenderBlur(filter.sigma, secondary, tertiary, window_flipped);
            }

            UseProgram(ProgramId::Passthrough);
            BindTexture(primary);
            glEnable(GL_BLEND);
            DrawFullscreenQuad();

            render_layers.SwapPostprocessPrimarySecondary();*/

            CurrentGPUContext->ResetRenderTarget();
            CurrentGPUContext->ResetCB();
            CurrentGPUContext->ResetSR();

            CurrentGPUContext->SetState(DropShadowPipeline);
            const auto primary = render_layers.GetPostprocessPrimary();
            const auto secondary = render_layers.GetPostprocessSecondary();

            GPUConstantBuffer* constantBuffer = FiltersShader->GetShader()->GetCB(0);

            // Update constant buffer data
            FilterCustomData data;
            data._color = filter->color;

            Matrix::Transpose(ViewProjection, data.ViewProjection);
            Matrix::Transpose(CurrentTransform, data.Model);

            // State and bindings
            CurrentGPUContext->BindSR(0, primary.framebuffer);
            CurrentGPUContext->BindCB(0, constantBuffer);
            CurrentGPUContext->SetRenderTarget(secondary.framebuffer);


            const Rectangle window_flipped = CurrentScissor;
            data._texCoordMin = (window_flipped.GetUpperLeft() + Float2(0.5)) / Float2(primary.width, primary.height);
            data._texCoordMax = (window_flipped.GetBottomRight() - Float2(0.5)) / Float2(primary.width, primary.height);

            data.Offset = filter->offset / Float2(-(float)CurrentViewport.Width, (float)CurrentViewport.Height);
            CurrentGPUContext->UpdateCB(constantBuffer, &data);
            CurrentGPUContext->DrawFullscreenTriangle();

            if (filter->sigma >= 0.5f)
            {
                const auto tertiary = render_layers.GetPostprocessTertiary();
                RenderBlur(filter->sigma, secondary, tertiary, window_flipped);
            }

            CurrentGPUContext->SetState(PassThroughPipelineBlend);

            GPUConstantBuffer* pConstantBuffer = FiltersShader->GetShader()->GetCB(0);
            FilterCustomData pData;
            CurrentGPUContext->BindCB(0, pConstantBuffer);
            CurrentGPUContext->UpdateCB(pConstantBuffer, &pData);

            CurrentGPUContext->BindSR(0, primary.framebuffer);
            CurrentGPUContext->DrawFullscreenTriangle();

            render_layers.SwapPostprocessPrimarySecondary();
        }
        break;
        case FilterType::ColorMatrix:
        {
            PROFILE_GPU("RmlUi.RenderFilters.ColorMatrix");
            /*
            UseProgram(ProgramId::ColorMatrix);
            glDisable(GL_BLEND);

            const GLint uniform_location = program_data->uniforms.Get(ProgramId::ColorMatrix, UniformId::ColorMatrix);
            constexpr bool transpose = std::is_same<decltype(filter.color_matrix), Rml::RowMajorMatrix4f>::value;
            glUniformMatrix4fv(uniform_location, 1, transpose, filter.color_matrix.data());

            const Gfx::FramebufferData& source = render_layers.GetPostprocessPrimary();
            const Gfx::FramebufferData& destination = render_layers.GetPostprocessSecondary();
            Gfx::BindTexture(source);
            glBindFramebuffer(GL_FRAMEBUFFER, destination.framebuffer);

            DrawFullscreenQuad();

            render_layers.SwapPostprocessPrimarySecondary();
            glEnable(GL_BLEND);
            */
            CurrentGPUContext->ResetRenderTarget();
            CurrentGPUContext->ResetCB();
            CurrentGPUContext->ResetSR();
            CurrentGPUContext->SetState(ColorMatrixPipeline);

            GPUConstantBuffer* constantBuffer = FiltersShader->GetShader()->GetCB(0);

            // Update constant buffer data
            FilterCustomData data;
            //data._color_matrix = filter->color_matrix;
            Matrix::Transpose(filter->color_matrix, data._color_matrix);
            CurrentGPUContext->BindCB(0, constantBuffer);
            CurrentGPUContext->UpdateCB(constantBuffer, &data);

            const auto source = render_layers.GetPostprocessPrimary();
            const auto destination = render_layers.GetPostprocessSecondary();

            CurrentGPUContext->SetRenderTarget(destination.framebuffer);
            CurrentGPUContext->BindSR(0, source.framebuffer);

            CurrentGPUContext->DrawFullscreenTriangle();

            render_layers.SwapPostprocessPrimarySecondary();
        }
        break;
        case FilterType::MaskImage:
        {
            PROFILE_GPU("RmlUi.RenderFilters.MaskImage");
            /*
            UseProgram(ProgramId::BlendMask);
            glDisable(GL_BLEND);

            const Gfx::FramebufferData& source = render_layers.GetPostprocessPrimary();
            const Gfx::FramebufferData& blend_mask = render_layers.GetBlendMask();
            const Gfx::FramebufferData& destination = render_layers.GetPostprocessSecondary();

            Gfx::BindTexture(source);
            glActiveTexture(GL_TEXTURE1);
            Gfx::BindTexture(blend_mask);
            glActiveTexture(GL_TEXTURE0);

            glBindFramebuffer(GL_FRAMEBUFFER, destination.framebuffer);

            DrawFullscreenQuad();

            render_layers.SwapPostprocessPrimarySecondary();
            glEnable(GL_BLEND);
            */
            CurrentGPUContext->ResetRenderTarget();
            CurrentGPUContext->ResetCB();
            CurrentGPUContext->ResetSR();
            CurrentGPUContext->SetState(MaskImagePipeline);

            GPUConstantBuffer* constantBuffer = FiltersShader->GetShader()->GetCB(0);

            // Update constant buffer data
            FilterCustomData data;
            CurrentGPUContext->UpdateCB(constantBuffer, &data);

            const auto source = render_layers.GetPostprocessPrimary();
            const auto blend_mask = render_layers.GetBlendMask();
            const auto destination = render_layers.GetPostprocessSecondary();

            CurrentGPUContext->SetRenderTarget(destination.framebuffer);
            CurrentGPUContext->BindCB(0, constantBuffer);
            CurrentGPUContext->BindSR(0, source.framebuffer);
            CurrentGPUContext->BindSR(1, blend_mask.framebuffer);

            CurrentGPUContext->DrawFullscreenTriangle();

            render_layers.SwapPostprocessPrimarySecondary();
        }
        break;
        case FilterType::Invalid:
        {
            Rml::Log::Message(Rml::Log::LT_WARNING, "Unhandled render filter %d.", (int)type);
        }
        break;
        }
    }

}

void FlaxRenderInterface::ReleaseFilter(Rml::CompiledFilterHandle filter)
{
    FilterCache[(int)filter]->Dispose();
}
#pragma endregion

#pragma region Layers
Rml::LayerHandle FlaxRenderInterface::PushLayer()
{
    PROFILE_GPU("RmlUi.PushLayer");

    const Rml::LayerHandle layer_handle = render_layers.PushLayer();

    return layer_handle;
}

void FlaxRenderInterface::CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend_mode, Rml::Span<const Rml::CompiledFilterHandle> filters)
{
    PROFILE_GPU("RmlUi.CompositeLayers");

    if (!InitShaders())
        return;

    auto sourceLayer = render_layers.GetLayer(source);

    /*
        BlitLayerToPostprocessPrimary(source_handle);

        // Render the filters, the PostprocessPrimary framebuffer is used for both input and output.
        RenderFilters(filters);

        // Render to the destination layer.
        glBindFramebuffer(GL_FRAMEBUFFER, render_layers.GetLayer(destination_handle).framebuffer);
        Gfx::BindTexture(render_layers.GetPostprocessPrimary());

        UseProgram(ProgramId::Passthrough);

        if (blend_mode == BlendMode::Replace)
            glDisable(GL_BLEND);

        DrawFullscreenQuad();

        if (blend_mode == BlendMode::Replace)
            glEnable(GL_BLEND);

        if (destination_handle != render_layers.GetTopLayerHandle())
            glBindFramebuffer(GL_FRAMEBUFFER, render_layers.GetTopLayer().framebuffer);
    */
    CurrentGPUContext->Clear(render_layers.GetPostprocessPrimary().framebuffer, Color().Transparent);
    CurrentGPUContext->Clear(render_layers.GetPostprocessSecondary().framebuffer, Color().Transparent);
    BlitTexturePostProcessPrimary(sourceLayer);

    RenderFilters(filters);

    if(blend_mode == Rml::BlendMode::Blend)
        CurrentGPUContext->SetState(PassThroughPipelineBlend);
    else
        CurrentGPUContext->SetState(PassThroughPipeline);

    GPUConstantBuffer* pConstantBuffer = FiltersShader->GetShader()->GetCB(0);
    FilterCustomData pData;
    CurrentGPUContext->BindCB(0, pConstantBuffer);
    CurrentGPUContext->UpdateCB(pConstantBuffer, &pData);

    CurrentGPUContext->ResetRenderTarget();
    //CurrentGPUContext->SetRenderTarget(render_layers.GetLayer(destination).framebuffer);
    SetupRenderTarget(render_layers.GetLayer(destination));
    CurrentGPUContext->BindSR(0, render_layers.GetPostprocessPrimary().framebuffer);
    CurrentGPUContext->FlushState();

    CurrentGPUContext->DrawFullscreenTriangle();

    if (destination != render_layers.GetTopLayerHandle())
        CurrentGPUContext->SetRenderTarget(render_layers.GetTopLayer().framebuffer);
}

void FlaxRenderInterface::PopLayer()
{
    PROFILE_GPU("RmlUi.PopLayer");
    render_layers.PopLayer();
}

Rml::TextureHandle FlaxRenderInterface::SaveLayerAsTexture()
{
    PROFILE_GPU("RmlUi.SaveLayerAsTexture");
    LOG(Info, "SaveLayerAsTexture");

    /*
    Rml::TextureHandle render_texture = GenerateTexture({}, bounds.Size());
    if (!render_texture)
        return {};

    BlitLayerToPostprocessPrimary(render_layers.GetTopLayerHandle());

    EnableScissorRegion(false);

    const Gfx::FramebufferData& source = render_layers.GetPostprocessPrimary();
    const Gfx::FramebufferData& destination = render_layers.GetPostprocessSecondary();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source.framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destination.framebuffer);

    // Flip the image vertically, as that convention is used for textures, and move to origin.
    glBlitFramebuffer(                                  //
        bounds.Left(), source.height - bounds.Bottom(), // src0
        bounds.Right(), source.height - bounds.Top(),   // src1
        0, bounds.Height(),                             // dst0
        bounds.Width(), 0,                              // dst1
        GL_COLOR_BUFFER_BIT, GL_NEAREST                 //
    );

    glBindTexture(GL_TEXTURE_2D, (GLuint)render_texture);

    const Gfx::FramebufferData& texture_source = destination;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, texture_source.framebuffer);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, bounds.Width(), bounds.Height());

    SetScissor(bounds);
    glBindFramebuffer(GL_FRAMEBUFFER, render_layers.GetTopLayer().framebuffer);
    Gfx::CheckGLError("SaveLayerAsTexture");

    return render_texture;*/

    const auto bounds = CurrentScissor;

    Rml::TextureHandle texture_handle = GenerateTexture({}, Rml::Vector2i(bounds.Size.X, bounds.Size.Y));

    if (!texture_handle)
        return {};

    auto texture = LoadedTextures.At((int)texture_handle);
    if (texture == nullptr)
        return {};


    CurrentGPUContext->Clear(render_layers.GetPostprocessPrimary().framebuffer, Color().Transparent);
    BlitTexturePostProcessPrimary(render_layers.GetTopLayer());

    bool scissorsWereEnabled = UseScissor;
    EnableScissorRegion(false);

    const auto source = render_layers.GetPostprocessPrimary();

    BlitTexturesUV(source.framebuffer, Float4(0,0,1,1), texture->View(), Float4(0, 0, 1, 1));

    CurrentGPUContext->ResetRenderTarget();
    CurrentGPUContext->SetRenderTarget(render_layers.GetTopLayer().framebuffer);

    EnableScissorRegion(scissorsWereEnabled);

    return texture_handle;
}

Rml::CompiledFilterHandle FlaxRenderInterface::SaveLayerAsMaskImage()
{
    PROFILE_GPU("RmlUi.SaveLayerAsMaskImage");

    BlitTexturePostProcessPrimary(render_layers.GetTopLayer());

    GPUPipelineState* pipeline = PassThroughPipeline;
    const auto source = render_layers.GetPostprocessPrimary();
    const auto destination = render_layers.GetBlendMask();

    BlitTextures(source, destination);

    Rml::CompiledFilterHandle filterHandle;
    CompiledFilter* filter = ReserveFilter(filterHandle);
    filter->type = FilterType::MaskImage;

    return filterHandle;
}
#pragma endregion

#pragma region Shaders

CompiledShader *ReserveShader(Rml::CompiledShaderHandle &shaderHandle)
{
    // Cache geometry structures in order to reduce allocations and recreating buffers
    for (int i = 1; i < ShaderCache.Count(); i++)
    {
        if (ShaderCache[i]->reserved)
            continue;

        ShaderCache[i]->reserved = true;
        shaderHandle = Rml::CompiledFilterHandle(i);
        return ShaderCache[i];
    }

    CompiledShader *geometry = New<CompiledShader>();
    shaderHandle = Rml::CompiledShaderHandle(ShaderCache.Count());
    ShaderCache.Add(geometry);
    return geometry;
}

Rml::CompiledShaderHandle FlaxRenderInterface::CompileShader(const Rml::String &name, const Rml::Dictionary &parameters)
{
    PROFILE_GPU("RmlUi.CompileShader");
    auto ApplyColorStopList = [](CompiledShader *shader, const Rml::Dictionary &shader_parameters)
    {
        auto it = shader_parameters.find("color_stop_list");
        RMLUI_ASSERT(it != shader_parameters.end() && it->second.GetType() == Rml::Variant::COLORSTOPLIST);
        const Rml::ColorStopList &color_stop_list = it->second.GetReference<Rml::ColorStopList>();
        const int num_stops = Rml::Math::Min((int)color_stop_list.size(), MAX_NUM_STOPS);

        shader->stop_positions.Resize(num_stops);
        shader->stop_colors.Resize(num_stops);
        for (int i = 0; i < num_stops; i++)
        {
            const Rml::ColorStop &stop = color_stop_list[i];
            RMLUI_ASSERT(stop.position.unit == Rml::Unit::NUMBER);
            shader->stop_positions[i] = stop.position.number;
            const auto color = stop.color;
            shader->stop_colors[i] = Color(Color32(color.red, color.green, color.blue, color.alpha));
        }
    };

    Rml::CompiledShaderHandle shaderHandle;
    CompiledShader *shader = ReserveShader(shaderHandle);

    if (name == "linear-gradient")
    {
        shader->type = CompiledShaderType::Gradient;
        const bool repeating = Rml::Get(parameters, "repeating", false);
        shader->gradient_function = (repeating ? ShaderGradientFunction::RepeatingLinear : ShaderGradientFunction::Linear);
        shader->p = ToFloat2(Rml::Get(parameters, "p0", Rml::Vector2f(0.f)));
        shader->v = ToFloat2(Rml::Get(parameters, "p1", Rml::Vector2f(0.f))) - shader->p;
        ApplyColorStopList(shader, parameters);
    }
    else if (name == "radial-gradient")
    {
        shader->type = CompiledShaderType::Gradient;
        const bool repeating = Rml::Get(parameters, "repeating", false);
        shader->gradient_function = (repeating ? ShaderGradientFunction::RepeatingRadial : ShaderGradientFunction::Radial);
        shader->p = ToFloat2(Rml::Get(parameters, "center", Rml::Vector2f(0.f)));
        shader->v = ToFloat2(Rml::Vector2f(1.f) / Rml::Get(parameters, "radius", Rml::Vector2f(1.f)));
        ApplyColorStopList(shader, parameters);
    }
    else if (name == "conic-gradient")
    {
        shader->type = CompiledShaderType::Gradient;
        const bool repeating = Rml::Get(parameters, "repeating", false);
        shader->gradient_function = (repeating ? ShaderGradientFunction::RepeatingConic : ShaderGradientFunction::Conic);
        shader->p = ToFloat2(Rml::Get(parameters, "center", Rml::Vector2f(0.f)));
        const float angle = Rml::Get(parameters, "angle", 0.f);
        shader->v = {Rml::Math::Cos(angle), Rml::Math::Sin(angle)};
        ApplyColorStopList(shader, parameters);
    }
    else if (name == "shader")
    {
        const Rml::String value = Rml::Get(parameters, "value", Rml::String());
        if (value == "creation")
        {
            shader->type = CompiledShaderType::Creation;
            shader->dimensions = ToFloat2(Rml::Get(parameters, "dimensions", Rml::Vector2f(0.f)));
        }
    }

    if (shader->type != CompiledShaderType::Invalid)
        return shaderHandle;

    Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported shader type '%s'.", name.c_str());
    return {};
}

void FlaxRenderInterface::RenderShader(Rml::CompiledShaderHandle shader_handle, Rml::CompiledGeometryHandle geometry_handle, Rml::Vector2f translation, Rml::TextureHandle texture)
{
    PROFILE_GPU("RmlUi.RenderShader");

    if (!InitShaders())
        return;

    const CompiledShader *shader = ShaderCache[(int)shader_handle];
    if (shader == nullptr)
        return;

    CompiledGeometry *compiledGeometry = GeometryCache[(int)geometry_handle];
    if (compiledGeometry == nullptr)
        return;

    compiledGeometry->vertexBuffer.Flush(CurrentGPUContext);
    compiledGeometry->indexBuffer.Flush(CurrentGPUContext);

    const CompiledShaderType type = shader->type;

    GPUPipelineState* pipeline;

    GPUBuffer* vb = compiledGeometry->vertexBuffer.GetBuffer();
    GPUBuffer* ib = compiledGeometry->indexBuffer.GetBuffer();

    CurrentGPUContext->ResetSR();
    CurrentGPUContext->SetRenderTarget(render_layers.GetTopLayer().framebuffer);
    if (UseScissor)
    {
        CurrentGPUContext->SetViewport(CurrentViewport);
        CurrentGPUContext->SetScissor(CurrentScissor);
    }
    else
        CurrentGPUContext->SetViewportAndScissors(CurrentViewport);
    CurrentGPUContext->FlushState();


    switch (type)
    {
    case CompiledShaderType::Gradient:
    {
        RMLUI_ASSERT(shader.stop_positions.size() == shader.stop_colors.size());
        const int num_stops = (int)shader->stop_positions.Count();
        /*
        UseProgram(ProgramId::Gradient);
        glUniform1i(GetUniformLocation(UniformId::Func), static_cast<int>(shader.gradient_function));
        glUniform2f(GetUniformLocation(UniformId::P), shader.p.x, shader.p.y);
        glUniform2f(GetUniformLocation(UniformId::V), shader.v.x, shader.v.y);
        glUniform1i(GetUniformLocation(UniformId::NumStops), num_stops);
        glUniform1fv(GetUniformLocation(UniformId::StopPositions), num_stops, shader.stop_positions.data());
        glUniform4fv(GetUniformLocation(UniformId::StopColors), num_stops, shader.stop_colors[0]);

        SubmitTransformUniform(translation);
        glBindVertexArray(geometry.vao);
        glDrawElements(GL_TRIANGLES, geometry.draw_count, GL_UNSIGNED_INT, (const GLvoid *)0);
        glBindVertexArray(0);
        */

        GPUConstantBuffer* constantBuffer = RmlShaderShader->GetShader()->GetCB(0);
        pipeline = GradientPipeline;

        // Update constant buffer data
        RmlShaderCustomData data;
        data.GradientFunction = shader->gradient_function;
        data.P = shader->p;
        data.V = shader->v;
        data.NumStops = num_stops;

        Matrix::Transpose(ViewProjection, data.ViewProjection);
        Matrix::Transpose(CurrentTransform, data.Model);
        data.Offset = (Float2)translation;
        CurrentGPUContext->UpdateCB(constantBuffer, &data);
        
        if (shader->stop_colors.Count() * sizeof(Color) > GradientColorsBuffer->GetSize())
            GradientColorsBuffer->Init(GPUBufferDescription::Structured(shader->stop_colors.Count(), sizeof(Color)));
        CurrentGPUContext->UpdateBuffer(GradientColorsBuffer, shader->stop_colors.Get(), shader->stop_colors.Count() * sizeof(Color));

        if (shader->stop_positions.Count() * sizeof(float) > GradientColorStopsBuffer->GetSize())
            GradientColorStopsBuffer->Init(GPUBufferDescription::Structured(shader->stop_positions.Count(), sizeof(float)));
        CurrentGPUContext->UpdateBuffer(GradientColorStopsBuffer, shader->stop_positions.Get(), shader->stop_positions.Count() * sizeof(float));

        // State and bindings
        CurrentGPUContext->BindSR(0, GradientColorsBuffer->View());
        CurrentGPUContext->BindSR(1, GradientColorStopsBuffer->View());
        
        CurrentGPUContext->BindCB(0, constantBuffer);

        CurrentGPUContext->BindVB(Span<GPUBuffer*>(&vb, 1));
        CurrentGPUContext->BindIB(ib);
    }
    break;
    case CompiledShaderType::Creation:
    {
        return;
    }
    break;
    case CompiledShaderType::Invalid:
    {
        Rml::Log::Message(Rml::Log::LT_WARNING, "Unhandled render shader %d.", (int)type);
    }
    break;
    }

    CurrentGPUContext->SetState(pipeline);
    CurrentGPUContext->DrawIndexed(compiledGeometry->indexBuffer.Data.Count() / sizeof(uint32));
}

void FlaxRenderInterface::ReleaseShader(Rml::CompiledShaderHandle shader_handle)
{
    ShaderCache[(int)shader_handle]->Dispose();
}
#pragma endregion
Viewport FlaxRenderInterface::GetViewport()
{
    return CurrentViewport;
}


void FlaxRenderInterface::Begin(RenderContext *renderContext, GPUContext *gpuContext, Viewport viewport)
{
    PROFILE_GPU_CPU("RmlUi.Begin");
    CurrentRenderContext = renderContext;
    CurrentGPUContext = gpuContext;
    CurrentViewport = viewport;
    CurrentTransform = Matrix::Identity;
    CurrentScissor = viewport.GetBounds();

    Matrix view, projection;
    const float halfWidth = viewport.Width * 0.5f;
    const float halfHeight = viewport.Height * 0.5f;
    const float zNear = 0.0f;
    const float zFar = 1.0f;
    Matrix::OrthoOffCenter(-halfWidth, halfWidth, halfHeight, -halfHeight, zNear, zFar, projection);
    Matrix::Translation(-halfWidth, -halfHeight, 0, view);
    Matrix::Multiply(view, projection, ViewProjection);

    render_layers.BeginFrame(viewport.Width, viewport.Height, CurrentRenderContext->Task->GetOutputView());
}

void FlaxRenderInterface::End()
{
    PROFILE_GPU_CPU("RmlUi.End");

    const auto fb_active = render_layers.GetTopLayer();
    const auto fb_postprocess = render_layers.GetPostprocessPrimary();

    //BlitTextures(fb_postprocess, fb_active);
    CurrentGPUContext->FlushState();
    CurrentGPUContext->ResetSR();
    CurrentGPUContext->SetRenderTarget(render_layers.GetTopLayer().framebuffer);

    render_layers.EndFrame();
    //
    // Flush generated glyphs to GPU
    FontManager::Flush();
    ((FlaxFontEngineInterface *)Rml::GetFontEngineInterface())->FlushFontAtlases();

    CurrentRenderContext = nullptr;
    CurrentGPUContext = nullptr;
}

Rml::TextureHandle FlaxRenderInterface::GetTextureHandle(GPUTexture *texture)
{
    if (texture == nullptr)
        return Rml::TextureHandle();

    for (int i = 1; i < LoadedTextures.Count(); i++)
    {
        if (LoadedTextures[i] == texture)
            return Rml::TextureHandle(i);
    }
    return Rml::TextureHandle();
}

Rml::TextureHandle FlaxRenderInterface::RegisterTexture(GPUTexture *texture, bool isFontTexture)
{
    Rml::TextureHandle handle = (Rml::TextureHandle)LoadedTextures.Count();
    LoadedTextures.Add(texture);

    if (isFontTexture)
        FontTextures.Add(texture);

    return handle;
}

void FlaxRenderInterface::ReleaseResources()
{
    LoadedTextureAssets.Clear();
    FontTextures.Clear();
    LoadedTextures.Clear();
    GPUTexture** data = AllocatedTextures.Get();
    for (int32 i = 0; i < AllocatedTextures.Count(); i++)
    {
        if (data[i])
            data[i]->ReleaseGPU();
    }
    AllocatedTextures.ClearDelete();
    GeometryCache.ClearDelete();
}

bool FlaxRenderInterface::CreateFramebuffer(FramebufferData &out_fb, int width, int height, MSAALevel samples, FramebufferAttachment attachment, GPUTextureView *shared_depth_stencil_buffer, GPUTextureView *outputBuffer)
{
    if (!outputBuffer)
    {
        auto texture = GPUDevice::Instance->CreateTexture(TEXT("Rml.Framebuffer"));

        if (texture->Init(GPUTextureDescription::New2D(width, height, PixelFormat::B8G8R8A8_UNorm, GPUTextureFlags::ShaderResource | GPUTextureFlags::RenderTarget, 1, 1, MSAALevel::None)))
            return false;

        AllocatedTextures.Add(texture);

        outputBuffer = texture->View();
    }

    GPUTextureView *depth_stencil_buffer = nullptr;
    if (attachment != FramebufferAttachment::None)
    {
        if (shared_depth_stencil_buffer)
        {
            // Share depth/stencil buffer
            depth_stencil_buffer = shared_depth_stencil_buffer;
        }
        else {
            auto texture = GPUDevice::Instance->CreateTexture(TEXT("Rml.DepthBuffer"));

            if (texture->Init(GPUTextureDescription::New2D(width, height, PixelFormat::D24_UNorm_S8_UInt, GPUTextureFlags::ShaderResource | GPUTextureFlags::DepthStencil)))
                return false;

            AllocatedTextures.Add(texture);

            depth_stencil_buffer = texture->View();
        }
    }

    out_fb = {};
    out_fb.width = width;
    out_fb.height = height;
    out_fb.framebuffer = outputBuffer;
    out_fb.depth_stencil_buffer = depth_stencil_buffer;
    out_fb.owns_depth_stencil_buffer = !shared_depth_stencil_buffer;

    return true;
}

void FlaxRenderInterface::DestroyFameBuffer(FramebufferData &buffer)
{

}

void FlaxRenderInterface::SetupRenderTarget(FramebufferData data, bool allowScissor)
{
    CurrentGPUContext->ResetRenderTarget();
    CurrentGPUContext->ResetSR();
    if(data.depth_stencil_buffer != nullptr)
        CurrentGPUContext->SetRenderTarget(data.depth_stencil_buffer, data.framebuffer);
    else
        CurrentGPUContext->SetRenderTarget(data.framebuffer);

    if (UseScissor && allowScissor)
    {
        CurrentGPUContext->SetViewport(CurrentViewport);
        CurrentGPUContext->SetScissor(CurrentScissor);
    }
    else
        CurrentGPUContext->SetViewportAndScissors(CurrentViewport);
}

FlaxRenderInterface::RenderLayerStack::RenderLayerStack()
{
    fb_postprocess.resize(4);
}

FlaxRenderInterface::RenderLayerStack::~RenderLayerStack()
{
    DestroyFramebuffers();
}

Rml::LayerHandle FlaxRenderInterface::RenderLayerStack::PushLayer(GPUTextureView *outputBuffer)
{
    ASSERT(layers_size <= (int)fb_layers.size());

    if (layers_size == (int)fb_layers.size())
    {
        // All framebuffers should share a single stencil buffer.
        GPUTextureView *shared_depth_stencil = (fb_layers.empty() ? (GPUTextureView *)nullptr : fb_layers.front().depth_stencil_buffer);

        fb_layers.push_back(FlaxRenderInterface::FramebufferData{});
        FlaxRenderInterface::CreateFramebuffer(fb_layers.back(), width, height, MSAALevel::X2, FramebufferAttachment::DepthStencil, shared_depth_stencil, outputBuffer);
    }
    layers_size += 1;

    if(outputBuffer == nullptr)
        CurrentGPUContext->Clear(GetTopLayer().framebuffer, Color().Transparent);
    return GetTopLayerHandle();
}

void FlaxRenderInterface::RenderLayerStack::PopLayer()
{
    ASSERT(layers_size > 0);
    layers_size -= 1;
}

const FlaxRenderInterface::FramebufferData &FlaxRenderInterface::RenderLayerStack::GetLayer(Rml::LayerHandle layer) const
{
    // TODO: Sem vložte příkaz
    ASSERT((size_t)layer < (size_t)layers_size);
    return fb_layers[layer];
}

const FlaxRenderInterface::FramebufferData &FlaxRenderInterface::RenderLayerStack::GetTopLayer() const
{
    return GetLayer(GetTopLayerHandle());
}

Rml::LayerHandle FlaxRenderInterface::RenderLayerStack::GetTopLayerHandle() const
{
    ASSERT(layers_size > 0);
    return static_cast<Rml::LayerHandle>(layers_size - 1);
}

void FlaxRenderInterface::RenderLayerStack::SwapPostprocessPrimarySecondary()
{
    std::swap(fb_postprocess[0], fb_postprocess[1]);
}

void FlaxRenderInterface::RenderLayerStack::BeginFrame(int new_width, int new_height, GPUTextureView *outputView)
{
    ASSERT(layers_size == 0);

    if (new_width != width || new_height != height)
    {
        width = new_width;
        height = new_height;

        DestroyFramebuffers();
    }

    PushLayer(outputView);
}

void FlaxRenderInterface::RenderLayerStack::EndFrame()
{
    ASSERT(layers_size == 1);
    PopLayer();
}

void FlaxRenderInterface::RenderLayerStack::DestroyFramebuffers()
{
    FMT_ASSERT(layers_size == 0, "Do not call this during frame rendering, that is, between BeginFrame() and EndFrame().");

    for (FlaxRenderInterface::FramebufferData &fb : fb_layers)
        FlaxRenderInterface::DestroyFameBuffer(fb);

    fb_layers.clear();

    for (FlaxRenderInterface::FramebufferData &fb : fb_postprocess)
        FlaxRenderInterface::DestroyFameBuffer(fb);
}

const FlaxRenderInterface::FramebufferData &FlaxRenderInterface::RenderLayerStack::EnsureFramebufferPostprocess(int index)
{
    ASSERT(index < (int)fb_postprocess.size())
    FlaxRenderInterface::FramebufferData &fb = fb_postprocess[index];

    if (!fb.framebuffer)
        FlaxRenderInterface::CreateFramebuffer(fb, width, height, MSAALevel::None, FramebufferAttachment::None, nullptr);

    return fb;
}
