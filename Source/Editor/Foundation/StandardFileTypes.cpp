//
// Copyright (c) 2017-2020 the rbfx project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "../Foundation/StandardFileTypes.h"

#include <Urho3D/Audio/Sound.h>
#include <Urho3D/Graphics/Animation.h>
#include <Urho3D/Graphics/Material.h>
#include <Urho3D/Graphics/Model.h>
#include <Urho3D/Graphics/Texture2D.h>
#include <Urho3D/Graphics/Texture2DArray.h>
#include <Urho3D/Graphics/Texture3D.h>
#include <Urho3D/Graphics/TextureCube.h>
#include <Urho3D/Resource/BinaryFile.h>
#include <Urho3D/Resource/JSONFile.h>
#include <Urho3D/Resource/XMLFile.h>
#include <Urho3D/Scene/Scene.h>
#include <Urho3D/Utility/AssetPipeline.h>

namespace Urho3D
{

void Foundation_StandardFileTypes(Context* context, Project* project)
{
    project->AddAnalyzeFileCallback([](ResourceFileDescriptor& desc, const AnalyzeFileContext& ctx)
    {
        desc.AddObjectType<BinaryFile>();
        if (ctx.xmlFile_)
            desc.AddObjectType<XMLFile>();
        if (ctx.jsonFile_)
            desc.AddObjectType<JSONFile>();
    });

    project->AddAnalyzeFileCallback([](ResourceFileDescriptor& desc, const AnalyzeFileContext& ctx)
    {
        if (desc.HasExtension({".wav", ".ogg"}))
            desc.AddObjectType<Sound>();
    });

    project->AddAnalyzeFileCallback([](ResourceFileDescriptor& desc, const AnalyzeFileContext& ctx)
    {
        if (ctx.HasXMLRoot("scene"))
            desc.AddObjectType<Scene>();
    });

    project->AddAnalyzeFileCallback([](ResourceFileDescriptor& desc, const AnalyzeFileContext& ctx)
    {
        if (ctx.HasXMLRoot("material"))
            desc.AddObjectType<Material>();
    });

    project->AddAnalyzeFileCallback([](ResourceFileDescriptor& desc, const AnalyzeFileContext& ctx)
    {
        if (desc.HasExtension({".dds", ".bmp", ".jpg", ".jpeg", ".tga", ".png"}))
        {
            desc.AddObjectType<Texture>();
            desc.AddObjectType<Texture2D>();
        }
        else if (ctx.HasXMLRoot("cubemap"))
        {
            desc.AddObjectType<Texture>();
            desc.AddObjectType<TextureCube>();
        }
        else if (ctx.HasXMLRoot("texture3d"))
        {
            desc.AddObjectType<Texture>();
            desc.AddObjectType<Texture3D>();
        }
        else if (ctx.HasXMLRoot("texturearray"))
        {
            desc.AddObjectType<Texture>();
            desc.AddObjectType<Texture2DArray>();
        }
    });

    project->AddAnalyzeFileCallback([](ResourceFileDescriptor& desc, const AnalyzeFileContext& ctx)
    {
        if (desc.HasExtension({".mdl"}))
        {
            desc.AddObjectType<Model>();
        }
    });

    project->AddAnalyzeFileCallback([](ResourceFileDescriptor& desc, const AnalyzeFileContext& ctx)
    {
        if (desc.HasExtension({".ani"}))
        {
            desc.AddObjectType<Animation>();
        }
    });

    project->AddAnalyzeFileCallback([](ResourceFileDescriptor& desc, const AnalyzeFileContext& ctx)
    {
        if (desc.HasExtension({".AssetPipeline.json"}))
        {
            desc.AddObjectType<AssetPipeline>();
        }
    });
}

}
