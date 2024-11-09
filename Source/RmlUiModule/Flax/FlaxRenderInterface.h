#pragma once

#include <ThirdParty/RmlUi/Core/RenderInterface.h>
#include <Engine/Core/Math/Viewport.h>
#include <Engine/Core/Math/Math.h>
#include <Engine/Content/AssetReference.h>
#include <Engine/Graphics/Enums.h>
#include <Engine/Render2D/Render2D.h>
#include <Engine/Render2D/RotatedRectangle.h>

struct RenderContext;
struct CompiledGeometry;
class Asset;
class GPUContext;
class GPUTexture;
class GPUTextureView;
class Texture;

/// <summary>
/// The RenderInterface implementation for Flax Engine.
/// </summary>
class FlaxRenderInterface : public Rml::RenderInterface
{
public:
    // [Rml::RenderInterface]
    FlaxRenderInterface();
    ~FlaxRenderInterface() override;

    //void RenderGeometry(Rml::Vertex* vertices, int num_vertices, int* indices, int num_indices, Rml::TextureHandle texture, const Rml::Vector2f& translation) override;
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

    void EnableClipMask(bool enable) override;
    void RenderToClipMask(Rml::ClipMaskOperation mask_operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) override;

    void SetTransform(const Rml::Matrix4f* transform) override;

    Rml::LayerHandle PushLayer() override;
    void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend_mode,
        Rml::Span<const Rml::CompiledFilterHandle> filters) override;
    void PopLayer() override;

    Rml::TextureHandle SaveLayerAsTexture() override;

    Rml::CompiledFilterHandle SaveLayerAsMaskImage() override;

    Rml::CompiledFilterHandle CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) override;
    void ReleaseFilter(Rml::CompiledFilterHandle filter) override;

    Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override;
    void RenderShader(Rml::CompiledShaderHandle shader_handle, Rml::CompiledGeometryHandle geometry_handle, Rml::Vector2f translation,
        Rml::TextureHandle texture) override;
    void ReleaseShader(Rml::CompiledShaderHandle effect_handle) override;

public:
    Viewport GetViewport();
    void SetViewport(int width, int height);
    void SetViewport(Viewport view);
    void SetScissor(Rectangle scissor);

    void InvalidateShaders(Asset* obj = nullptr);
    bool InitShaders();
    void Begin(RenderContext* renderContext, GPUContext* context, Viewport viewport);
    void End();
    void CompileGeometry(CompiledGeometry* compiledGeometry, const Rml::Vertex* vertices, int num_vertices, const int* indices, int num_indices);
    void RenderCompiledGeometry(CompiledGeometry* compiledGeometry, const Rml::Vector2f& translation, Rml::TextureHandle texture);
    void RenderGeometryWithPipeline(CompiledGeometry* compiledGeometry, const Rml::Vector2f& translation, GPUTexture* texture, GPUPipelineState* pipeline);
    void HookGenerateTexture(Rml::TextureHandle textureHandle);
    Rml::TextureHandle GetTextureHandle(GPUTexture* texture);
    Rml::TextureHandle RegisterTexture(GPUTexture* texture, bool isFontTexture = false);
    void ReleaseResources();
    
private:
    Rml::TextureHandle _generateTextureOverride = {};
    struct FramebufferData {
        int width, height;
        GPUTextureView* framebuffer = (GPUTextureView*)nullptr;
        /*
        GPUTexture* color_tex_buffer;
        GPUTexture* color_render_buffer;
        */
        GPUTextureView* depth_stencil_buffer = (GPUTextureView*)nullptr;
        bool owns_depth_stencil_buffer;
    };
    enum class FramebufferAttachment { None, DepthStencil };

    void BlitTexturesUV(GPUTextureView* sourceView, Float4 source, GPUTextureView* destinationView, Float4 destination);

    void BlitTexturesUV(FramebufferData sourceData, Float4 source, FramebufferData destinationData, Float4 destination) {
        BlitTexturesUV(sourceData.framebuffer, source, destinationData.framebuffer, destination);
    }
    void BlitTextures(FramebufferData sourceData, Rectangle source, FramebufferData destinationData, Rectangle destination);
    void BlitTextures(FramebufferData sourceData, FramebufferData destinationData);
    void BlitTexturePostProcessPrimary(FramebufferData source) {
        const auto destination = render_layers.GetPostprocessPrimary();

        BlitTextures(source, destination);
    }

    void RenderBlur(float sigma, const FramebufferData& source_destination, const FramebufferData& temp, const Rectangle window_flipped);
    void RenderFilters(Rml::Span<const Rml::CompiledFilterHandle> filter_handles);

    static bool CreateFramebuffer(FramebufferData& outBuffer, int width, int height, MSAALevel samples, FramebufferAttachment attachment, GPUTextureView* shared_depth_stencil_buffer, GPUTextureView* outputBuffer = nullptr);
    static void DestroyFameBuffer(FramebufferData& buffer);

    void SetupRenderTarget(FramebufferData data, bool allowScissor = true);

    class RenderLayerStack {
    public:
        RenderLayerStack();
        ~RenderLayerStack();

        // Push a new layer. All references to previously retrieved layers are invalidated.
        Rml::LayerHandle PushLayer(GPUTextureView* outputBuffer = nullptr);

        // Pop the top layer. All references to previously retrieved layers are invalidated.
        void PopLayer();

        const FramebufferData& GetLayer(Rml::LayerHandle layer) const;
        const FramebufferData& GetTopLayer() const;
        Rml::LayerHandle GetTopLayerHandle() const;

        const FramebufferData& GetPostprocessPrimary() { return EnsureFramebufferPostprocess(0); }
        const FramebufferData& GetPostprocessSecondary() { return EnsureFramebufferPostprocess(1); }
        const FramebufferData& GetPostprocessTertiary() { return EnsureFramebufferPostprocess(2); }
        const FramebufferData& GetBlendMask() { return EnsureFramebufferPostprocess(3); }

        void SwapPostprocessPrimarySecondary();

        void BeginFrame(int new_width, int new_height, GPUTextureView* outputView);
        void EndFrame();

    private:
        void DestroyFramebuffers(bool force = false);
        const FramebufferData& EnsureFramebufferPostprocess(int index);

        int width = 0, height = 0;

        // The number of active layers is manually tracked since we re-use the framebuffers stored in the fb_layers stack.
        int layers_size = 0;

        Rml::Vector<FramebufferData> fb_layers;
        Rml::Vector<FramebufferData> fb_postprocess;
    };

    RenderLayerStack render_layers;
};