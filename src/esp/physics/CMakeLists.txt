find_package(MagnumPlugins REQUIRED StbImageImporter StbImageConverter TinyGltfImporter)

set(ESP_DEFAULT_PHYSICS_CONFIG_REL_PATH ./data/default.physics_config.json)
set(ESP_DEFAULT_PHYSICS_CONFIG
    ${PROJECT_SOURCE_DIR}/.${ESP_DEFAULT_PHYSICS_CONFIG_REL_PATH}
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/configure.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/configure.h
)

add_library(
  physics STATIC
  objectManagers/PhysicsObjectBaseManager.h
  objectManagers/RigidBaseManager.h
  objectManagers/RigidObjectManager.cpp
  objectManagers/RigidObjectManager.h
  objectWrappers/ManagedPhysicsObjectBase.h
  objectWrappers/ManagedRigidBase.h
  objectWrappers/ManagedRigidObject.h
  PhysicsManager.cpp
  PhysicsManager.h
  PhysicsObjectBase.h
  RigidBase.h
  RigidObject.cpp
  RigidObject.h
  RigidStage.cpp
  RigidStage.h
)

if(BUILD_WITH_BULLET)
  add_subdirectory(bullet)

  target_link_libraries(physics PUBLIC bulletphysics)
endif()

target_link_libraries(
  physics
  PUBLIC core
         scene
         assets
         MagnumPlugins::StbImageImporter
         MagnumPlugins::StbImageConverter
         MagnumPlugins::TinyGltfImporter
)

set_directory_properties(PROPERTIES CORRADE_USE_PEDANTIC_FLAGS ON)
