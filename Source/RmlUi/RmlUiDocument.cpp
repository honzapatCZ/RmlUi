#include "RmlUiPlugin.h"
#include "RmlUiCanvas.h"
#include "RmlUiDocument.h"

// Conflicts with both Flax and RmlUi Math.h
#undef RadiansToDegrees
#undef DegreesToRadians
#undef NormaliseAngle

#include <ThirdParty/RmlUi/Core/Context.h>
#include <ThirdParty/RmlUi/Core/ElementDocument.h>
#include <ThirdParty/RmlUi/Core/FontEngineInterface.h>

#include <Engine/Core/Log.h>
#include <Engine/Serialization/Serialization.h>
#include <Engine/Scripting/Plugins/PluginManager.h>

#include <locale>

#include "RmlUiHelpers.h"

RmlUiDocument::RmlUiDocument(const SpawnParams& params)
    : Actor(params)
{
}

void RmlUiDocument::Show() const
{
    if (GetDocument() == nullptr)
        return;

    GetDocument()->Show();

    if (AutoFocusDocument)
        Focus();
}

void RmlUiDocument::Hide() const
{
    if (!RmlUiPlugin::IsInitialized())
        return;
    if (GetDocument() == nullptr)
        return;

    GetDocument()->Hide();
}

void RmlUiDocument::Close() const
{
    if (!RmlUiPlugin::IsInitialized())
        return;
    if (GetDocument() == nullptr)
        return;

    GetDocument()->Close();
}

RmlUiElement* RmlUiDocument::CreateElement(const String& name)
{
    auto elementPointer = GetDocument()->CreateElement(ToRmlString(name));
    const auto wrappedElement = element->WrapChildElement(elementPointer.get());
    //element->wrappedChildElements.Add(wrappedElement);
    ownedElements.Add(MoveTemp(elementPointer));
    return wrappedElement;
}

RmlUiElement* RmlUiDocument::CreateTextNode(const String& text)
{
    auto elementPointer = GetDocument()->CreateTextNode(ToRmlString(text));
    const auto wrappedElement = element->WrapChildElement(elementPointer.get());
    //element->wrappedChildElements.Add(wrappedElement);
    ownedElements.Add(MoveTemp(elementPointer));
    return wrappedElement;
}

bool RmlUiDocument::HasFocus() const
{
    RmlUiCanvas* canvas = GetCanvas();
    if (canvas == nullptr)
        return false;
    return canvas->HasFocus();
}

void RmlUiDocument::Focus() const
{
    RmlUiCanvas* canvas = GetCanvas();
    if (canvas != nullptr)
        RmlUiPlugin::FocusCanvas(canvas);
}

void RmlUiDocument::Defocus() const
{
    RmlUiCanvas* canvas = GetCanvas();
    if (canvas != nullptr)
        RmlUiPlugin::DefocusCanvas(canvas);
}

RmlUiCanvas* RmlUiDocument::GetCanvas() const
{
    return Cast<RmlUiCanvas>(GetParent());
}

Rml::Context* RmlUiDocument::GetContext() const
{
    RmlUiCanvas* canvas = GetCanvas();
    if (canvas == nullptr)
        return nullptr;
    return canvas->GetContext();
}

Rml::ElementDocument* RmlUiDocument::GetDocument() const
{
    if (element == nullptr)
        return nullptr;
    return dynamic_cast<Rml::ElementDocument*>(element->GetElement());
}

bool RmlUiDocument::LoadDocument()
{
    if (Document == String::Empty)
        return false;

    Rml::Context* context = GetContext();
    if (context == nullptr)
        return false;

    if (!GetIsActive())
        return false;

    // Pre-load font assets
    const auto fontEngineInterface = Rml::GetFontEngineInterface();
    for (const auto& font : Fonts)
    {
        if (font.Font == nullptr || font.Font->WaitForLoaded())
            continue;

        StringAnsi fontPathAnsi = font.Font->GetPath().ToStringAnsi();
        Rml::String fontPath(fontPathAnsi.Get(), fontPathAnsi.Length());
        fontEngineInterface->LoadFontFace(fontPath, font.UseAsFallbackFont, Rml::Style::FontWeight::Auto);
    }

    auto documentPath = Rml::String(StringAnsi(Document).Get());

    // Fix decimal parsing issues by changing the locale
    std::locale oldLocale = std::locale::global(std::locale::classic());
    element = New<RmlUiElement>(context->LoadDocument(documentPath));
    std::locale::global(oldLocale);

    if (element == nullptr)
    {
        LOG(Error, "Failed to load RmlUiDocument with id '{0}'", Document);
        return false;
    }

    return true;
}

void RmlUiDocument::UnloadDocument()
{
    if (!RmlUiPlugin::IsInitialized())
        return;

    Rml::Context* context = GetContext();
    if (context == nullptr)
        return;
    if (element == nullptr)
        return;

    /*for (auto wrappedChild : wrappedChildElements)
    {
        Delete(wrappedChild);
    }*/
    //wrappedChildElements.ClearDelete();
    
    for (auto& el : ownedElements)
        el.release();
    ownedElements.Clear();

    context->UnloadDocument(GetDocument());
    Delete(element);
    element = nullptr;
}

bool RmlUiDocument::IsLoaded()
{
    return element != nullptr;
}

void RmlUiDocument::BeginPlay(SceneBeginData* data)
{
    if (AutoLoadDocument)
        LoadDocument();
    Actor::BeginPlay(data);
    LOG(Info, "BeginPlay");
}

void RmlUiDocument::EndPlay()
{
    UnloadDocument();
    Actor::EndPlay();
    LOG(Info, "EndPlay");
}

void RmlUiDocument::OnEnable()
{
    Show();

    LOG(Info, "Enable");

    Actor::OnEnable();
#if USE_EDITOR
    PluginManager::GetPlugin<RmlUiEditorPlugin>()->OnReload.Bind<RmlUiDocument, &RmlUiDocument::OnReload>(this);
#endif
}

void RmlUiDocument::OnDisable()
{
    Hide();
    LOG(Info, "Disable");
    Actor::OnDisable();
#if USE_EDITOR
    PluginManager::GetPlugin<RmlUiEditorPlugin>()->OnReload.Unbind<RmlUiDocument, &RmlUiDocument::OnReload>(this);
#endif
}
#if USE_EDITOR
void RmlUiDocument::OnReload(const String& file) {
    LoadDocument();
    Show();
}
#endif

void RmlUiDocument::OnParentChanged()
{
    Actor::OnParentChanged();
}

void RmlUiDocument::OnTransformChanged()
{
    Actor::OnTransformChanged();
}

#if USE_EDITOR
void RmlUiDocument::OnActiveInTreeChanged()
{
    bool active = GetIsActive();
    if (active)
    {
        if (element == nullptr && AutoLoadDocument)
            LoadDocument();
        Show();
    }
    else if (!active && element != nullptr)
    {
        Hide();
    }
    Actor::OnActiveInTreeChanged();
}
#endif