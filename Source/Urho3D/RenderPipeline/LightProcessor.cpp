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

#include "../Precompiled.h"

#include "../Core/Context.h"
#include "../Core/IteratorRange.h"
#include "../Core/WorkQueue.h"
#include "../Math/NumericRange.h"
#include "../Math/Polyhedron.h"
#include "../Graphics/Camera.h"
#include "../Graphics/Octree.h"
#include "../Graphics/OctreeQuery.h"
#include "../Graphics/Renderer.h"
#include "../Graphics/Texture2D.h"
#include "../RenderPipeline/LightProcessor.h"
#include "../RenderPipeline/LightProcessorQuery.h"
#include "../Scene/Node.h"

#include <EASTL/fixed_vector.h>

#include "../DebugNew.h"

namespace Urho3D
{

namespace
{

/// Cube shadow map padding, in pixels.
const float cubeShadowMapPadding = 2.0f;

/// Return current light fade.
float GetLightFade(Light* light)
{
    const float fadeStart = light->GetFadeDistance();
    const float fadeEnd = light->GetDrawDistance();
    if (light->GetLightType() != LIGHT_DIRECTIONAL && fadeEnd > 0.0f && fadeStart > 0.0f && fadeStart < fadeEnd)
        return ea::min(1.0f - (light->GetDistance() - fadeStart) / (fadeEnd - fadeStart), 1.0f);
    return 1.0f;
}

/// Return spot light matrix.
Matrix4 CalculateSpotMatrix(Light* light)
{
    Node* lightNode = light->GetNode();
    const Matrix3x4 spotView = Matrix3x4(lightNode->GetWorldPosition(), lightNode->GetWorldRotation(), 1.0f).Inverse();

    // Make the projected light slightly smaller than the shadow map to prevent light spill
    Matrix4 spotProj = Matrix4::ZERO;
    const float h = 1.005f / tanf(light->GetFov() * M_DEGTORAD * 0.5f);
    const float w = h / light->GetAspectRatio();
    spotProj.m00_ = w;
    spotProj.m11_ = h;
    spotProj.m22_ = 1.0f / Max(light->GetRange(), M_EPSILON);
    spotProj.m32_ = 1.0f;

    Matrix4 texAdjust;
#ifdef URHO3D_OPENGL
    texAdjust.SetTranslation(Vector3(0.5f, 0.5f, 0.5f));
    texAdjust.SetScale(Vector3(0.5f, -0.5f, 0.5f));
#else
    texAdjust.SetTranslation(Vector3(0.5f, 0.5f, 0.0f));
    texAdjust.SetScale(Vector3(0.5f, -0.5f, 1.0f));
#endif

    return texAdjust * spotProj * spotView;
}

/// Return expected number of splits.
unsigned CalculateNumSplits(Light* light)
{
    switch (light->GetLightType())
    {
    case LIGHT_SPOT:
        return 1;
    case LIGHT_POINT:
        return MAX_CUBEMAP_FACES;
    case LIGHT_DIRECTIONAL:
        return static_cast<unsigned>(light->GetNumShadowSplits());
    default:
        return 0;
    }
}

/// Return effective splits of directional light.
ea::fixed_vector<FloatRange, MAX_CASCADE_SPLITS> GetActiveSplits(Light* light, float nearClip, float farClip)
{
    const CascadeParameters& cascade = light->GetShadowCascade();
    const int numSplits = light->GetNumShadowSplits();

    ea::fixed_vector<FloatRange, MAX_CASCADE_SPLITS> result;

    float nearSplit = nearClip;
    for (unsigned i = 0; i < numSplits; ++i)
    {
        // Stop if split is completely beyond camera far clip
        if (nearSplit > farClip)
            break;

        const float farSplit = ea::min(farClip, cascade.splits_[i]);
        if (farSplit <= nearSplit)
            break;

        result.emplace_back(nearSplit, farSplit);
        nearSplit = farSplit;
    }

    return result;
}

/// Return lower bound of distance from light to camera.
float EstimateDistanceToCamera(Camera* cullCamera, Light* light)
{
    const Vector3 cameraPos = cullCamera->GetNode()->GetWorldPosition();
    switch (light->GetLightType())
    {
    case LIGHT_DIRECTIONAL:
        return 0.0f;
    case LIGHT_POINT:
        return Sphere(light->GetNode()->GetWorldPosition(), light->GetRange() * 1.25f).Distance(cameraPos);
    case LIGHT_SPOT:
        return light->GetFrustum().Distance(cameraPos);
    default:
        return 0.0f;
    }
}

}

LightProcessor::LightProcessor(Light* light)
    : light_(light)
{
}

LightProcessor::~LightProcessor()
{
}

void LightProcessor::BeginUpdate(DrawableProcessor* drawableProcessor, LightProcessorCallback* callback)
{
    // Clear temporary containers
    litGeometries_.clear();
    shadowCasterCandidates_.clear();
    shadowMap_ = {};

    // Initialize shadow
    isShadowRequested_ = callback->IsLightShadowed(light_);
    numSplitsRequested_ = isShadowRequested_ ? CalculateNumSplits(light_) : 0;

    // Update splits
    if (splits_.size() <= numSplitsRequested_)
    {
        // Allocate splits and reset timer immediately
        splitRemainingTimeToLive_ = NumSplitFramesToLive;
        while (splits_.size() < numSplitsRequested_)
            splits_.emplace_back(this, splits_.size());
    }
    else
    {
        // Deallocate splits by timeout
        --splitRemainingTimeToLive_;
        if (splitRemainingTimeToLive_ == 0)
        {
            while (splits_.size() > numSplitsRequested_)
                splits_.pop_back();
        }
    }
}

void LightProcessor::Update(DrawableProcessor* drawableProcessor)
{
    const FrameInfo& frameInfo = drawableProcessor->GetFrameInfo();
    Octree* octree = frameInfo.octree_;
    Camera* cullCamera = frameInfo.cullCamera_;
    const LightType lightType = light_->GetLightType();

    // Check if light volume contains camera
    cameraIsInsideLightVolume_ = EstimateDistanceToCamera(cullCamera, light_) < cullCamera->GetNearClip() * 2.0f;

    // Query lit geometries (and shadow casters for spot and point lights)
    switch (lightType)
    {
    case LIGHT_SPOT:
    {
        SpotLightGeometryQuery query(litGeometries_, hasLitGeometries_,
            isShadowRequested_ ? &shadowCasterCandidates_ : nullptr,
            drawableProcessor, light_, cullCamera->GetViewMask());
        octree->GetDrawables(query);
        hasForwardLitGeometries_ = !litGeometries_.empty();
        break;
    }
    case LIGHT_POINT:
    {
        PointLightGeometryQuery query(litGeometries_, hasLitGeometries_,
            isShadowRequested_ ? &shadowCasterCandidates_ : nullptr,
            drawableProcessor, light_, cullCamera->GetViewMask());
        octree->GetDrawables(query);
        hasForwardLitGeometries_ = !litGeometries_.empty();
        break;
    }
    case LIGHT_DIRECTIONAL:
    {
        cameraIsInsideLightVolume_ = true;
        hasLitGeometries_ = false;
        hasForwardLitGeometries_ = false;
        const unsigned lightMask = light_->GetLightMask();
        for (Drawable* drawable : drawableProcessor->GetGeometries())
        {
            const unsigned drawableIndex = drawable->GetDrawableIndex();
            const unsigned char flags = drawableProcessor->GetGeometryRenderFlags(drawableIndex);
            const bool isLit = !!(flags & GeometryRenderFlag::Lit);
            const bool isForwardLit = !!(flags & GeometryRenderFlag::ForwardLit);

            hasLitGeometries_ = hasLitGeometries_ || isLit;
            hasForwardLitGeometries_ = hasForwardLitGeometries_ || isForwardLit;

            if (isLit && (drawable->GetLightMaskInZone() & lightMask))
                litGeometries_.push_back(drawable);
        };
        break;
    }
    }

    // Update shadows
    if (!isShadowRequested_)
    {
        numActiveSplits_ = 0;
        return;
    }

    InitializeShadowSplits(drawableProcessor);

    for (unsigned i = 0; i < numActiveSplits_; ++i)
    {
        switch (lightType)
        {
        case LIGHT_SPOT:
            splits_[i].ProcessSpotShadowCasters(drawableProcessor, shadowCasterCandidates_);
            break;
        case LIGHT_POINT:
            splits_[i].ProcessPointShadowCasters(drawableProcessor, shadowCasterCandidates_);
            break;
        case LIGHT_DIRECTIONAL:
            splits_[i].ProcessDirectionalShadowCasters(drawableProcessor, shadowCasterCandidates_);
            break;
        default:
            break;
        }
    }

    const auto hasShadowCaster = [](const ShadowSplitProcessor& split) { return split.HasShadowCasters(); };
    if (!ea::any_of(splits_.begin(), splits_.begin() + numActiveSplits_, hasShadowCaster))
    {
        numActiveSplits_ = 0;
        return;
    }

    // Evaluate split shadow map size
    // TODO(renderer): Implement me
    shadowMapSplitSize_ = light_->GetLightType() != LIGHT_POINT ? 512 : 256;
    shadowMapSize_ = IntVector2{ shadowMapSplitSize_, shadowMapSplitSize_ } * GetNumSplitsInGrid();
}

void LightProcessor::EndUpdate(DrawableProcessor* drawableProcessor, LightProcessorCallback* callback)
{
    // Allocate shadow map
    if (numActiveSplits_ > 0)
    {
        shadowMap_ = callback->AllocateTransientShadowMap(shadowMapSize_);
        if (!shadowMap_)
            numActiveSplits_ = 0;
        else
        {
            for (unsigned i = 0; i < numActiveSplits_; ++i)
                splits_[i].FinalizeShadow(shadowMap_.GetSplit(i, GetNumSplitsInGrid()));
        }
    }

    // TODO(renderer): Fill second parameter
    Camera* cullCamera = drawableProcessor->GetFrameInfo().cullCamera_;
    CookShaderParameters(cullCamera, 0.0f);
    UpdateHashes();
}

void LightProcessor::InitializeShadowSplits(DrawableProcessor* drawableProcessor)
{
    /// Setup splits
    switch (light_->GetLightType())
    {
    case LIGHT_DIRECTIONAL:
    {
        const FrameInfo& frameInfo = drawableProcessor->GetFrameInfo();
        Camera* cullCamera = frameInfo.cullCamera_;
        const auto activeSplits = GetActiveSplits(light_, cullCamera->GetNearClip(), cullCamera->GetFarClip());

        numActiveSplits_ = activeSplits.size();
        for (unsigned i = 0; i < numActiveSplits_; ++i)
            splits_[i].InitializeDirectional(drawableProcessor, activeSplits[i], litGeometries_);
        break;
    }
    case LIGHT_SPOT:
    {
        numActiveSplits_ = 1;
        splits_[0].InitializeSpot();
        break;
    }
    case LIGHT_POINT:
    {
        numActiveSplits_ = MAX_CUBEMAP_FACES;
        for (unsigned i = 0; i < MAX_CUBEMAP_FACES; ++i)
            splits_[i].InitializePoint(static_cast<CubeMapFace>(i));
        break;
    }
    }

}

void LightProcessor::CookShaderParameters(Camera* cullCamera, float subPixelOffset)
{
    Node* lightNode = light_->GetNode();
    const LightType lightType = light_->GetLightType();

    // Setup resources
    auto renderer = light_->GetSubsystem<Renderer>();
    cookedParams_.shadowMap_ = shadowMap_.texture_;
    cookedParams_.lightRamp_ = light_->GetRampTexture() ? light_->GetRampTexture() : renderer->GetDefaultLightRamp();
    cookedParams_.lightShape_ = light_->GetShapeTexture() ? light_->GetShapeTexture() : renderer->GetDefaultLightSpot();

    // Setup common shader parameters
    cookedParams_.position_ = lightNode->GetWorldPosition();
    cookedParams_.direction_ = lightNode->GetWorldRotation() * Vector3::BACK;
    cookedParams_.inverseRange_ = lightType == LIGHT_DIRECTIONAL ? 0.0f : 1.0f / Max(light_->GetRange(), M_EPSILON);
    cookedParams_.volumetricRadius_ = light_->GetRadius();
    cookedParams_.volumetricLength_ = light_->GetLength();

    // Negative lights will use subtract blending, so use absolute RGB values
    const float fade = GetLightFade(light_);
    cookedParams_.effectiveColorInGammaSpace_ = fade * light_->GetEffectiveColor().Abs().ToVector3();
    cookedParams_.effectiveColorInLinearSpace_ = fade * light_->GetEffectiveColor().Abs().GammaToLinear().ToVector3();
    cookedParams_.effectiveSpecularIntensity_ = fade * light_->GetEffectiveSpecularIntensity();

    // Setup vertex light parameters
    if (lightType == LIGHT_SPOT)
    {
        cookedParams_.spotCutoff_ = Cos(light_->GetFov() * 0.5f);
        cookedParams_.inverseSpotCutoff_ = 1.0f / (1.0f - cookedParams_.spotCutoff_);
    }
    else
    {
        cookedParams_.spotCutoff_ = -2.0f;
        cookedParams_.inverseSpotCutoff_ = 1.0f;
    }

    // TODO(renderer): Skip this step if there's no cookies
    switch (lightType)
    {
    case LIGHT_DIRECTIONAL:
        cookedParams_.numLightMatrices_ = 0;
        break;
    case LIGHT_SPOT:
        cookedParams_.lightMatrices_[0] = CalculateSpotMatrix(light_);
        cookedParams_.numLightMatrices_ = 1;
        break;
    case LIGHT_POINT:
        cookedParams_.lightMatrices_[0] = lightNode->GetWorldRotation().RotationMatrix();
        cookedParams_.numLightMatrices_ = 1;
        break;
    default:
        break;
    }

    // Skip the rest if no shadow
    if (!shadowMap_)
        return;

    // Initialize size of shadow map
    const float textureSizeX = static_cast<float>(shadowMap_.texture_->GetWidth());
    const float textureSizeY = static_cast<float>(shadowMap_.texture_->GetHeight());
    cookedParams_.shadowMapInvSize_ = { 1.0f / textureSizeX, 1.0f / textureSizeY };

    cookedParams_.shadowCubeUVBias_ = Vector2::ZERO;
    cookedParams_.shadowCubeAdjust_ = Vector4::ZERO;
    switch (lightType)
    {
    case LIGHT_DIRECTIONAL:
        cookedParams_.numLightMatrices_ = MAX_CASCADE_SPLITS;
        for (unsigned splitIndex = 0; splitIndex < numActiveSplits_; ++splitIndex)
            cookedParams_.lightMatrices_[splitIndex] = splits_[splitIndex].GetWorldToShadowSpaceMatrix(subPixelOffset);
        break;

    case LIGHT_SPOT:
        cookedParams_.numLightMatrices_ = 2;
        cookedParams_.lightMatrices_[1] = splits_[0].GetWorldToShadowSpaceMatrix(subPixelOffset);
        break;

    case LIGHT_POINT:
    {
        const auto& splitViewport = splits_[0].GetShadowMap().rect_;
        const float viewportSizeX = static_cast<float>(splitViewport.Width());
        const float viewportSizeY = static_cast<float>(splitViewport.Height());
        const float viewportOffsetX = static_cast<float>(splitViewport.Left());
        const float viewportOffsetY = static_cast<float>(splitViewport.Top());
        const Vector2 relativeViewportSize{ viewportSizeX / textureSizeX, viewportSizeY / textureSizeY };
        const Vector2 relativeViewportOffset{ viewportOffsetX / textureSizeX, viewportOffsetY / textureSizeY };
        cookedParams_.shadowCubeUVBias_ =
            Vector2::ONE - 2.0f * cubeShadowMapPadding * cookedParams_.shadowMapInvSize_ / relativeViewportSize;
#ifdef URHO3D_OPENGL
        const Vector2 scale = relativeViewportSize * Vector2(1, -1);
        const Vector2 offset = Vector2(0, 1) + relativeViewportOffset * Vector2(1, -1);
#else
        const Vector2 scale = relativeViewportSize;
        const Vector2 offset = relativeViewportOffset;
#endif
        cookedParams_.shadowCubeAdjust_ = { scale, offset };
        break;
    }
    default:
        break;
    }

    {
        // Calculate shadow camera depth parameters for point light shadows and shadow fade parameters for
        //  directional light shadows, stored in the same uniform
        Camera* shadowCamera = splits_[0].GetShadowCamera();
        const float nearClip = shadowCamera->GetNearClip();
        const float farClip = shadowCamera->GetFarClip();
        const float q = farClip / (farClip - nearClip);
        const float r = -q * nearClip;

        const CascadeParameters& parameters = light_->GetShadowCascade();
        const float viewFarClip = cullCamera->GetFarClip();
        const float shadowRange = parameters.GetShadowRange();
        const float fadeStart = parameters.fadeStart_ * shadowRange / viewFarClip;
        const float fadeEnd = shadowRange / viewFarClip;
        const float fadeRange = fadeEnd - fadeStart;

        cookedParams_.shadowDepthFade_ = { q, r, fadeStart, 1.0f / fadeRange };
    }

    {
        float intensity = light_->GetShadowIntensity();
        const float fadeStart = light_->GetShadowFadeDistance();
        const float fadeEnd = light_->GetShadowDistance();
        if (fadeStart > 0.0f && fadeEnd > 0.0f && fadeEnd > fadeStart)
            intensity =
                Lerp(intensity, 1.0f, Clamp((light_->GetDistance() - fadeStart) / (fadeEnd - fadeStart), 0.0f, 1.0f));
        const float pcfValues = (1.0f - intensity);
        float samples = 1.0f;
        // TODO(renderer): Support me
        //if (renderer->GetShadowQuality() == SHADOWQUALITY_PCF_16BIT || renderer->GetShadowQuality() == SHADOWQUALITY_PCF_24BIT)
        //    samples = 4.0f;
        cookedParams_.shadowIntensity_ = { pcfValues / samples, intensity, 0.0f, 0.0f };
    }

    cookedParams_.shadowSplitDistances_ = { M_LARGE_VALUE, M_LARGE_VALUE, M_LARGE_VALUE, M_LARGE_VALUE };
    if (numActiveSplits_ > 1)
        cookedParams_.shadowSplitDistances_.x_ = splits_[0].GetCascadeZRange().second / cullCamera->GetFarClip();
    if (numActiveSplits_ > 2)
        cookedParams_.shadowSplitDistances_.y_ = splits_[1].GetCascadeZRange().second / cullCamera->GetFarClip();
    if (numActiveSplits_ > 3)
        cookedParams_.shadowSplitDistances_.z_ = splits_[2].GetCascadeZRange().second / cullCamera->GetFarClip();

    // TODO(renderer): Implement me
    //cookedParams_.shadowNormalBias_ = light_->GetShadowBias().normalOffset_;
    //cookedParams_.shadowNormalBias_ = Vector4::ZERO;
    /*if (light->GetShadowBias().normalOffset_ > 0.0f)
    {
        Vector4 normalOffsetScale(Vector4::ZERO);

        // Scale normal offset strength with the width of the shadow camera view
        if (light->GetLightType() != LIGHT_DIRECTIONAL)
        {
            Camera* shadowCamera = lightQueue_->shadowSplits_[0].shadowCamera_;
            normalOffsetScale.x_ = 2.0f * tanf(shadowCamera->GetFov() * M_DEGTORAD * 0.5f) * shadowCamera->GetFarClip();
        }
        else
        {
            normalOffsetScale.x_ = lightQueue_->shadowSplits_[0].shadowCamera_->GetOrthoSize();
            if (lightQueue_->shadowSplits_.size() > 1)
                normalOffsetScale.y_ = lightQueue_->shadowSplits_[1].shadowCamera_->GetOrthoSize();
            if (lightQueue_->shadowSplits_.size() > 2)
                normalOffsetScale.z_ = lightQueue_->shadowSplits_[2].shadowCamera_->GetOrthoSize();
            if (lightQueue_->shadowSplits_.size() > 3)
                normalOffsetScale.w_ = lightQueue_->shadowSplits_[3].shadowCamera_->GetOrthoSize();
        }

        normalOffsetScale *= light->GetShadowBias().normalOffset_;
#ifdef GL_ES_VERSION_2_0
        normalOffsetScale *= renderer->GetMobileNormalOffsetMul();
#endif
    }*/

    cookedParams_.shadowDepthBiasMultiplier_.fill(1.0f);
    if (light_->GetLightType() == LIGHT_DIRECTIONAL)
    {
        const float biasAutoAdjust = light_->GetShadowCascade().biasAutoAdjust_;
        for (unsigned i = 1; i < numActiveSplits_; ++i)
        {
            const float splitScale = ea::max(1.0f, splits_[i].GetCascadeZRange().second / splits_[0].GetCascadeZRange().second);
            const float multiplier = 1.0f + (splitScale - 1.0f) * biasAutoAdjust;
            cookedParams_.shadowDepthBiasMultiplier_[i] = SnapRound(multiplier, 0.1f);
        }
    }

    const float normalOffset = light_->GetShadowBias().normalOffset_;
    for (unsigned i = 0; i < numActiveSplits_; ++i)
        cookedParams_.shadowNormalBias_[i] = splits_[i].GetShadowMapTexelSizeInWorldSpace() * normalOffset;
}

void LightProcessor::UpdateHashes()
{
    const BiasParameters& biasParameters = light_->GetShadowBias();

    unsigned commonHash = 0;
    CombineHash(commonHash, light_->GetLightType());
    CombineHash(commonHash, HasShadow());
    CombineHash(commonHash, !!light_->GetShapeTexture());
    CombineHash(commonHash, light_->GetSpecularIntensity() > 0.0f);
    CombineHash(commonHash, biasParameters.normalOffset_ > 0.0f);
    CombineHash(commonHash, MakeHash(biasParameters.constantBias_ * 1000000.0f));
    CombineHash(commonHash, MakeHash(biasParameters.slopeScaledBias_ * 1000.0f));
    CombineHash(commonHash, light_->GetLightMaskEffective() & PORTABLE_LIGHTMASK);

    forwardLitBatchHash_ = commonHash;

    lightVolumeBatchHash_ = commonHash;
    CombineHash(lightVolumeBatchHash_, cameraIsInsideLightVolume_);

    if (light_->GetLightType() != LIGHT_DIRECTIONAL)
        shadowBatchStateHashes_.fill(commonHash);
    else
    {
        for (unsigned i = 0; i < numActiveSplits_; ++i)
        {
            shadowBatchStateHashes_[i] = commonHash;
            CombineHash(shadowBatchStateHashes_[i], MakeHash(100.0f * cookedParams_.shadowDepthBiasMultiplier_[i]));
        }
    }
}

IntVector2 LightProcessor::GetNumSplitsInGrid() const
{
    if (numActiveSplits_ == 1)
        return { 1, 1 };
    else if (numActiveSplits_ == 2)
        return { 2, 1 };
    else if (numActiveSplits_ < 6)
        return { 2, 2 };
    else
        return { 3, 2 };
}

LightProcessorCache::LightProcessorCache()
{
}

LightProcessorCache::~LightProcessorCache()
{
}

LightProcessor* LightProcessorCache::GetLightProcessor(Light* light)
{
    WeakPtr<Light> weakLight(light);
    auto& lightProcessor = cache_[weakLight];
    if (!lightProcessor)
        lightProcessor = ea::make_unique<LightProcessor>(light);
    return lightProcessor.get();
}

}