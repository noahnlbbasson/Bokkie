cmake_minimum_required(VERSION 3.16)
project(Bokkie)

set(CMAKE_CXX_STANDARD 20)

include(FetchContent)
configure_file(config.json config.json COPYONLY)
configure_file(brain/memory.md brain/memory.md COPYONLY)

set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)

if(WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
    curl
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG curl-8_17_0
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
        json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
)
FetchContent_MakeAvailable(curl json)

add_executable(Bokkie main.cpp
        ProcessRunner.cpp
        ProcessRunner.h
        Client.cpp
        Client.h
        Tools.cpp
        Tools.h
        ToolRegistry.cpp
        ToolRegistry.h
        brain/Conversation.cpp
        brain/Conversation.h
        brain/Memory.cpp
        brain/Memory.h
        brain/TaskState.cpp
        brain/TaskState.h
        Runtime.cpp
        Runtime.h
        Agent.cpp
        Agent.h
        main.cpp
        md_render.h
        LandingScreen.cpp
        LandingScreen.h
        LandingScreen.h
        WorkspacesScreen.cpp
        WorkspacesScreen.h
        Terminal.h
        workspaces/Workspace.cpp
        workspaces/Workspace.h)

target_compile_definitions(Bokkie PRIVATE
        "BOKKIE_TASK_STATE_FILE=\"${CMAKE_CURRENT_SOURCE_DIR}/brain/task_state.json\"")

target_link_libraries(Bokkie PRIVATE CURL::libcurl nlohmann_json::nlohmann_json)
