cmake_minimum_required(VERSION 3.20)
project(ProtoHUD VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_DEBUG   "-g -DDEBUG")

# ── Git hash compile definition ───────────────────────────────────────────────

execute_process(
    COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --short HEAD
    OUTPUT_VARIABLE GIT_HASH_STR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_HASH_STR)
    set(GIT_HASH_STR "unknown")
endif()

# ── Dependencies ──────────────────────────────────────────────────────────────

find_package(PkgConfig REQUIRED)

# GLFW3 (window + input)
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

# ZBar (QR / barcode scanning) — optional; install with: sudo apt install libzbar-dev
pkg_check_modules(ZBAR zbar)
if(ZBAR_FOUND)
    message(STATUS "ZBar found — QR scanning enabled")
else()
    message(STATUS "ZBar not found — QR scanning disabled (sudo apt install libzbar-dev)")
endif()

# SDL2 (gamepad/controller input) — optional; install with: sudo apt install libsdl2-dev
pkg_check_modules(SDL2 sdl2)
if(SDL2_FOUND)
    message(STATUS "SDL2 found — gamepad input enabled")
else()
    message(STATUS "SDL2 not found — gamepad input disabled (sudo apt install libsdl2-dev)")
endif()

# DBus (KDE Connect bridge) — optional; install with: sudo apt install libdbus-1-dev
pkg_check_modules(DBUS dbus-1)
if(DBUS_FOUND)
    message(STATUS "DBus found — KDE Connect bridge enabled")
else()
    message(STATUS "DBus not found — KDE Connect bridge disabled (sudo apt install libdbus-1-dev)")
endif()

# ── Dear ImGui (vendored source, no external install required) ───────────────
# Fetched once by CMake; cached in build/_deps after first configure.
include(FetchContent)

# ── NanoVG (vector graphics renderer for the HUD layer) ──────────────────────
# nanovg has no CMakeLists.txt. Pin SOURCE_DIR explicitly so the path used in
# add_library is always correct regardless of FetchContent variable scope.
set(_nanovg_src "${CMAKE_BINARY_DIR}/_deps/nanovg-src")
FetchContent_Declare(nanovg
    GIT_REPOSITORY https://github.com/memononen/nanovg.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
    SOURCE_DIR     "${_nanovg_src}"
)
FetchContent_MakeAvailable(nanovg)

add_library(nanovg STATIC "${_nanovg_src}/src/nanovg.c")
target_include_directories(nanovg PUBLIC "${_nanovg_src}/src" ${GLES_INCLUDE_DIRS})
target_compile_definitions(nanovg PUBLIC NANOVG_GLES2)
# nanovg.c embeds stb_image.h with STB_IMAGE_IMPLEMENTATION; STB_IMAGE_STATIC
# makes those symbols file-local so they don't collide with splash.cpp's copy.
target_compile_definitions(nanovg PRIVATE STB_IMAGE_STATIC)
target_link_libraries(nanovg PUBLIC ${GLES_LIBRARIES})

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(stb)

# QR-Code-generator (nayuki, MIT) — encode the scheduler web URL for the menu.
# The repo has no top-level CMakeLists, so MakeAvailable just fetches the source
# (like nanovg); we compile the one C++ file ourselves.
FetchContent_Declare(qrcodegen
    GIT_REPOSITORY https://github.com/nayuki/QR-Code-generator.git
    GIT_TAG        v1.8.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(qrcodegen)
add_library(qrcodegen_cpp STATIC "${qrcodegen_SOURCE_DIR}/cpp/qrcodegen.cpp")
target_include_directories(qrcodegen_cpp PUBLIC "${qrcodegen_SOURCE_DIR}/cpp")


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
    src/input/gpio_inputs.cpp
    src/input/coproc_inputs.cpp
    src/input/gamepad_input.cpp
    src/input/wireless_controller.cpp
    src/serial/serial_port.cpp
    src/serial/teensy_controller.cpp
    src/serial/protoface_controller.cpp
    src/serial/shm_frame_reader.cpp
    src/serial/lora_radio.cpp
    src/serial/smartknob.cpp
    src/hud/hud_renderer.cpp
    src/hud/background_library.cpp
    src/menu/menu_system.cpp
    src/menu/file_picker.cpp
    src/menu/face_editor.cpp
    src/profile_manager.cpp
    src/vitrue/xr_display.cpp
    src/vitrue/timewarp.cpp
    src/audio/audio_engine.cpp
    src/audio/voice_analyzer.cpp
    src/post_process.cpp
    src/sensor/mpu9250.cpp
    src/sensor/bno055.cpp
    src/sensor/mpr121_boop_sensor.cpp
    src/sensor/light_sensor.cpp
    src/accessory/led_strip.cpp
    src/accessory/accessory_leds.cpp
    src/sys/system_monitor.cpp
    src/sys/fan_controller.cpp
    src/sys/scheduler_monitor.cpp
    src/net/wifi_monitor.cpp
    src/net/ping_monitor.cpp
    src/net/bt_monitor.cpp
    src/net/weather_monitor.cpp
    src/crash_reporter.cpp
    src/capture.cpp
    src/video_recorder.cpp
    src/qr_scanner.cpp
    src/splash.cpp
    src/integrations/phone_inbox.cpp
    $<$<BOOL:${DBUS_FOUND}>:src/integrations/kdeconnect_bridge.cpp>
    src/nanovg_gl_impl.cpp
    src/hud/toast_renderer.cpp
)

add_executable(protohud ${SOURCES})

target_include_directories(protohud PRIVATE
    src/
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${stb_SOURCE_DIR}
    ${nanovg_SOURCE_DIR}/src
    ${VITURE_INCLUDE}
    ${OpenCV_INCLUDE_DIRS}
    ${LIBCAMERA_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLES_INCLUDE_DIRS}
    ${GLFW3_INCLUDE_DIRS}
    ${LIBGPIOD_INCLUDE_DIRS}
    ${ALSA_INCLUDE_DIRS}
    $<$<BOOL:${ZBAR_FOUND}>:${ZBAR_INCLUDE_DIRS}>
    $<$<BOOL:${SDL2_FOUND}>:${SDL2_INCLUDE_DIRS}>
    $<$<BOOL:${DBUS_FOUND}>:${DBUS_INCLUDE_DIRS}>
)

target_link_libraries(protohud PRIVATE
    imgui
    nanovg
    qrcodegen_cpp
    glasses
    protoface_native
    ${OpenCV_LIBS}
    ${LIBCAMERA_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES_LIBRARIES}
    ${GLFW3_LIBRARIES}
    ${LIBGPIOD_LIBRARIES}
    ${ALSA_LIBRARIES}
    nlohmann_json::nlohmann_json
    Threads::Threads
    $<$<BOOL:${ZBAR_FOUND}>:${ZBAR_LIBRARIES}>
    $<$<BOOL:${SDL2_FOUND}>:${SDL2_LIBRARIES}>
    $<$<BOOL:${DBUS_FOUND}>:${DBUS_LIBRARIES}>
)

if(ZBAR_FOUND)
    target_compile_definitions(protohud PRIVATE HAVE_ZBAR)
endif()

if(SDL2_FOUND)
    target_compile_definitions(protohud PRIVATE HAVE_SDL2)
endif()

if(DBUS_FOUND)
    target_compile_definitions(protohud PRIVATE HAVE_DBUS)
endif()

target_compile_options(protohud PRIVATE
    ${LIBCAMERA_CFLAGS_OTHER}
    ${EGL_CFLAGS_OTHER}
    ${GLES_CFLAGS_OTHER}
    ${GLFW3_CFLAGS_OTHER}
)

target_compile_definitions(protohud PRIVATE
    GIT_HASH="${GIT_HASH_STR}"
)

# Ensure libglasses.so is found at runtime relative to the binary
set_target_properties(protohud PROPERTIES
    BUILD_RPATH   "${VITURE_LIB_DIR}"
    INSTALL_RPATH "\$ORIGIN/../lib"
)

# ── Assets & Config ────────────────────────────────────────���──────────────────

# ── Native face renderer (Protoface C++ port) ─────────────────────────────────
# Static library linked into protohud (NativeFaceController is the in-process
# face backend, selected via config "protoface.mode": "native"). EXCLUDE_FROM_ALL
# keeps it out of the default "all" target, but linking it into protohud forces
# it to build. It can also be compile-checked in isolation:
#     cmake --build build --target protoface_native
add_library(protoface_native STATIC EXCLUDE_FROM_ALL
    src/face/face_state.cpp
    src/face/materials.cpp
    src/face/renderer.cpp
    src/face/face_loader.cpp
    src/face/particles.cpp
    src/face/gif_player.cpp
    src/face/eye_animations.cpp
    src/face/shm_pusher_output.cpp
    src/face/max7219_chain.cpp
    src/face/max7219_panel_output.cpp
    src/face/neopixel_matrix_chain.cpp
    src/face/neopixel_matrix_output.cpp
    src/face/native_face_controller.cpp
)
target_include_directories(protoface_native PRIVATE
    src/
    ${OpenCV_INCLUDE_DIRS}
    ${stb_SOURCE_DIR}            # stb_image.h for GIF decoding
)
target_link_libraries(protoface_native PRIVATE
    ${OpenCV_LIBS}
    nlohmann_json::nlohmann_json
    Threads::Threads
)

configure_file(config/config.example.json ${CMAKE_BINARY_DIR}/config.json COPYONLY)

# Copy shaders to build directory.
# configure_file() silently skips files when the destination directory doesn't
# exist yet, so we use file(MAKE_DIRECTORY) + file(COPY) for a reliable
# configure-time copy, and add a POST_BUILD command so shader edits are picked
# up on the next ninja run without requiring a full reconfigure.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/assets/shaders")
file(COPY "${CMAKE_SOURCE_DIR}/assets/shaders/"
     DESTINATION "${CMAKE_BINARY_DIR}/assets/shaders")

# Copy notification/widget icons (PNG) to the build directory.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/assets/icons")
file(COPY "${CMAKE_SOURCE_DIR}/assets/icons/"
     DESTINATION "${CMAKE_BINARY_DIR}/assets/icons")

# Copy VITURE .so files next to the binary for in-tree runs
add_custom_command(TARGET protohud POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VITURE_LIB_DIR}/libglasses.so"
        "${VITURE_LIB_DIR}/libcarina_vio.so"
        "$<TARGET_FILE_DIR:protohud>"
    COMMENT "Copying VITURE SDK libraries to build directory"
)

# Keep shaders in sync with source on every build (no reconfigure needed after edits)
add_custom_command(TARGET protohud POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/shaders"
        "$<TARGET_FILE_DIR:protohud>/assets/shaders"
    COMMENT "Syncing shaders to build directory"
)

# Keep icons in sync with source on every build (drop PNGs in assets/icons, rebuild)
add_custom_command(TARGET protohud POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/icons"
        "$<TARGET_FILE_DIR:protohud>/assets/icons"
    COMMENT "Syncing icons to build directory"
)

install(TARGETS protohud RUNTIME DESTINATION bin)
install(FILES config/config.example.json DESTINATION etc/protohud RENAME config.json)
# Companion scheduler daemon (Python; owns the web form + Google Calendar sync).
# Exclude any real OAuth secrets/tokens so they never get packaged.
install(DIRECTORY scheduler_daemon DESTINATION share/protohud
        PATTERN "token.json"          EXCLUDE
        PATTERN "client_secret*.json" EXCLUDE
        PATTERN "__pycache__"         EXCLUDE)
install(FILES "${VITURE_LIB_DIR}/libglasses.so"
              "${VITURE_LIB_DIR}/libcarina_vio.so"
        DESTINATION lib)
