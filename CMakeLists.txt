cmake_minimum_required(VERSION 3.15)
project(com.polyphase.formats.video)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Polyphase Engine path
set(POLYPHASE_PATH "/home/uai/Projects/Octave/Polyphase-Engine/")

# Gather source files
file(GLOB_RECURSE SOURCES "Source/*.cpp" "Source/*.c")
file(GLOB_RECURSE HEADERS "Source/*.h" "Source/*.hpp")

# Create shared library
add_library(com.polyphase.formats.video SHARED ${SOURCES} ${HEADERS})

# Find Vulkan SDK
if(DEFINED ENV{VULKAN_SDK})
    set(VULKAN_SDK_PATH $ENV{VULKAN_SDK})
else()
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        get_filename_component(VULKAN_SDK_PATH ${Vulkan_INCLUDE_DIRS} DIRECTORY)
    endif()
endif()

# Include directories
target_include_directories(com.polyphase.formats.video PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
    ${POLYPHASE_PATH}/Engine/Source
    ${POLYPHASE_PATH}/Engine/Source/Engine
    ${POLYPHASE_PATH}/Engine/Source/Plugins
    ${POLYPHASE_PATH}/External/Lua
    ${POLYPHASE_PATH}/External/glm
    ${POLYPHASE_PATH}/External/Imgui
    ${POLYPHASE_PATH}/External/ImGuizmo
    ${POLYPHASE_PATH}/External/bullet3/src
    ${POLYPHASE_PATH}/External/Assimp
    ${POLYPHASE_PATH}/External
)

# Add Vulkan SDK include path if found
if(VULKAN_SDK_PATH)
    target_include_directories(com.polyphase.formats.video PRIVATE ${VULKAN_SDK_PATH}/Include)
endif()

# Compile definitions
target_compile_definitions(com.polyphase.formats.video PRIVATE
    OCTAVE_PLUGIN_EXPORT
    EDITOR=1
    LUA_ENABLED=1
    GLM_FORCE_RADIANS
    PLATFORM_LINUX=1
    API_VULKAN=1
    COMPOLYPHASEFORMATSVIDEO_EXPORTS
)

# Link against Polyphase import library and dependencies
if(WIN32)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(POLYPHASE_LIB_PATH "${POLYPHASE_PATH}/Standalone/Build/Windows/x64/DebugEditor")
    else()
        set(POLYPHASE_LIB_PATH "${POLYPHASE_PATH}/Standalone/Build/Windows/x64/ReleaseEditor")
    endif()
    target_link_directories(com.polyphase.formats.video PRIVATE ${POLYPHASE_LIB_PATH})
    target_link_libraries(com.polyphase.formats.video PRIVATE Polyphase)
endif()
