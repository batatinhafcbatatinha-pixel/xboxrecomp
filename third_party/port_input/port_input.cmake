# port_input_stage(<target>): SDL3.dll (official 3.2.26 build) and gamecontrollerdb.txt next to <target>'s
# executable, for port_input (port_input.c loads SDL3.dll at run time). Pinned by hash, fetched once into
# <build>/port-input; when the download fails the game still builds and keeps its own input.
function(port_input_stage target)
    if(NOT WIN32)
        return()
    endif()
    set(dir "${CMAKE_BINARY_DIR}/port-input")
    set(db "${dir}/gamecontrollerdb.txt")
    if(NOT EXISTS "${db}")
        file(DOWNLOAD
            "https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/22a98f6339abf4b6ca6cb061443a70e4f7ab252d/gamecontrollerdb.txt"
            "${db}" EXPECTED_HASH SHA256=0f17831b6a9e0e9fc2f52bf8ab1bef1d30ffd307d966c2118e556db345ea7e96
            STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            message(WARNING "gamecontrollerdb.txt was not downloaded: ${status}")
            file(REMOVE "${db}")
        endif()
    endif()
    if(NOT EXISTS "${dir}/SDL3.dll")
        set(zip "${dir}/SDL3-3.2.26-win32-x64.zip")
        file(DOWNLOAD "https://github.com/libsdl-org/SDL/releases/download/release-3.2.26/SDL3-3.2.26-win32-x64.zip"
            "${zip}" EXPECTED_HASH SHA256=866bd702f16cc767420ed401e8c5663679833a0de56892f03e4ef3e9c0a5a333
            STATUS status)
        list(GET status 0 code)
        if(code EQUAL 0)
            file(ARCHIVE_EXTRACT INPUT "${zip}" DESTINATION "${dir}" PATTERNS SDL3.dll LICENSE.txt)
        else()
            message(WARNING "SDL3.dll was not downloaded: ${status}")
        endif()
        file(REMOVE "${zip}")
    endif()
    if(EXISTS "${dir}/SDL3.dll")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dir}/SDL3.dll" "$<TARGET_FILE_DIR:${target}>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dir}/LICENSE.txt" "$<TARGET_FILE_DIR:${target}>/SDL3-LICENSE.txt")
    endif()
    if(EXISTS "${db}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${db}" "$<TARGET_FILE_DIR:${target}>")
    endif()
endfunction()
