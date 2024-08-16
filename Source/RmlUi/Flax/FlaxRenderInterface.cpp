#include "RmlUiPlugin.h"

// Conflicts with both Flax and RmlUi Math.h
#undef RadiansToDegrees
#undef DegreesToRadians
#undef NormaliseAngle

#include "FlaxFontEngineInterface.h"
#include "FlaxRenderInterface.h"
#include "StaticIndexBuffer.h"
#include "StaticVertexBuffer.h"

#include <ThirdParty/RmlUi/Core/Context.h>
#include <ThirdParty/RmlUi/Core/Core.h>
#include <ThirdParty/RmlUi/Core/FontEngineInterface.h>

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
#include <Engine/Render2D/RotatedRectangle.h>
#include <Engine/Graphics/RenderTargetPool.h>

struct BasicVertex
{
    Float2 Position;
    Half2 TexCoord;
    Color Color;
    Float2 ClipOrigin;
    RotatedRectangle ClipMask;
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
          type(FilterType::Invalid)
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

PACK_STRUCT(struct CustomData {
    Matrix ViewProjection;
    Matrix Model;
    Float2 Offset;
    Float2 Dummy;
});
#define RENDER2D_BLUR_MAX_SAMPLES 64

// The format for the blur effect temporary buffer
#define PS_Blur_Format PixelFormat::R8G8B8A8_UNorm

PACK_STRUCT(struct BlurData {
    Float2 InvBufferSize;
    uint32 SampleCount;
    float Dummy0;
    Float4 Bounds;
    Float4 WeightAndOffsets[RENDER2D_BLUR_MAX_SAMPLES / 2];
});

namespace
{
    RenderContext *CurrentRenderContext = nullptr;
    GPUContext *CurrentGPUContext = nullptr;
    Viewport CurrentViewport;

    Rectangle CurrentScissor;
    Matrix CurrentTransform;
    Matrix ViewProjection;
    bool UseScissor = false;

    AssetReference<Shader> BasicShader;
    AssetReference<Shader> GUIShader;

    GPUPipelineState *FontPipeline = nullptr;
    GPUPipelineState *ImagePipeline = nullptr;
    GPUPipelineState *ColorPipeline = nullptr;

    GPUPipelineState *DownscalePipeline = nullptr;
    GPUPipelineState *DownscalePipelineBlend = nullptr;
    GPUPipelineState *BlurHPipeline = nullptr;
    GPUPipelineState *BlurVPipeline = nullptr;

    Array<CompiledGeometry *> GeometryCache(2);
    Array<CompiledFilter *> FilterCache(2);

    Dictionary<GPUTexture *, AssetReference<Texture>> LoadedTextureAssets(32);
    Array<GPUTexture *> LoadedTextures(32);
    Array<GPUTexture *> AllocatedTextures(32);
    HashSet<GPUTexture *> FontTextures(32);
}

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

CompiledFilter *ReserveFilter(Rml::CompiledFilterHandle &filterHandle)
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

    CompiledFilter *geometry = New<CompiledFilter>();
    filterHandle = Rml::CompiledFilterHandle(FilterCache.Count());
    FilterCache.Add(geometry);
    return geometry;
}

void FlaxRenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle handle)
{
    if ((int)handle == 0)
        return;
    GeometryCache[(int)handle]->Dispose();
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

    // Handles with value of 0 are invalid, reserve the first slot in the arrays
    LoadedTextures.Add(nullptr);
    GeometryCache.Add(nullptr);
    FilterCache.Add(nullptr);
}

FlaxRenderInterface::~FlaxRenderInterface()
{
    BasicShader.Get()->OnReloading.Unbind<FlaxRenderInterface, &FlaxRenderInterface::InvalidateShaders>(this);
    GUIShader.Get()->OnReloading.Unbind<FlaxRenderInterface, &FlaxRenderInterface::InvalidateShaders>(this);
    InvalidateShaders();
}

void FlaxRenderInterface::InvalidateShaders(Asset *obj)
{
    SAFE_DELETE_GPU_RESOURCE(FontPipeline);
    SAFE_DELETE_GPU_RESOURCE(ImagePipeline);
    SAFE_DELETE_GPU_RESOURCE(ColorPipeline);
}
bool FlaxRenderInterface::InitShaders()
{
    if (!BasicShader->IsLoaded() && BasicShader->WaitForLoaded())
        return false;

    if (!GUIShader->IsLoaded() && GUIShader->WaitForLoaded())
        return false;
    // Setup pipelines
    if (FontPipeline == nullptr || ImagePipeline == nullptr || ColorPipeline == nullptr)
    {
        bool useDepth = false;
        GPUPipelineState::Description desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        desc.DepthEnable = desc.DepthWriteEnable = useDepth;
        desc.DepthWriteEnable = false;
        desc.DepthClipEnable = false;
        desc.VS = BasicShader->GetShader()->GetVS("VS");
        desc.CullMode = CullMode::TwoSided;

        desc.BlendMode = BlendingMode::AlphaBlend;
        desc.PS = BasicShader->GetShader()->GetPS("PS_Font");
        FontPipeline = GPUDevice::Instance->CreatePipelineState();
        if (FontPipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create font pipeline state");
            return false;
        }

        desc.PS = BasicShader->GetShader()->GetPS("PS_Image");
        ImagePipeline = GPUDevice::Instance->CreatePipelineState();
        if (ImagePipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create image pipeline state");
            return false;
        }

        desc.BlendMode = BlendingMode::AlphaBlend;
        desc.PS = BasicShader->GetShader()->GetPS("PS_Color");
        ColorPipeline = GPUDevice::Instance->CreatePipelineState();
        if (ColorPipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create color pipeline state");
            return false;
        }
    }
    if (DownscalePipelineBlend == nullptr || DownscalePipeline == nullptr || BlurHPipeline == nullptr || BlurVPipeline == nullptr)
    {
        bool useDepth = false;
        GPUPipelineState::Description desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        desc.DepthEnable = desc.DepthWriteEnable = useDepth;
        desc.DepthWriteEnable = false;
        desc.DepthClipEnable = false;
        desc.VS = GUIShader->GetShader()->GetVS("VS");
        desc.CullMode = CullMode::TwoSided;

        desc.BlendMode = BlendingMode::AlphaBlend;
        desc.PS = GUIShader->GetShader()->GetPS("PS_Downscale");
        DownscalePipelineBlend = GPUDevice::Instance->CreatePipelineState();
        if (DownscalePipelineBlend->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create color pipeline state");
            return false;
        }

        desc.BlendMode = BlendingMode::Opaque;
        desc.PS = GUIShader->GetShader()->GetPS("PS_Downscale");
        DownscalePipeline = GPUDevice::Instance->CreatePipelineState();
        if (DownscalePipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create color pipeline state");
            return false;
        }

        desc.PS = GUIShader->GetShader()->GetPS("PS_Blur");
        BlurHPipeline = GPUDevice::Instance->CreatePipelineState();
        if (BlurHPipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create color pipeline state");
            return false;
        }

        desc.PS = GUIShader->GetShader()->GetPS("PS_Blur", 1);
        BlurVPipeline = GPUDevice::Instance->CreatePipelineState();
        if (BlurVPipeline->Init(desc))
        {
            LOG(Error, "RmlUi: Failed to create color pipeline state");
            return false;
        }
    }
    return true;
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
        vb0.TexCoord = Half2((Float2)vertices[i].tex_coord);
        vb0.Color = Color(Color32(vertices[i].colour.red, vertices[i].colour.green, vertices[i].colour.blue, vertices[i].colour.alpha));
        vb0.ClipOrigin = Float2::Zero;
        vb0.ClipMask = defaultMask;

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

    compiledGeometry->vertexBuffer.Flush(CurrentGPUContext);
    compiledGeometry->indexBuffer.Flush(CurrentGPUContext);

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

    GPUConstantBuffer *constantBuffer = BasicShader->GetShader()->GetCB(0);
    GPUBuffer *vb = compiledGeometry->vertexBuffer.GetBuffer();
    GPUBuffer *ib = compiledGeometry->indexBuffer.GetBuffer();

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
    CurrentGPUContext->BindVB(Span<GPUBuffer *>(&vb, 1));
    CurrentGPUContext->BindIB(ib);
    CurrentGPUContext->SetState(pipeline);

    CurrentGPUContext->DrawIndexed(compiledGeometry->indexBuffer.Data.Count() / sizeof(uint32));
}

void FlaxRenderInterface::HookGenerateTexture(Rml::TextureHandle textureHandle)
{
    _generateTextureOverride = textureHandle;
}

void FlaxRenderInterface::EnableScissorRegion(bool enable)
{
    PROFILE_GPU("RmlUi.EnableScissorRegion");
    UseScissor = enable;
}

void FlaxRenderInterface::SetScissorRegion(Rml::Rectanglei region)
{
    PROFILE_GPU("RmlUi.SetScissorRegion");
    CurrentScissor = Rectangle((float)region.Position().x, (float)region.Position().y, (float)region.Size().x, (float)region.Size().y);
    // LOG(Info, "Set Scissor: {0} {1} {2} {3}", CurrentScissor.GetX(), CurrentScissor.GetY(), CurrentScissor.GetWidth(), CurrentScissor.GetHeight());
}

void FlaxRenderInterface::EnableClipMask(bool enable)
{
    PROFILE_GPU("RmlUi.EnableClipMask");
}

void FlaxRenderInterface::RenderToClipMask(Rml::ClipMaskOperation mask_operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation)
{
    PROFILE_GPU("RmlUi.RenderToClipMask");
}

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

    BytesContainer data(source_data.data(), source_dimensions.x * source_dimensions.y * 4);
    auto task = texture->UploadMipMapAsync(data, 0, true);
    if (task)
        task->Start();

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

void FlaxRenderInterface::SetTransform(const Rml::Matrix4f *transform_)
{
    PROFILE_GPU("RmlUi.SetTransform");
    // We assume the library is not built with row-major matrices enabled
    CurrentTransform = transform_ != nullptr ? *(const Matrix *)transform_->data() : Matrix::Identity;
}

Rml::LayerHandle FlaxRenderInterface::PushLayer()
{
    PROFILE_GPU("RmlUi.PushLayer");

    const Rml::LayerHandle layer_handle = render_layers.PushLayer();

    return layer_handle;
}

void FlaxRenderInterface::RenderFilters(Rml::Span<const Rml::CompiledFilterHandle> filter_handles)
{
    PROFILE_GPU("RmlUi.RenderFilters");
    /*for (const Rml::CompiledFilterHandle filter_handle : filter_handles)
    {
        const CompiledFilter& filter = *reinterpret_cast<const CompiledFilter*>(filter_handle);
        const FilterType type = filter.type;

        switch (type)
        {
        case FilterType::Passthrough:
        {
            UseProgram(ProgramId::Passthrough);
            glBlendFunc(GL_CONSTANT_ALPHA, GL_ZERO);
            glBlendColor(0.0f, 0.0f, 0.0f, filter.blend_factor);

            const Gfx::FramebufferData& source = render_layers.GetPostprocessPrimary();
            const Gfx::FramebufferData& destination = render_layers.GetPostprocessSecondary();
            Gfx::BindTexture(source);
            glBindFramebuffer(GL_FRAMEBUFFER, destination.framebuffer);

            DrawFullscreenQuad();

            render_layers.SwapPostprocessPrimarySecondary();
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        break;
        case FilterType::Blur:
        {
            glDisable(GL_BLEND);

            const Gfx::FramebufferData& source_destination = render_layers.GetPostprocessPrimary();
            const Gfx::FramebufferData& temp = render_layers.GetPostprocessSecondary();

            const Rml::Rectanglei window_flipped = VerticallyFlipped(scissor_state, viewport_height);
            RenderBlur(filter.sigma, source_destination, temp, window_flipped);

            glEnable(GL_BLEND);
        }
        break;
        case FilterType::DropShadow:
        {
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

            render_layers.SwapPostprocessPrimarySecondary();
        }
        break;
        case FilterType::ColorMatrix:
        {
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
        }
        break;
        case FilterType::MaskImage:
        {
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
        }
        break;
        case FilterType::Invalid:
        {
            Rml::Log::Message(Rml::Log::LT_WARNING, "Unhandled render filter %d.", (int)type);
        }
        break;
        }
    }*/
}

void FlaxRenderInterface::CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend_mode, Rml::Span<const Rml::CompiledFilterHandle> filters)
{
    PROFILE_GPU("RmlUi.CompositeLayers");

    if (!InitShaders())
        return;

    auto sourceLayer = render_layers.GetLayer(source);
    auto destinationLayer = render_layers.GetLayer(destination);
    auto postProcess = render_layers.GetPostprocessPrimary();

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
}

void FlaxRenderInterface::PopLayer()
{
    PROFILE_GPU("RmlUi.PopLayer");
    render_layers.PopLayer();
}

Rml::TextureHandle FlaxRenderInterface::SaveLayerAsTexture(Rml::Vector2i dimensions)
{
    PROFILE_GPU("RmlUi.PopLayer");
    return Rml::TextureHandle();
}

Rml::CompiledFilterHandle FlaxRenderInterface::SaveLayerAsMaskImage()
{
    PROFILE_GPU("RmlUi.PopLayer");
    return Rml::CompiledFilterHandle();
}

Rml::CompiledFilterHandle FlaxRenderInterface::CompileFilter(const Rml::String &name, const Rml::Dictionary &parameters)
{
    PROFILE_GPU("RmlUi.CompileFilter");

    Rml::CompiledFilterHandle filterHandle;
    CompiledFilter *filter = ReserveFilter(filterHandle);

    if (name == "opacity")
    {
        filter->type = FilterType::Passthrough;
        filter->blend_factor = Rml::Get(parameters, "value", 1.0f);
    }
    else if (name == "blur")
    {
        filter->type = FilterType::Blur;
        filter->sigma = 0.5f * Rml::Get(parameters, "radius", 1.0f);
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
        filter->color_matrix = *(const Matrix *)Rml::Matrix4f::Diag(value, value, value, 1.f).data();
    }
    else if (name == "contrast")
    {
        filter->type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float grayness = 0.5f - 0.5f * value;
        filter->color_matrix = *(const Matrix *)Rml::Matrix4f::Diag(value, value, value, 1.f).data();
        filter->color_matrix.SetColumn4(Float4(grayness, grayness, grayness, 1.f));
    }
    else if (name == "invert")
    {
        filter->type = FilterType::ColorMatrix;
        const float value = Rml::Math::Clamp(Rml::Get(parameters, "value", 1.0f), 0.f, 1.f);
        const float inverted = 1.f - 2.f * value;
        filter->color_matrix = *(const Matrix *)Rml::Matrix4f::Diag(inverted, inverted, inverted, 1.f).data();
        filter->color_matrix.SetColumn4(Float4(value, value, value, 1.f));
    }
    else if (name == "grayscale")
    {
        filter->type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float rev_value = 1.f - value;
        const Rml::Vector3f gray = value * Rml::Vector3f(0.2126f, 0.7152f, 0.0722f);
        // clang-format off
        filter->color_matrix = *(const Matrix*)Rml::Matrix4f::FromRows(
            { gray.x + rev_value, gray.y,             gray.z,             0.f },
            { gray.x,             gray.y + rev_value, gray.z,             0.f },
            { gray.x,             gray.y,             gray.z + rev_value, 0.f },
            { 0.f,                0.f,                0.f,                1.f }
        ).data();
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
        filter->color_matrix = *(const Matrix*)Rml::Matrix4f::FromRows(
            { r_mix.x + rev_value, r_mix.y,             r_mix.z,             0.f },
            { g_mix.x,             g_mix.y + rev_value, g_mix.z,             0.f },
            { b_mix.x,             b_mix.y,             b_mix.z + rev_value, 0.f },
            { 0.f,                 0.f,                 0.f,                 1.f }
        ).data();
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
        filter->color_matrix = *(const Matrix*)Rml::Matrix4f::FromRows(
            { 0.213f + 0.787f * c - 0.213f * s,  0.715f - 0.715f * c - 0.715f * s,  0.072f - 0.072f * c + 0.928f * s,  0.f },
            { 0.213f - 0.213f * c + 0.143f * s,  0.715f + 0.285f * c + 0.140f * s,  0.072f - 0.072f * c - 0.283f * s,  0.f },
            { 0.213f - 0.213f * c - 0.787f * s,  0.715f - 0.715f * c + 0.715f * s,  0.072f + 0.928f * c + 0.072f * s,  0.f },
            { 0.f,                               0.f,                               0.f,                               1.f }
        ).data();
        // clang-format on
    }
    else if (name == "saturate")
    {
        filter->type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        // clang-format off
        filter->color_matrix = *(const Matrix*)Rml::Matrix4f::FromRows(
            { 0.213f + 0.787f * value,  0.715f - 0.715f * value,  0.072f - 0.072f * value,  0.f },
            { 0.213f - 0.213f * value,  0.715f + 0.285f * value,  0.072f - 0.072f * value,  0.f },
            { 0.213f - 0.213f * value,  0.715f - 0.715f * value,  0.072f + 0.928f * value,  0.f },
            { 0.f,                      0.f,                      0.f,                      1.f }
        ).data();
        // clang-format on
    }

    if (filter->type != FilterType::Invalid)
        return filterHandle;

    Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported filter type '%s'.", name.c_str());
    return {};
}

void FlaxRenderInterface::ReleaseFilter(Rml::CompiledFilterHandle filter)
{
    FilterCache[(int)filter]->Dispose();
}

Rml::CompiledShaderHandle FlaxRenderInterface::CompileShader(const Rml::String &name, const Rml::Dictionary &parameters)
{
    PROFILE_GPU("RmlUi.CompileShader");
    return Rml::CompiledShaderHandle();
}

void FlaxRenderInterface::RenderShader(Rml::CompiledShaderHandle shader_handle, Rml::CompiledGeometryHandle geometry_handle, Rml::Vector2f translation, Rml::TextureHandle texture)
{
    PROFILE_GPU("RmlUi.RenderShader");
}

void FlaxRenderInterface::ReleaseShader(Rml::CompiledShaderHandle effect_handle)
{
}

Viewport FlaxRenderInterface::GetViewport()
{
    return CurrentViewport;
}

void FlaxRenderInterface::SetViewport(int width, int height)
{
    CurrentViewport = Viewport(0, 0, (float)width, (float)height);
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
    AllocatedTextures.ClearDelete();
    GeometryCache.ClearDelete();
}

bool FlaxRenderInterface::CreateFramebuffer(FramebufferData &out_fb, int width, int height, int samples, GPUTextureView *shared_depth_stencil_buffer, GPUTextureView *outputBuffer)
{
    if (!outputBuffer)
    {

        auto texture = GPUDevice::Instance->CreateTexture();

        if (texture->Init(GPUTextureDescription::New2D(width, height, PixelFormat::B8G8R8A8_UNorm)))
            return false;

        outputBuffer = texture->View();
    }

    GPUTextureView *depth_stencil_buffer = nullptr;
    if (shared_depth_stencil_buffer)
    {
        // Share depth/stencil buffer
        depth_stencil_buffer = shared_depth_stencil_buffer;
    }

    out_fb = {};
    out_fb.width = width;
    out_fb.height = height;
    out_fb.framebuffer = outputBuffer;
    out_fb.depth_stencil_buffer = depth_stencil_buffer;
    out_fb.owns_depth_stencil_buffer = !shared_depth_stencil_buffer;

    return false;
}

void FlaxRenderInterface::DestroyFameBuffer(FramebufferData &buffer)
{
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
        FlaxRenderInterface::CreateFramebuffer(fb_layers.back(), width, height, 0, shared_depth_stencil, outputBuffer);
    }

    layers_size += 1;
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
        FlaxRenderInterface::CreateFramebuffer(fb, width, height, 0, nullptr);

    return fb;
}
