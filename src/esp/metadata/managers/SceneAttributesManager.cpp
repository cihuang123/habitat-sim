// Copyright (c) Facebook, Inc. and its affiliates.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "SceneAttributesManager.h"
#include "esp/metadata/MetadataUtils.h"
#include "esp/physics/RigidBase.h"

#include "esp/io/io.h"
#include "esp/io/json.h"

namespace esp {
namespace metadata {

using attributes::SceneAttributes;
using attributes::SceneObjectInstanceAttributes;

namespace managers {

SceneAttributes::ptr SceneAttributesManager::createObject(
    const std::string& sceneInstanceHandle,
    bool registerTemplate) {
  std::string msg;
  SceneAttributes::ptr attrs = this->createFromJsonOrDefaultInternal(
      sceneInstanceHandle, msg, registerTemplate);

  if (nullptr != attrs) {
    LOG(INFO) << msg << " scene instance attributes created"
              << (registerTemplate ? " and registered." : ".");
  }
  return attrs;
}  // SceneAttributesManager::createObject

SceneAttributes::ptr SceneAttributesManager::initNewObjectInternal(
    const std::string& sceneInstanceHandle,
    bool) {
  SceneAttributes::ptr newAttributes =
      this->constructFromDefault(sceneInstanceHandle);
  if (nullptr == newAttributes) {
    newAttributes = SceneAttributes::create(sceneInstanceHandle);
  }
  // attempt to set source directory if exists
  this->setFileDirectoryFromHandle(newAttributes);

  // any internal default configuration here
  return newAttributes;
}  // SceneAttributesManager::initNewObjectInternal

void SceneAttributesManager::setValsFromJSONDoc(
    SceneAttributes::ptr attribs,
    const io::JsonGenericValue& jsonConfig) {
  const std::string attribsDispName = attribs->getSimplifiedHandle();
  // Check for translation origin.  Default to unknown.
  attribs->setTranslationOrigin(getTranslationOriginVal(jsonConfig));

  // Check for stage instance existance
  if ((jsonConfig.HasMember("stage_instance")) &&
      (jsonConfig["stage_instance"].IsObject())) {
    attribs->setStageInstance(
        createInstanceAttributesFromJSON(jsonConfig["stage_instance"]));
  } else {
    LOG(WARNING) << "SceneAttributesManager::setValsFromJSONDoc : No Stage "
                    "specified for scene "
                 << attribsDispName << ", or specification error.";
  }
  // Check for object instances existance
  if ((jsonConfig.HasMember("object_instances")) &&
      (jsonConfig["object_instances"].IsArray())) {
    const auto& objectArray = jsonConfig["object_instances"];
    for (rapidjson::SizeType i = 0; i < objectArray.Size(); i++) {
      const auto& objCell = objectArray[i];
      if (objCell.IsObject()) {
        attribs->addObjectInstance(createInstanceAttributesFromJSON(objCell));
      } else {
        LOG(WARNING) << "SceneAttributesManager::setValsFromJSONDoc : Object "
                        "specification error in scene "
                     << attribsDispName << " at idx : " << i << ".";
      }
    }
  } else {
    LOG(WARNING) << "SceneAttributesManager::setValsFromJSONDoc : No Objects "
                    "specified for scene "
                 << attribsDispName << ", or specification error.";
  }
  std::string dfltLighting = "";
  if (io::readMember<std::string>(jsonConfig, "default_lighting",
                                  dfltLighting)) {
    // if "default lighting" is specified in scene json set value.
    attribs->setLightingHandle(dfltLighting);
  } else {
    LOG(WARNING)
        << "SceneAttributesManager::setValsFromJSONDoc : No default_lighting "
           "specified for scene "
        << attribsDispName << ".";
  }

  std::string navmeshName = "";
  if (io::readMember<std::string>(jsonConfig, "navmesh_instance",
                                  navmeshName)) {
    // if "navmesh_instance" is specified in scene json set value.
    attribs->setNavmeshHandle(navmeshName);
  } else {
    LOG(WARNING)
        << "SceneAttributesManager::setValsFromJSONDoc : No navmesh_instance "
           "specified for scene "
        << attribsDispName << ".";
  }

  std::string semanticDesc = "";
  if (io::readMember<std::string>(jsonConfig, "semantic_scene_instance",
                                  semanticDesc)) {
    // if "semantic scene instance" is specified in scene json set value.
    attribs->setSemanticSceneHandle(semanticDesc);
  } else {
    LOG(WARNING) << "SceneAttributesManager::setValsFromJSONDoc : No "
                    "semantic_scene_instance specified for scene "
                 << attribsDispName << ".";
  }
}  // SceneAttributesManager::setValsFromJSONDoc

SceneObjectInstanceAttributes::ptr
SceneAttributesManager::createInstanceAttributesFromJSON(
    const io::JsonGenericValue& jCell) {
  SceneObjectInstanceAttributes::ptr instanceAttrs =
      createEmptyInstanceAttributes("");
  // template handle describing stage/object instance
  io::jsonIntoConstSetter<std::string>(
      jCell, "template_name",
      [instanceAttrs](const std::string& template_name) {
        instanceAttrs->setHandle(template_name);
      });

  // Check for translation origin override for a particular instance.  Default
  // to unknown, which will mean use scene instance-level default.
  instanceAttrs->setTranslationOrigin(getTranslationOriginVal(jCell));

  // set specified shader type value.  May be Unknown, which means the default
  // value specified in the stage or object attributes will be used.
  instanceAttrs->setShaderType(getShaderTypeFromJsonDoc(jCell));

  // motion type of object.  Ignored for stage.  TODO : verify is valid
  // motion type using standard mechanism of static map comparison.

  int motionTypeVal = static_cast<int>(physics::MotionType::UNDEFINED);
  std::string tmpVal = "";
  if (io::readMember<std::string>(jCell, "motion_type", tmpVal)) {
    // motion type tag was found, perform check - first convert to lowercase
    std::string strToLookFor = Cr::Utility::String::lowercase(tmpVal);
    auto found =
        SceneObjectInstanceAttributes::MotionTypeNamesMap.find(strToLookFor);
    if (found != SceneObjectInstanceAttributes::MotionTypeNamesMap.end()) {
      motionTypeVal = static_cast<int>(found->second);
    } else {
      LOG(WARNING)
          << "SceneAttributesManager::createInstanceAttributesFromJSON : "
             "motion_type value in json  : `"
          << tmpVal << "|" << strToLookFor
          << "` does not map to a valid physics::MotionType value, so "
             "defaulting motion type to MotionType::UNDEFINED.";
    }
  }
  instanceAttrs->setMotionType(motionTypeVal);

  // translation from origin
  io::jsonIntoConstSetter<Magnum::Vector3>(
      jCell, "translation",
      [instanceAttrs](const Magnum::Vector3& translation) {
        instanceAttrs->setTranslation(translation);
      });

  // orientation TODO : support euler angles too?
  io::jsonIntoConstSetter<Magnum::Quaternion>(
      jCell, "rotation", [instanceAttrs](const Magnum::Quaternion& rotation) {
        instanceAttrs->setRotation(rotation);
      });

  return instanceAttrs;

}  // SceneAttributesManager::createInstanceAttributesFromJSON

int SceneAttributesManager::getTranslationOriginVal(
    const io::JsonGenericValue& jsonDoc) {
  // Check for translation origin.  Default to unknown.
  int transOrigin = static_cast<int>(SceneInstanceTranslationOrigin::Unknown);
  std::string tmpTransOriginVal = "";
  if (io::readMember<std::string>(jsonDoc, "translation_origin",
                                  tmpTransOriginVal)) {
    // translation_origin tag was found, perform check - first convert to
    // lowercase
    std::string strToLookFor =
        Cr::Utility::String::lowercase(tmpTransOriginVal);
    auto found =
        SceneAttributes::InstanceTranslationOriginMap.find(strToLookFor);
    if (found != SceneAttributes::InstanceTranslationOriginMap.end()) {
      transOrigin = static_cast<int>(found->second);
    } else {
      LOG(WARNING) << "SceneAttributesManager::getTranslationOriginVal : "
                      "motion_type value in json  : `"
                   << tmpTransOriginVal << "|" << strToLookFor
                   << "` does not map to a valid "
                      "SceneInstanceTranslationOrigin value, so defaulting "
                      "motion type to SceneInstanceTranslationOrigin::Unknown.";
    }
  }
  return transOrigin;
}  // SceneAttributesManager::getTranslationOriginVal

int SceneAttributesManager::registerObjectFinalize(
    SceneAttributes::ptr sceneAttributes,
    const std::string& sceneAttributesHandle,
    bool) {
  // adds template to library, and returns either the ID of the existing
  // template referenced by sceneAttributesHandle, or the next available ID
  // if not found.
  int datasetTemplateID =
      this->addObjectToLibrary(sceneAttributes, sceneAttributesHandle);
  return datasetTemplateID;
}  // SceneAttributesManager::registerObjectFinalize

}  // namespace managers
}  // namespace metadata
}  // namespace esp
