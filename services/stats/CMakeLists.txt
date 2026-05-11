cmake_minimum_required(VERSION 3.16)
project(dronebros-stats LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.18.3
)
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)
FetchContent_GetProperties(httplib)
if(NOT httplib_POPULATED)
  FetchContent_Populate(httplib)
endif()

add_library(httplib_headers INTERFACE)
target_include_directories(httplib_headers INTERFACE "${httplib_SOURCE_DIR}")

add_executable(dronebros-stats main.cpp collect.cpp)
target_link_libraries(dronebros-stats PRIVATE httplib_headers nlohmann_json::nlohmann_json)
find_package(Threads REQUIRED)
target_link_libraries(dronebros-stats PRIVATE Threads::Threads)
