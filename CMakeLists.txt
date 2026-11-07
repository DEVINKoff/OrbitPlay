cmake_minimum_required(VERSION 3.20)
project(OrbitPlay VERSION 1.7.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
	set(ORBIT_IOS ON)
	enable_language(OBJC OBJCXX)
elseif(NOT WIN32)
	message(FATAL_ERROR "Orbit Play builds for Windows (Direct3D 11) and iPadOS (Metal). "
		"For iPad, configure with -DCMAKE_SYSTEM_NAME=iOS (see ios/build.sh).")
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

FetchContent_Declare(imgui
	GIT_REPOSITORY https://github.com/ocornut/imgui.git
	GIT_TAG v1.91.5
	GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

set(IMGUI_SOURCES
	${imgui_SOURCE_DIR}/imgui.cpp
	${imgui_SOURCE_DIR}/imgui_draw.cpp
	${imgui_SOURCE_DIR}/imgui_tables.cpp
	${imgui_SOURCE_DIR}/imgui_widgets.cpp
	${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp)
if(ORBIT_IOS)
	add_library(imgui STATIC ${IMGUI_SOURCES} ${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm)
	set_source_files_properties(${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
	target_link_libraries(imgui PUBLIC SDL3::SDL3 "-framework Metal" "-framework QuartzCore")
else()
	add_library(imgui STATIC ${IMGUI_SOURCES} ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
	target_link_libraries(imgui PUBLIC SDL3::SDL3 d3d11 d3dcompiler)
endif()
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

# ---------------------------------------------------------------------------
# Shared by both platforms
set(SHARED_SOURCES
	src/app.cpp
	src/audio.cpp
	src/discovery.cpp
	src/gamepad.cpp
	src/log.cpp
	src/registration.cpp
	src/settings.cpp
	src/stream_session.cpp
	src/kbm.cpp
	src/recorder.cpp
	src/ui_main.cpp
	src/uisound.cpp)

if(ORBIT_IOS)
	# ---- iPadOS app bundle
	set(FONTS
		${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/IBMPlexSans-Light.ttf
		${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/IBMPlexSans-Regular.ttf
		${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/IBMPlexSans-Medium.ttf
		${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/IBMPlexSans-SemiBold.ttf)
	set_source_files_properties(${FONTS} PROPERTIES MACOSX_PACKAGE_LOCATION "fonts")
	add_executable(OrbitPlay MACOSX_BUNDLE
		${SHARED_SOURCES}
		src/apple/main_apple.cpp
		src/apple/app_apple.cpp
		src/apple/renderer_metal.mm
		src/apple/https_apple.mm
		src/platform/platform_apple.mm
		${FONTS})
	set_source_files_properties(src/apple/renderer_metal.mm src/apple/https_apple.mm src/platform/platform_apple.mm
		PROPERTIES COMPILE_FLAGS "-fobjc-arc")
	set_target_properties(OrbitPlay PROPERTIES
		OUTPUT_NAME "OrbitPlay"
		MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in"
		MACOSX_BUNDLE_GUI_IDENTIFIER "app.orbitplay.ipad"
		MACOSX_BUNDLE_BUNDLE_NAME "Orbit Play"
		MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
		MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
		XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO)
	target_compile_definitions(OrbitPlay PRIVATE ORBITPLAY_VERSION="${PROJECT_VERSION}")
	target_link_libraries(OrbitPlay PRIVATE
		chiaki-lib
		imgui
		SDL3::SDL3
		PkgConfig::FFMPEG
		"-framework Foundation" "-framework UIKit" "-framework Metal" "-framework QuartzCore" "-framework CoreVideo"
		"-framework CoreMedia" "-framework VideoToolbox" "-framework AudioToolbox" "-framework AVFoundation"
		"-framework CoreGraphics" "-framework ImageIO" "-framework Security" "-framework GameController"
		"-framework CoreHaptics" "-framework CoreMotion" "-framework CoreBluetooth" "-framework CoreFoundation"
		"-weak_framework MetalFX" "-weak_framework UniformTypeIdentifiers"
		z bz2 iconv)
else()
	# ---- Windows
	add_executable(OrbitPlay WIN32
		${SHARED_SOURCES}
		src/main.cpp
		src/renderer.cpp
		src/vpscaler.cpp
		src/window_chrome.cpp
		src/platform/platform_win.cpp
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
endif()
