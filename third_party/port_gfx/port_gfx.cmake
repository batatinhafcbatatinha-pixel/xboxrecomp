# port_gfx_add(<target>): builds port_gfx (Vulkan presentation, FSR 1.0, DLSS; see port_gfx.h) into <target>.
# Nothing to download or link: Vulkan and NVIDIA's NGX are loaded at run time.
set(PORT_GFX_DIR "${CMAKE_CURRENT_LIST_DIR}")
function(port_gfx_add target)
    target_sources(${target} PRIVATE "${PORT_GFX_DIR}/port_gfx.c")
    target_include_directories(${target} PRIVATE "${PORT_GFX_DIR}" "${PORT_GFX_DIR}/third_party")
    target_compile_definitions(${target} PRIVATE PORT_GFX=1)
endfunction()

# port_dlss_add(<target>): DLSS through Direct3D 12 (port_dlss.h) for a runtime that renders with it. NGX is loaded at
# run time; nothing to link. For a C++-only project the file builds as C++.
function(port_dlss_add target)
    target_sources(${target} PRIVATE "${PORT_GFX_DIR}/port_dlss.c")
    target_include_directories(${target} PRIVATE "${PORT_GFX_DIR}")
    target_compile_definitions(${target} PRIVATE PORT_DLSS=1)
endfunction()
