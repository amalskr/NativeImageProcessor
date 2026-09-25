cmake_minimum_required(VERSION 3.22.1)

project("nativeimageprocessor")

add_library(
        native-image-processor
        SHARED
        src/main/cpp/native_image_processor.cpp
        src/main/cpp/video_watermark.cpp
)

# Prebuilt FFmpeg (see scripts/build_ffmpeg.sh). AGP packages these .so files automatically.
set(FFMPEG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/ffmpeg)
set(FFMPEG_LIBS avutil avcodec avformat swscale)
foreach (lib ${FFMPEG_LIBS})
    add_library(${lib} SHARED IMPORTED)
    set_target_properties(${lib} PROPERTIES
            IMPORTED_LOCATION ${FFMPEG_DIR}/lib/${ANDROID_ABI}/lib${lib}.so)
endforeach ()
target_include_directories(native-image-processor PRIVATE ${FFMPEG_DIR}/include)

find_library(log-lib log)
find_library(jnigraphics-lib jnigraphics)

target_link_libraries(
        native-image-processor
        ${FFMPEG_LIBS}
        ${log-lib}
        ${jnigraphics-lib}
)
