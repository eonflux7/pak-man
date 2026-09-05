cmake_minimum_required(VERSION 3.24)
project(pak_man VERSION 0.1.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zlib
  URL https://github.com/madler/zlib/archive/refs/tags/v1.3.2.tar.gz
  URL_HASH SHA256=B99A0B86C0BA9360EC7E78C4F1E43B1CBDF1E6936C8FA0F6835C0CD694A495A1)
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
  URL_HASH SHA256=21D8A0A565E85DCE943E375DB00812C2F3F0AB21F3F0F7964E364A63422D7F99)
FetchContent_MakeAvailable(zlib imgui)

add_library(pakman_core STATIC src/core/archive.cpp)
target_include_directories(pakman_core PUBLIC include)
target_link_libraries(pakman_core PUBLIC zlibstatic)
target_compile_definitions(pakman_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(pakman_core PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(pakman_cli src/cli/main.cpp)
target_link_libraries(pakman_cli PRIVATE pakman_core)
set_target_properties(pakman_cli PROPERTIES OUTPUT_NAME pakman-cli)
if(MSVC)
  target_compile_options(pakman_cli PRIVATE /utf-8)
endif()

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui_lib PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(pakman WIN32 src/gui/main.cpp)
target_link_libraries(pakman PRIVATE pakman_core imgui_lib d3d11 dxgi shell32 ole32)
target_compile_definitions(pakman PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(pakman PRIVATE /utf-8)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(pakman_tests tests/test_roundtrip.cpp)
  target_link_libraries(pakman_tests PRIVATE pakman_core)
  if(MSVC)
    target_compile_options(pakman_tests PRIVATE /utf-8)
  endif()
  add_test(NAME pakman_roundtrip COMMAND pakman_tests)
endif()

install(TARGETS pakman pakman_cli RUNTIME DESTINATION .)
install(FILES README.md LICENSE THIRD_PARTY.md DESTINATION .)
install(FILES ${imgui_SOURCE_DIR}/LICENSE.txt DESTINATION licenses RENAME Dear-ImGui-LICENSE.txt)
install(FILES ${zlib_SOURCE_DIR}/LICENSE DESTINATION licenses RENAME zlib-LICENSE.txt)
