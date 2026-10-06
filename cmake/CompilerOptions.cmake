# Warnings and sanitizers for this project's own targets (never third-party).
function(hpc_configure_target target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wconversion -Wsign-conversion -Wshadow
            -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
            -Wformat=2
        )
    endif()

    if(HPC_SANITIZE)
        if(MSVC)
            message(FATAL_ERROR "HPC_SANITIZE is only supported with GCC/Clang")
        endif()
        target_compile_options(${target} PRIVATE -fsanitize=${HPC_SANITIZE} -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=${HPC_SANITIZE})
    endif()
endfunction()
