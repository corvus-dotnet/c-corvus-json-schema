cmake_minimum_required(VERSION 3.16)
project(bowtie_corvus_json_schema C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The C library's release package (its CMake package), linked statically.
find_package(corvus_json_schema REQUIRED)
add_executable(bowtie_corvus_json_schema bowtie_corvus_json_schema.c)
target_compile_options(bowtie_corvus_json_schema PRIVATE -Wall -Wextra -Werror)
target_link_libraries(bowtie_corvus_json_schema PRIVATE corvus_json_schema::corvus_json_schema_static)
