# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(mesh_local_control)

# Add custom outbox implementation to mqtt component
#idf_component_get_property(mqtt mqtt COMPONENT_LIB)

#target_sources(${mqtt} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/main/custom_outbox.c)

#Get nvs_flash dependency
#idf_component_get_property(nvs_flash nvs_flash COMPONENT_LIB)

#Link nvs_flash to the mqtt target
#target_link_libraries(${mesh_lite} ${nvs_flash})



