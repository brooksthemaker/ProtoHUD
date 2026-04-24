cmake_minimum_required(VERSION 3.20)
project(ProtoHUD VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_DEBUG   "-g -DDEBUG")

# ── Dependencies ──────────────────────────────────────────────────────────────

find_package(PkgConfig REQUIRED)

# GLFW3 (window + input — X11 backend on Bullseye)
# Try pkg-config first; fall back to CMake config (installed by the source build).
pkg_check_modules(GLFW3 glfw3)   # not REQUIRED here — we handle failure below
if(NOT GLFW3_FOUND)
    find_package(glfw3 3.3 QUIET)
    if(glfw3_FOUND)
        # Translate find_package target to the variables the rest of this file uses
        get_target_property(_glfw_loc glfw LOCATION)
        set(GLFW3_LIBRARIES    glfw)
        set(GLFW3_INCLUDE_DIRS "")
        set(GLFW3_CFLAGS_OTHER "")
        set(GLFW3_FOUND TRUE)
        message(STATUS "GLFW3 found via find_package (${_glfw_loc})")
    else()
        message(FATAL_ERROR
            "GLFW3 not found.\n"
            "Install with:  sudo apt-get install libglfw3-dev\n"
            "Or run:        scripts/install.sh  (builds from source as fallback)")
    endif()
endif()

# OpenCV (USB cameras + MJPEG decode for Beast camera)
find_package(OpenCV REQUIRED COMPONENTS core imgproc videoio imgcodecs)

# libcamera (OWLsight CSI cameras)
pkg_check_modules(LIBCAMERA REQUIRED libcamera)

# EGL + OpenGL ES (zero-copy DMA camera path)
pkg_check_modules(EGL  REQUIRED egl)
pkg_check_modules(GLES REQUIRED glesv2)

# nlohmann/json
find_package(nlohmann_json REQUIRED)

# Threads
find_package(Threads REQUIRED)

# libgpiod (GPIO button input)
pkg_check_modules(LIBGPIOD REQUIRED libgpiod)

# ALSA (audio capture from RP2350 USB Audio + playback to output device)
pkg_check_modules(ALSA REQUIRED alsa)

# ── Dear ImGui (vendored source, no external install required) ───────────────
# Fetched once by CMake; cached in build/_deps after first configure.
include(FetchContent)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# ImGui has no CMakeLists — build it as a static library here.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    # GLFW platform backend
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    # OpenGL ES 2 render backend
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${GLFW3_INCLUDE_DIRS}
    ${GLES_INCLUDE_DIRS}
)
# Select GLES2 inside the imgui_impl_opengl3 backend
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES2)
target_link_libraries(imgui PUBLIC ${GLFW3_LIBRARIES} ${GLES_LIBRARIES})

# ── VITURE XR SDK (vendored, aarch64) ────────────────────────────────────────

set(VITURE_SDK_DIR  "${CMAKE_SOURCE_DIR}/vendor/viture")
set(VITURE_INCLUDE  "${VITURE_SDK_DIR}/include")
set(VITURE_LIB_DIR  "${VITURE_SDK_DIR}/lib/aarch64")

add_library(glasses SHARED IMPORTED)
set_target_properties(glasses PROPERTIES
    IMPORTED_LOCATION             "${VITURE_LIB_DIR}/libglasses.so"
    INTERFACE_INCLUDE_DIRECTORIES "${VITURE_INCLUDE}"
)

# ── Sources ───────────────────────────────────────────────────────────────────

set(SOURCES
    src/main.cpp
    src/android/android_mirror.cpp
    src/camera/camera_manager.cpp
    src/camera/dma_camera.cpp
    src/camera/viture_camera.cpp
    src/input/gpio_buttons.cpp
    src/serial/serial_port.cpp
    src/serial/teensy_controller.cpp
    src/serial/lora_radio.cpp
    src/serial/smartknob.cpp
    src/hud/hud_renderer.cpp
    src/menu/menu_system.cpp
    src/vitrue/xr_display.cpp
    src/vitrue/timewarp.cpp
    src/audio/audio_engine.cpp
)

add_executable(protohud ${SOURCES})

target_include_directories(protohud PRIVATE
    src/
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${VITURE_INCLUDE}
    ${OpenCV_INCLUDE_DIRS}
    ${LIBCAMERA_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLES_INCLUDE_DIRS}
    ${GLFW3_INCLUDE_DIRS}
    ${LIBGPIOD_INCLUDE_DIRS}
    ${ALSA_INCLUDE_DIRS}
)

target_link_libraries(protohud PRIVATE
    imgui
    glasses
    ${OpenCV_LIBS}
    ${LIBCAMERA_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES_LIBRARIES}
    ${GLFW3_LIBRARIES}
    ${LIBGPIOD_LIBRARIES}
    ${ALSA_LIBRARIES}
    nlohmann_json::nlohmann_json
    Threads::Threads
)

target_compile_options(protohud PRIVATE
    ${LIBCAMERA_CFLAGS_OTHER}
    ${EGL_CFLAGS_OTHER}
    ${GLES_CFLAGS_OTHER}
    ${GLFW3_CFLAGS_OTHER}
)

# Ensure libglasses.so is found at runtime relative to the binary
set_target_properties(protohud PROPERTIES
    BUILD_RPATH   "${VITURE_LIB_DIR}"
    INSTALL_RPATH "\$ORIGIN/../lib"
)

# ── Assets & Config ────────────────────────────────────────���──────────────────

configure_file(config/config.json ${CMAKE_BINARY_DIR}/config.json COPYONLY)

# Copy NV12 shaders to build directory
file(GLOB SHADER_FILES "${CMAKE_SOURCE_DIR}/assets/shaders/*")
foreach(SHADER ${SHADER_FILES})
    configure_file(${SHADER} "${CMAKE_BINARY_DIR}/assets/shaders/" COPYONLY)
endforeach()

# Copy VITURE .so files next to the binary for in-tree runs
add_custom_command(TARGET protohud POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VITURE_LIB_DIR}/libglasses.so"
        "${VITURE_LIB_DIR}/libcarina_vio.so"
        "$<TARGET_FILE_DIR:protohud>"
    COMMENT "Copying VITURE SDK libraries to build directory"
)

install(TARGETS protohud RUNTIME DESTINATION bin)
install(FILES config/config.json DESTINATION etc/protohud)
install(FILES "${VITURE_LIB_DIR}/libglasses.so"
              "${VITURE_LIB_DIR}/libcarina_vio.so"
        DESTINATION lib)
