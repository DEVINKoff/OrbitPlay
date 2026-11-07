cmake_minimum_required(VERSION 3.20)
project(OrbitPlay VERSION 1.7.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
	message(FATAL_ERROR "Orbit Play targets Windows (Direct3D 11). Build it with MSYS2 or the GitHub Actions workflow.")
endif()

# ---------------------------------------------------------------------------
# chiaki-ng: only the Remote Play protocol library is used.
set(CHIAKI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/chiaki-ng" CACHE PATH "chiaki-ng source tree")
set(CHIAKI_ENABLE_GUI OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_CLI OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_FFMPEG_DECODER ON CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_PI_DECODER OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_SETSU OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_STEAMDECK_NATIVE OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_SPEEX OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_STEAM_SHORTCUT OFF CACHE BOOL "" FORCE)

include(FetchContent)
if(NOT EXISTS "${CHIAKI_DIR}/lib/CMakeLists.txt")
	# Not vendored: fetch the pinned chiaki-ng revision (with its submodules).
	message(STATUS "Fetching chiaki-ng")
	FetchContent_Declare(chiaki_ng
		GIT_REPOSITORY https://github.com/streetpea/chiaki-ng.git
		GIT_TAG 8179a2d1964c75dc32614fd27e51c2381093f539
		GIT_SUBMODULES third-party/curl third-party/gf-complete third-party/jerasure third-party/nanopb
		GIT_SUBMODULES_RECURSE ON
		SOURCE_SUBDIR populate-only) # no CMakeLists there: download only, added below
	FetchContent_MakeAvailable(chiaki_ng)
	set(CHIAKI_DIR "${chiaki_ng_SOURCE_DIR}")
endif()
add_subdirectory("${CHIAKI_DIR}" chiaki EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Dependencies
find_package(SDL3 REQUIRED CONFIG)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil)

include(FetchContent)
FetchContent_Declare(imgui
	GIT_REPOSITORY https://github.com/ocornut/imgui.git
	GIT_TAG v1.91.5
	GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
	${imgui_SOURCE_DIR}/imgui.cpp
	${imgui_SOURCE_DIR}/imgui_draw.cpp
	${imgui_SOURCE_DIR}/imgui_tables.cpp
	${imgui_SOURCE_DIR}/imgui_widgets.cpp
	${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
	${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3 d3d11 d3dcompiler)

# ---------------------------------------------------------------------------
add_executable(OrbitPlay WIN32
	src/main.cpp
	src/app.cpp
	src/audio.cpp
	src/discovery.cpp
	src/gamepad.cpp
	src/log.cpp
	src/registration.cpp
	src/renderer.cpp
	src/settings.cpp
	src/stream_session.cpp
	src/vpscaler.cpp
	src/kbm.cpp
	src/recorder.cpp
	src/ui_main.cpp
	src/window_chrome.cpp
	src/uisound.cpp
	res/orbitplay.rc)

target_compile_definitions(OrbitPlay PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 ORBITPLAY_VERSION="${PROJECT_VERSION}")
target_link_libraries(OrbitPlay PRIVATE
	chiaki-lib
	imgui
	SDL3::SDL3
	PkgConfig::FFMPEG
	d3d11 dxgi d3dcompiler winmm winhttp shell32 iphlpapi ws2_32 windowscodecs ole32 uuid dbghelp comctl32 dwmapi)

if(MINGW)
	target_link_options(OrbitPlay PRIVATE -static-libgcc -static-libstdc++)
endif()
