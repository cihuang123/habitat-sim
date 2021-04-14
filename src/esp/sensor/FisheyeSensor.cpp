// Copyright (c) Facebook, Inc. and its affiliates.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
#include "FisheyeSensor.h"
#include "esp/core/Check.h"
#include "esp/gfx/DoubleSphereCameraShader.h"

#include <Corrade/Utility/Assert.h>
#include <Corrade/Utility/FormatStl.h>

namespace Mn = Magnum;
namespace Cr = Corrade;

namespace esp {
namespace sensor {

FisheyeSensorSpec::FisheyeSensorSpec() : VisualSensorSpec() {
  uuid = "fisheye";
  sensorSubType = SensorSubType::Fisheye;
}

void FisheyeSensorSpec::sanityCheck() {
  VisualSensorSpec::sanityCheck();
  ESP_CHECK(sensorSubType == SensorSubType::Fisheye,
            "FisheyeSensorSpec::sanityCheck(): sensorSpec is not Fisheye");

  ESP_CHECK(focalLength[0] > 0 && focalLength[1] > 0,
            "FisheyeSensorSpec::sanityCheck(): focal length," << focalLength
                                                              << "is illegal.");
  if (cubemapSize != Cr::Containers::NullOpt) {
    ESP_CHECK(*cubemapSize > 0,
              "FisheyeSensorSpec::sanityCheck(): the size of the cubemap,"
                  << *cubemapSize << "is illegal.");
  }
}

void FisheyeSensorDoubleSphereSpec::sanityCheck() {
  FisheyeSensorSpec::sanityCheck();
  ESP_CHECK(fisheyeModelType == FisheyeSensorModelType::DoubleSphere,
            "FisheyeSensorDoubleSphereSpec::sanityCheck(): fisheye model "
            "type is wrong. It should be DoubleSphere.");
  ESP_CHECK(alpha >= 0 && alpha <= 1.0,
            "FisheyeSensorDoubleSphereSpec::sanityCheck(): alpha"
                << alpha << "is illegal.");
}

template <typename T>
void specSanityCheck(FisheyeSensorSpec* spec) {
  auto actualSpec = dynamic_cast<T*>(spec);
  ESP_CHECK(actualSpec,
            "FisheyeSensor::FisheyeSensor(): the spec cannot be converted "
            "to any known fisheye sensor spec.");
  actualSpec->sanityCheck();
}

int computeCubemapSize(const esp::vec2i& resolution,
                       const Cr::Containers::Optional<int>& cubemapSize) {
  int size = resolution[0] < resolution[1] ? resolution[0] : resolution[1];
  // if user sets the size of the cubemap, use it
  if (cubemapSize != Corrade::Containers::NullOpt) {
    size = *cubemapSize;
  }
  return size;
}

FisheyeSensor::FisheyeSensor(scene::SceneNode& cameraNode,
                             const FisheyeSensorSpec::ptr& spec)
    : VisualSensor(cameraNode, spec) {
  // switch (fisheyeSensorSpec_->fisheyeModelType) {
  //   case FisheyeSensorModelType::DoubleSphere: {
  //     specSanityCheck<FisheyeSensorDoubleSphereSpec>(fisheyeSensorSpec_.get());
  //   } break;
  // };

  // initialize a cubemap
  int size = computeCubemapSize(fisheyeSensorSpec_->resolution,
                                fisheyeSensorSpec_->cubemapSize);
  gfx::CubeMap::Flags cubeMapFlags = {};
  switch (fisheyeSensorSpec_->sensorType) {
    case SensorType::Color:
      cubeMapFlags |= gfx::CubeMap::Flag::ColorTexture;
      break;
    case SensorType::Depth:
      cubeMapFlags |= gfx::CubeMap::Flag::DepthTexture;
      break;
    // TODO: Semantic
    default:
      CORRADE_INTERNAL_ASSERT_UNREACHABLE();
      break;
  }
  cubeMap_ = esp::gfx::CubeMap{size, cubeMapFlags};

  // initialize the cubemap camera, it attaches to the same node as the sensor
  // You do not have to release it in the dtor since magnum scene graph will
  // handle it
  cubeMapCamera_ = new gfx::CubeMapCamera(cameraNode);
  cubeMapCamera_->setProjectionMatrix(size, fisheyeSensorSpec_->near,
                                      fisheyeSensorSpec_->far);

  // setup shader flags
  switch (fisheyeSensorSpec_->sensorType) {
    case SensorType::Color:
      fisheyeShaderFlags_ |= gfx::FisheyeShader::Flag::ColorTexture;
      break;
    case SensorType::Depth:
      fisheyeShaderFlags_ |= gfx::FisheyeShader::Flag::DepthTexture;
      break;
    // TODO: Semantic
    // sensor type list is too long, have to use default
    default:
      CORRADE_INTERNAL_ASSERT_UNREACHABLE();
      break;
  }
  // prepare a big triangle mesh to cover the screen
  mesh_ = Mn::GL::Mesh{};
  mesh_.setCount(3);
}

bool FisheyeSensorSpec::operator==(const FisheyeSensorSpec& a) const {
  return VisualSensorSpec::operator==(a) &&
         fisheyeModelType == a.fisheyeModelType &&
         focalLength == a.focalLength &&
         principalPointOffset == a.principalPointOffset;
}

Mn::ResourceKey FisheyeSensor::getShaderKey() {
  return Cr::Utility::formatString(
      FISH_EYE_SHADER_KEY_TEMPLATE,
      static_cast<Mn::UnsignedInt>(fisheyeSensorSpec_->fisheyeModelType),
      static_cast<gfx::FisheyeShader::Flags::UnderlyingType>(
          fisheyeShaderFlags_));
}

bool FisheyeSensor::drawObservation(sim::Simulator& sim) {
  if (!hasRenderTarget()) {
    return false;
  }

  // in case the fisheye sensor resolution changed at runtime
  {
    int size = computeCubemapSize(fisheyeSensorSpec_->resolution,
                                  fisheyeSensorSpec_->cubemapSize);
    bool reset = cubeMap_->reset(size);
    if (reset) {
      cubeMapCamera_->setProjectionMatrix(size, fisheyeSensorSpec_->near,
                                          fisheyeSensorSpec_->far);
    }
  }

  esp::gfx::RenderCamera::Flags flags = {gfx::RenderCamera::Flag::ClearColor |
                                         gfx::RenderCamera::Flag::ClearDepth};
  if (sim.isFrustumCullingEnabled()) {
    flags |= gfx::RenderCamera::Flag::FrustumCulling;
  }

  // generate the cubemap texture
  if (fisheyeSensorSpec_->sensorType == SensorType::Semantic) {
    bool twoSceneGraphs =
        (&sim.getActiveSemanticSceneGraph() != &sim.getActiveSceneGraph());

    if (twoSceneGraphs) {
      VisualSensor::MoveSemanticSensorNodeHelper helper(*this, sim);
      cubeMap_->renderToTexture(*cubeMapCamera_,
                                sim.getActiveSemanticSceneGraph(), flags);
    } else {
      cubeMap_->renderToTexture(*cubeMapCamera_,
                                sim.getActiveSemanticSceneGraph(), flags);
    }

    if (twoSceneGraphs) {
      flags |= gfx::RenderCamera::Flag::ObjectsOnly;
      // Incremental rendering:
      // BE AWARE that here "ClearColor" and "ClearDepth" is NOT set!!
      // Rendering happens on top of whatever existing there.
      flags &= ~gfx::RenderCamera::Flag::ClearColor;
      flags &= ~gfx::RenderCamera::Flag::ClearDepth;
      cubeMap_->renderToTexture(*cubeMapCamera_, sim.getActiveSceneGraph(),
                                flags);
    }
  } else {
    cubeMap_->renderToTexture(*cubeMapCamera_, sim.getActiveSceneGraph(),
                              flags);
  }

  auto drawWith = [&](auto& shader) {
    if (fisheyeSensorSpec_->sensorType == SensorType::Color) {
      shader.bindColorTexture(
          cubeMap_->getTexture(gfx::CubeMap::TextureType::Color));
    }
    if (fisheyeSensorSpec_->sensorType == SensorType::Depth) {
      shader.bindDepthTexture(
          cubeMap_->getTexture(gfx::CubeMap::TextureType::Depth));
    }
    renderTarget().renderEnter();
    shader.draw(mesh_);
    renderTarget().renderExit();
  };

  switch (fisheyeSensorSpec_->fisheyeModelType) {
    case FisheyeSensorModelType::DoubleSphere: {
      Magnum::Resource<gfx::FisheyeShader, gfx::DoubleSphereCameraShader>
          shader = getShader<gfx::DoubleSphereCameraShader>();
      auto& actualSpec =
          static_cast<FisheyeSensorDoubleSphereSpec&>(*fisheyeSensorSpec_);
      (*shader)
          .setFocalLength(actualSpec.focalLength)
          .setPrincipalPointOffset(actualSpec.principalPointOffset)
          .setAlpha(actualSpec.alpha)
          .setXi(actualSpec.xi);
      drawWith(*shader);
    } break;

      // TODO:
      // The other FisheyeSensorModelType

    default:
      CORRADE_INTERNAL_ASSERT_UNREACHABLE();
      break;
  }

  return true;
}

}  // namespace sensor
}  // namespace esp
