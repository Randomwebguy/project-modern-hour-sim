# AMD64 asset/state compatibility, linked in process. D3D11 handles rendering.
file(GLOB RESOURCE64_SOURCE CONFIGURE_DEPENDS source/*.cpp)
add_library(genzh_resource64 STATIC ${RESOURCE64_SOURCE})
target_compile_features(genzh_resource64 PRIVATE cxx_std_17)
target_compile_definitions(genzh_resource64 PRIVATE D3D8TO9NOLOG WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(genzh_resource64 PRIVATE /W3 /we4311 /we4312 /we4302)
target_link_libraries(genzh_resource64 PUBLIC d3d9)
