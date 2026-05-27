cmake_minimum_required(VERSION 3.18)
project(GTASAClient CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Tamano minimo, simbolos ocultos por default (solo los AML exportados
# se marcan explicit con visibility=default en main.cpp).
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden -ffunction-sections -fdata-sections")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections -Wl,--strip-all")

add_library(GTASAClient SHARED src/main.cpp)

target_include_directories(GTASAClient PRIVATE
    src
    include
)

set_source_files_properties(src/main.cpp PROPERTIES
    COMPILE_FLAGS "-fvisibility=default"
)

target_link_libraries(GTASAClient PRIVATE log android dl)

set_target_properties(GTASAClient PROPERTIES
    OUTPUT_NAME "GTASAClient"
    PREFIX "lib"
    SUFFIX ".so"
)
