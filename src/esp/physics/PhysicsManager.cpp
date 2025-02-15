// Copyright (c) Facebook, Inc. and its affiliates.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "PhysicsManager.h"
#include <Magnum/Math/Range.h>
#include "esp/assets/CollisionMeshData.h"
#include "esp/physics/objectManagers/RigidObjectManager.h"
#include "esp/sim/Simulator.h"
namespace esp {
namespace physics {

PhysicsManager::PhysicsManager(
    assets::ResourceManager& _resourceManager,
    const metadata::attributes::PhysicsManagerAttributes::cptr&
        _physicsManagerAttributes)
    : resourceManager_(_resourceManager),
      physicsManagerAttributes_(_physicsManagerAttributes),
      rigidObjectManager_(RigidObjectManager::create()) {}

bool PhysicsManager::initPhysics(scene::SceneNode* node) {
  physicsNode_ = node;
  // set the rigidObjectManager's weak reference to physics manager to be based
  // on the same shared pointer that Simulator is using.
  rigidObjectManager_->setPhysicsManager(shared_from_this());
  // Copy over relevant configuration
  fixedTimeStep_ = physicsManagerAttributes_->getTimestep();

  //! Create new scene node and set up any physics-related variables
  // Overridden by specific physics-library-based class
  initialized_ = initPhysicsFinalize();
  return initialized_;
}

bool PhysicsManager::initPhysicsFinalize() {
  //! Create new scene node
  staticStageObject_ = physics::RigidStage::create(&physicsNode_->createChild(),
                                                   resourceManager_);
  return true;
}

PhysicsManager::~PhysicsManager() {
  LOG(INFO) << "Deconstructing PhysicsManager";
}

bool PhysicsManager::addStage(
    const metadata::attributes::StageAttributes::ptr& initAttributes,
    const std::vector<assets::CollisionMeshData>& meshGroup) {
  // Test Mesh primitive is valid
  for (const assets::CollisionMeshData& meshData : meshGroup) {
    if (!isMeshPrimitiveValid(meshData)) {
      return false;
    }
  }

  //! Initialize scene
  bool sceneSuccess = addStageFinalize(initAttributes);
  return sceneSuccess;
}  // PhysicsManager::addStage

bool PhysicsManager::addStageFinalize(
    const metadata::attributes::StageAttributes::ptr& initAttributes) {
  //! Initialize scene
  bool sceneSuccess = staticStageObject_->initialize(initAttributes);
  return sceneSuccess;
}  // PhysicsManager::addStageFinalize

int PhysicsManager::addObjectInstance(
    const esp::metadata::attributes::SceneObjectInstanceAttributes::ptr&
        objInstAttributes,
    const std::string& attributesHandle,
    bool defaultCOMCorrection,
    scene::SceneNode* attachmentNode,
    const std::string& lightSetup) {
  const std::string errMsgTmplt = "PhysicsManager::addObjectInstance : ";
  // Get ObjectAttributes
  auto objAttributes =
      resourceManager_.getObjectAttributesManager()->getObjectCopyByHandle(
          attributesHandle);
  if (!objAttributes) {
    LOG(ERROR) << errMsgTmplt
               << "Missing/improperly configured objectAttributes "
               << attributesHandle << ", whose handle contains "
               << objInstAttributes->getHandle()
               << " as specified in object instance attributes.";
    return false;
  }
  // set shader type to use for stage
  int objShaderType = objInstAttributes->getShaderType();
  if (objShaderType !=
      static_cast<int>(
          metadata::attributes::ObjectInstanceShaderType::Unknown)) {
    objAttributes->setShaderType(objShaderType);
  }
  int objID = 0;
  if (simulator_ != nullptr) {
    auto& drawables = simulator_->getDrawableGroup();
    objID = addObject(objAttributes, &drawables, attachmentNode, lightSetup);
  } else {
    // support creation when simulator DNE
    objID = addObject(objAttributes, nullptr, attachmentNode, lightSetup);
  }

  if (objID == ID_UNDEFINED) {
    // instancing failed for some reason.
    LOG(ERROR) << errMsgTmplt << "Object create failed for objectAttributes "
               << attributesHandle << ", whose handle contains "
               << objInstAttributes->getHandle()
               << " as specified in object instance attributes.";
    return ID_UNDEFINED;
  }

  // set object's location, rotation and other pertinent state values based on
  // scene object instance values
  this->existingObjects_.at(objID)->setStateFromAttributes(
      objInstAttributes.get(), defaultCOMCorrection);

  return objID;
}  // PhysicsManager::addObjectInstance

int PhysicsManager::addObject(const std::string& attributesHandle,
                              scene::SceneNode* attachmentNode,
                              const std::string& lightSetup) {
  esp::metadata::attributes::ObjectAttributes::ptr attributes =
      resourceManager_.getObjectAttributesManager()->getObjectCopyByHandle(
          attributesHandle);
  if (!attributes) {
    LOG(ERROR) << "PhysicsManager::addObject : "
                  "Object creation failed due to unknown attributes "
               << attributesHandle;
    return ID_UNDEFINED;
  } else {
    // attributes exist, get drawables if valid simulator accessible
    if (simulator_ != nullptr) {
      auto& drawables = simulator_->getDrawableGroup();
      return addObject(attributes, &drawables, attachmentNode, lightSetup);
    } else {
      // support creation when simulator DNE
      return addObject(attributes, nullptr, attachmentNode, lightSetup);
    }
  }
}  // PhysicsManager::addObject

int PhysicsManager::addObject(const int attributesID,
                              scene::SceneNode* attachmentNode,
                              const std::string& lightSetup) {
  const esp::metadata::attributes::ObjectAttributes::ptr attributes =
      resourceManager_.getObjectAttributesManager()->getObjectCopyByID(
          attributesID);
  if (!attributes) {
    LOG(ERROR) << "PhysicsManager::addObject : "
                  "Object creation failed due to unknown attributes ID "
               << attributesID;
    return ID_UNDEFINED;
  } else {
    // attributes exist, get drawables if valid simulator accessible
    if (simulator_ != nullptr) {
      auto& drawables = simulator_->getDrawableGroup();
      return addObject(attributes, &drawables, attachmentNode, lightSetup);
    } else {
      // support creation when simulator DNE
      return addObject(attributes, nullptr, attachmentNode, lightSetup);
    }
  }
}  // PhysicsManager::addObject

int PhysicsManager::addObject(
    const esp::metadata::attributes::ObjectAttributes::ptr& objectAttributes,
    DrawableGroup* drawables,
    scene::SceneNode* attachmentNode,
    const std::string& lightSetup) {
  //! Make rigid object and add it to existingObjects
  if (!objectAttributes) {
    // should never run, but just in case
    LOG(ERROR) << "PhysicsManager::addObject : "
                  "Object creation failed due to nonexistant "
                  "objectAttributes";
    return ID_UNDEFINED;
  }
  // verify whether necessary assets exist, and if not, instantiate them
  // only make object if asset instantiation succeeds (short circuit)
  bool objectSuccess =
      resourceManager_.instantiateAssetsOnDemand(objectAttributes);
  if (!objectSuccess) {
    LOG(ERROR) << "PhysicsManager::addObject : "
                  "ResourceManager::instantiateAssetsOnDemand unsuccessful. "
                  "Aborting.";
    return ID_UNDEFINED;
  }

  // derive valid object ID and create new node if necessary
  int nextObjectID_ = allocateObjectID();
  scene::SceneNode* objectNode = attachmentNode;
  if (attachmentNode == nullptr) {
    objectNode = &staticStageObject_->node().createChild();
  }

  objectSuccess =
      makeAndAddRigidObject(nextObjectID_, objectAttributes, objectNode);

  if (!objectSuccess) {
    deallocateObjectID(nextObjectID_);
    if (attachmentNode == nullptr) {
      delete objectNode;
    }
    LOG(ERROR) << "PhysicsManager::addObject : PhysicsManager::makeRigidObject "
                  "unsuccessful.  Aborting.";
    return ID_UNDEFINED;
  }

  // temp non-owning pointer to object
  esp::physics::RigidObject* const obj =
      (existingObjects_.at(nextObjectID_).get());

  obj->visualNodes_.push_back(obj->visualNode_);

  //! Draw object via resource manager
  //! Render node as child of physics node
  //! Verify we should make the object drawable
  if (obj->getInitializationAttributes()->getIsVisible()) {
    resourceManager_.addObjectToDrawables(obj->getInitializationAttributes(),
                                          obj->visualNode_, drawables,
                                          obj->visualNodes_, lightSetup);
  }

  // finalize rigid object creation
  objectSuccess = obj->finalizeObject();
  if (!objectSuccess) {
    // if failed for some reason, remove and return
    removeObject(nextObjectID_, true, true);
    LOG(ERROR) << "PhysicsManager::addObject : PhysicsManager::finalizeObject "
                  "unsuccessful.  Aborting.";
    return ID_UNDEFINED;
  }
  // Valid object exists by here.
  // Now we need to create wrapper, wrap around object,
  // and register wrapper with wrapper manager
  // 1.0 Get unique name for object using simplified attributes name.
  std::string simpleObjectHandle = objectAttributes->getSimplifiedHandle();
  LOG(WARNING) << "PhysicsManager::addObject : simpleObjectHandle : "
               << simpleObjectHandle;
  std::string newObjectHandle =
      rigidObjectManager_->getUniqueHandleFromCandidate(simpleObjectHandle);
  LOG(WARNING) << "PhysicsManager::addObject : newObjectHandle : "
               << newObjectHandle;

  existingObjects_.at(nextObjectID_)->setObjectName(newObjectHandle);
  // 2.0 Get wrapper - name is irrelevant, do not register.
  ManagedRigidObject::ptr objWrapper =
      rigidObjectManager_->createObject("No Name Yet");

  // 3.0 Put object in wrapper
  objWrapper->setObjectRef(existingObjects_.at(nextObjectID_));

  // 4.0 register wrapper in manager
  rigidObjectManager_->registerObject(objWrapper, newObjectHandle);

  return nextObjectID_;
}  // PhysicsManager::addObject

void PhysicsManager::removeObject(const int physObjectID,
                                  bool deleteObjectNode,
                                  bool deleteVisualNode) {
  assertIDValidity(physObjectID);
  scene::SceneNode* objectNode = &existingObjects_.at(physObjectID)->node();
  scene::SceneNode* visualNode = existingObjects_.at(physObjectID)->visualNode_;
  std::string objName = existingObjects_.at(physObjectID)->getObjectName();
  existingObjects_.erase(physObjectID);
  deallocateObjectID(physObjectID);
  if (deleteObjectNode) {
    delete objectNode;
  } else if (deleteVisualNode && visualNode) {
    delete visualNode;
  }
  // remove wrapper if one is present
  if (rigidObjectManager_->getObjectLibHasHandle(objName)) {
    rigidObjectManager_->removeObjectByID(physObjectID);
  }
}  // PhysicsManager::removeObject

void PhysicsManager::setObjectMotionType(const int physObjectID,
                                         MotionType mt) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->setMotionType(mt);
}

MotionType PhysicsManager::getObjectMotionType(const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->getMotionType();
}

int PhysicsManager::allocateObjectID() {
  if (!recycledObjectIDs_.empty()) {
    int recycledID = recycledObjectIDs_.back();
    recycledObjectIDs_.pop_back();
    return recycledID;
  }

  return nextObjectID_++;
}

int PhysicsManager::deallocateObjectID(int physObjectID) {
  recycledObjectIDs_.push_back(physObjectID);
  return physObjectID;
}

bool PhysicsManager::makeAndAddRigidObject(
    int newObjectID,
    const esp::metadata::attributes::ObjectAttributes::ptr& objectAttributes,
    scene::SceneNode* objectNode) {
  auto ptr =
      physics::RigidObject::create(objectNode, newObjectID, resourceManager_);
  bool objSuccess = ptr->initialize(objectAttributes);
  if (objSuccess) {
    existingObjects_.emplace(newObjectID, std::move(ptr));
  }
  return objSuccess;
}

//! Base physics manager has no requirement for mesh primitive
bool PhysicsManager::isMeshPrimitiveValid(const assets::CollisionMeshData&) {
  return true;
}

// TODO: this function should do any engine specific setting which is
// necessary to change the timestep
void PhysicsManager::setTimestep(double dt) {
  fixedTimeStep_ = dt;
}

void PhysicsManager::setGravity(const Magnum::Vector3&) {
  // Can't do this for kinematic simulator
}

Magnum::Vector3 PhysicsManager::getGravity() const {
  return Magnum::Vector3(0);
}

void PhysicsManager::stepPhysics(double dt) {
  // We don't step uninitialized physics sim...
  if (!initialized_) {
    return;
  }

  // ==== Physics stepforward ======
  // NOTE: simulator step goes here in derived classes...

  if (dt < 0) {
    dt = fixedTimeStep_;
  }

  // handle in-between step times? Ideally dt is a multiple of
  // sceneMetaData_.timestep
  double targetTime = worldTime_ + dt;
  while (worldTime_ < targetTime) {
    // per fixed-step operations can be added here

    // kinematic velocity control intergration
    for (auto& object : existingObjects_) {
      VelocityControl::ptr velControl = object.second->getVelocityControl();
      if (velControl->controllingAngVel || velControl->controllingLinVel) {
        object.second->setRigidState(velControl->integrateTransform(
            fixedTimeStep_, object.second->getRigidState()));
      }
    }
    worldTime_ += fixedTimeStep_;
  }
}

//! Profile function. In BulletPhysics stationary objects are
//! marked as inactive to speed up simulation. This function
//! helps checking how many objects are active/inactive at any
//! time step
int PhysicsManager::checkActiveObjects() {
  if (staticStageObject_ == nullptr) {
    return 0;
  }

  // We don't check uninitialized physics sim...
  if (!initialized_) {
    return 0;
  }

  int numActive = 0;
  for (auto& itr : existingObjects_) {
    if (itr.second->isActive()) {
      numActive += 1;
    }
  }
  return numActive;
}

bool PhysicsManager::isActive(const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->isActive();
}

void PhysicsManager::applyForce(const int physObjectID,
                                const Magnum::Vector3& force,
                                const Magnum::Vector3& relPos) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->applyForce(force, relPos);
}

void PhysicsManager::applyImpulse(const int physObjectID,
                                  const Magnum::Vector3& impulse,
                                  const Magnum::Vector3& relPos) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->applyImpulse(impulse, relPos);
}

void PhysicsManager::applyTorque(const int physObjectID,
                                 const Magnum::Vector3& torque) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->applyTorque(torque);
}

void PhysicsManager::applyImpulseTorque(const int physObjectID,
                                        const Magnum::Vector3& impulse) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->applyImpulseTorque(impulse);
}

void PhysicsManager::setTransformation(const int physObjectID,
                                       const Magnum::Matrix4& trans) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->setTransformation(trans);
}
void PhysicsManager::setRigidState(const int physObjectID,
                                   const esp::core::RigidState& rigidState) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->setRigidState(rigidState);
}
void PhysicsManager::setTranslation(const int physObjectID,
                                    const Magnum::Vector3& vector) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->setTranslation(vector);
}
void PhysicsManager::setRotation(const int physObjectID,
                                 const Magnum::Quaternion& quaternion) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->setRotation(quaternion);
}
void PhysicsManager::resetTransformation(const int physObjectID) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->resetTransformation();
}
void PhysicsManager::translate(const int physObjectID,
                               const Magnum::Vector3& vector) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->translate(vector);
}
void PhysicsManager::translateLocal(const int physObjectID,
                                    const Magnum::Vector3& vector) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->translateLocal(vector);
}
void PhysicsManager::rotate(const int physObjectID,
                            const Magnum::Rad angleInRad,
                            const Magnum::Vector3& normalizedAxis) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->rotate(angleInRad, normalizedAxis);
}

void PhysicsManager::rotateLocal(const int physObjectID,
                                 const Magnum::Rad angleInRad,
                                 const Magnum::Vector3& normalizedAxis) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->rotateLocal(angleInRad, normalizedAxis);
}

void PhysicsManager::rotateX(const int physObjectID,
                             const Magnum::Rad angleInRad) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->rotateX(angleInRad);
}
void PhysicsManager::rotateY(const int physObjectID,
                             const Magnum::Rad angleInRad) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->rotateY(angleInRad);
}
void PhysicsManager::rotateXLocal(const int physObjectID,
                                  const Magnum::Rad angleInRad) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->rotateXLocal(angleInRad);
}
void PhysicsManager::rotateYLocal(const int physObjectID,
                                  const Magnum::Rad angleInRad) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->rotateYLocal(angleInRad);
}
void PhysicsManager::rotateZ(const int physObjectID,
                             const Magnum::Rad angleInRad) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->rotateZ(angleInRad);
}
void PhysicsManager::rotateZLocal(const int physObjectID,
                                  const Magnum::Rad angleInRad) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->rotateZLocal(angleInRad);
}

Magnum::Matrix4 PhysicsManager::getTransformation(
    const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->node().transformation();
}

esp::core::RigidState PhysicsManager::getRigidState(
    const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->getRigidState();
}

Magnum::Vector3 PhysicsManager::getTranslation(const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->node().translation();
}

Magnum::Quaternion PhysicsManager::getRotation(const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->node().rotation();
}

void PhysicsManager::setLinearVelocity(const int physObjectID,
                                       const Magnum::Vector3& linVel) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->setLinearVelocity(linVel);
}

void PhysicsManager::setAngularVelocity(const int physObjectID,
                                        const Magnum::Vector3& angVel) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->setAngularVelocity(angVel);
}

Magnum::Vector3 PhysicsManager::getLinearVelocity(
    const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->getLinearVelocity();
}

Magnum::Vector3 PhysicsManager::getAngularVelocity(
    const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->getAngularVelocity();
}

VelocityControl::ptr PhysicsManager::getVelocityControl(
    const int physObjectID) {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->getVelocityControl();
}

//============ Object Setter functions =============
void PhysicsManager::setMass(const int physObjectID, const double mass) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->setMass(mass);
}
void PhysicsManager::setCOM(const int physObjectID,
                            const Magnum::Vector3& COM) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->setCOM(COM);
}
void PhysicsManager::setInertiaVector(const int physObjectID,
                                      const Magnum::Vector3& inertia) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->setInertiaVector(inertia);
}
void PhysicsManager::setFrictionCoefficient(const int physObjectID,
                                            const double frictionCoefficient) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)
      ->setFrictionCoefficient(frictionCoefficient);
}
void PhysicsManager::setRestitutionCoefficient(
    const int physObjectID,
    const double restitutionCoefficient) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)
      ->setRestitutionCoefficient(restitutionCoefficient);
}
void PhysicsManager::setLinearDamping(const int physObjectID,
                                      const double linDamping) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->setLinearDamping(linDamping);
}
void PhysicsManager::setAngularDamping(const int physObjectID,
                                       const double angDamping) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->setAngularDamping(angDamping);
}

#ifdef ESP_BUILD_WITH_VHACD
void PhysicsManager::generateVoxelization(const int physObjectID,
                                          const int resolution) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)
      ->generateVoxelization(resourceManager_, resolution);
}

void PhysicsManager::generateStageVoxelization(const int resolution) {
  staticStageObject_->generateVoxelization(resourceManager_, resolution);
}
#endif

//============ Object Getter functions =============
double PhysicsManager::getMass(const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->getMass();
}

Magnum::Vector3 PhysicsManager::getCOM(const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->getCOM();
}

Magnum::Vector3 PhysicsManager::getInertiaVector(const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->getInertiaVector();
}

Magnum::Matrix3 PhysicsManager::getInertiaMatrix(const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->getInertiaMatrix();
}

Magnum::Vector3 PhysicsManager::getScale(const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->getScale();
}

double PhysicsManager::getFrictionCoefficient(const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->getFrictionCoefficient();
}

double PhysicsManager::getRestitutionCoefficient(const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->getRestitutionCoefficient();
}

double PhysicsManager::getLinearDamping(const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->getLinearDamping();
}

double PhysicsManager::getAngularDamping(const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->getAngularDamping();
}
std::shared_ptr<esp::geo::VoxelWrapper> PhysicsManager::getObjectVoxelization(
    const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->getVoxelization();
}

std::shared_ptr<esp::geo::VoxelWrapper> PhysicsManager::getStageVoxelization()
    const {
  return staticStageObject_->getVoxelization();
}

void PhysicsManager::setObjectBBDraw(int physObjectID,
                                     DrawableGroup* drawables,
                                     bool drawBB) {
  assertIDValidity(physObjectID);
  if (existingObjects_.at(physObjectID)->BBNode_ && !drawBB) {
    // destroy the node
    delete existingObjects_.at(physObjectID)->BBNode_;
    existingObjects_.at(physObjectID)->BBNode_ = nullptr;
  } else if (drawBB && existingObjects_.at(physObjectID)->visualNode_) {
    // add a new BBNode
    Magnum::Vector3 scale = existingObjects_.at(physObjectID)
                                ->visualNode_->getCumulativeBB()
                                .size() /
                            2.0;
    existingObjects_.at(physObjectID)->BBNode_ =
        &existingObjects_.at(physObjectID)->visualNode_->createChild();
    existingObjects_.at(physObjectID)->BBNode_->MagnumObject::setScaling(scale);
    existingObjects_.at(physObjectID)
        ->BBNode_->MagnumObject::setTranslation(
            existingObjects_[physObjectID]
                ->visualNode_->getCumulativeBB()
                .center());
    resourceManager_.addPrimitiveToDrawables(
        0, *existingObjects_.at(physObjectID)->BBNode_, drawables);
  }
}

void PhysicsManager::setObjectVoxelizationDraw(int physObjectID,
                                               const std::string& gridName,
                                               DrawableGroup* drawables,
                                               bool drawVoxelization) {
  assertIDValidity(physObjectID);
  setVoxelizationDraw(gridName,
                      static_cast<esp::physics::RigidBase*>(
                          existingObjects_.at(physObjectID).get()),
                      drawables, drawVoxelization);
}

void PhysicsManager::setStageVoxelizationDraw(const std::string& gridName,
                                              DrawableGroup* drawables,
                                              bool drawVoxelization) {
  setVoxelizationDraw(
      gridName, static_cast<esp::physics::RigidBase*>(staticStageObject_.get()),
      drawables, drawVoxelization);
}

void PhysicsManager::setVoxelizationDraw(const std::string& gridName,
                                         esp::physics::RigidBase* rigidBase,
                                         DrawableGroup* drawables,
                                         bool drawVoxelization) {
  if (rigidBase->VoxelNode_ && !drawVoxelization) {
    // destroy the node
    delete rigidBase->VoxelNode_;
    rigidBase->VoxelNode_ = nullptr;

  } else if (drawVoxelization && rigidBase->visualNode_) {
    if (rigidBase->VoxelNode_) {
      // if the VoxelNode is already rendering something, destroy it.
      delete rigidBase->VoxelNode_;
    }

    // re-create the voxel node
    rigidBase->VoxelNode_ = &rigidBase->visualNode_->createChild();

    esp::geo::VoxelWrapper* voxelWrapper_ = rigidBase->voxelWrapper.get();
    gfx::Drawable::Flags meshAttributeFlags{};
    resourceManager_.createDrawable(
        voxelWrapper_->getVoxelGrid()->getMeshGL(gridName), meshAttributeFlags,
        *rigidBase->VoxelNode_, DEFAULT_LIGHTING_KEY,
        PER_VERTEX_OBJECT_ID_MATERIAL_KEY, drawables);

    // If the RigidBase is a stage, need to set the BB to make culling work.
    if (dynamic_cast<esp::physics::RigidStage*>(rigidBase) != nullptr) {
      // set bounding box for the node to be the bb computed by vhacd
      Mn::Range3D bb{rigidBase->voxelWrapper->getVoxelGrid()->getOffset(),
                     rigidBase->voxelWrapper->getVoxelGrid()->getMaxOffset()};
      rigidBase->VoxelNode_->setMeshBB(bb);
      //
      rigidBase->node().computeCumulativeBB();
    }
  }
}

const scene::SceneNode& PhysicsManager::getObjectSceneNode(
    int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->getSceneNode();
}

scene::SceneNode& PhysicsManager::getObjectSceneNode(int physObjectID) {
  assertIDValidity(physObjectID);
  return const_cast<scene::SceneNode&>(
      existingObjects_.at(physObjectID)->getSceneNode());
}

const scene::SceneNode& PhysicsManager::getObjectVisualSceneNode(
    int physObjectID) const {
  assertIDValidity(physObjectID);
  return *existingObjects_.at(physObjectID)->visualNode_;
}

std::vector<scene::SceneNode*> PhysicsManager::getObjectVisualSceneNodes(
    const int physObjectID) const {
  assertIDValidity(physObjectID);
  return existingObjects_.at(physObjectID)->visualNodes_;
}

void PhysicsManager::setSemanticId(const int physObjectID,
                                   uint32_t semanticId) {
  assertIDValidity(physObjectID);
  existingObjects_.at(physObjectID)->setSemanticId(semanticId);
}

}  // namespace physics
}  // namespace esp
