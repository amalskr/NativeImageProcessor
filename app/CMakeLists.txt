cmake_minimum_required(VERSION 3.22.1)

project("nativeimageprocessor")

add_library(
        native-image-processor
        SHARED
        src/main/cpp/native_image_processor.cpp
)

find_library(log-lib log)
find_library(jnigraphics-lib jnigraphics)

target_link_libraries(
        native-image-processor
        ${log-lib}
        ${jnigraphics-lib}
)
