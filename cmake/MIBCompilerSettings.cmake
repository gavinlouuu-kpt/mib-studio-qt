# Suppress character encoding warnings from third-party headers.
if(MSVC)
    # C/C++ only: add_compile_options also reaches MASM (crashpad's
    # capture_context_win.asm), and ml64 rejects cl flags — the auto-beta
    # runs from 2026-09-08 failed with LNK1181 on that .obj.
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/wd4828>)
    # Conformance mode. With Qt in the link, Qt6::Platform propagated
    # /permissive- and /Zc:__cplusplus to every target; the Qt-free backend
    # and its tests must compile the same way (e.g. a functional cast such as
    # `int(i)` is a prvalue only in conformance mode).
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/permissive-> $<$<COMPILE_LANGUAGE:C,CXX>:/Zc:__cplusplus>)
endif()

# Preserve PDBs and address-to-source mapping for Release builds so that
# minidumps from end-user crashes can be symbolicated. /Zi generates the
# .pdb; /DEBUG instructs the linker to emit and keep it; /OPT:REF /OPT:ICF
# restore Release optimizations that /DEBUG would otherwise disable.
if(MSVC)
    add_compile_options($<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Zi>)
    add_link_options(
        $<$<CONFIG:Release>:/DEBUG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>)
endif()

# Optional sanitizers for the CI sanitizer lane (GCC/Clang on Linux). Set
# -DMIB_SANITIZER to one of: thread | address | undefined | address+undefined.
# No-op on MSVC. ThreadSanitizer catches the lost-wakeup / data-race class that
# produced the trigger and FrameStore bugs; ASan+UBSan catch memory/UB.
set(MIB_SANITIZER "" CACHE STRING
    "Sanitizer build: thread | address | undefined | address+undefined")
if(MIB_SANITIZER AND NOT MSVC)
    if(MIB_SANITIZER STREQUAL "thread")
        set(_mib_san "-fsanitize=thread")
    elseif(MIB_SANITIZER STREQUAL "address")
        set(_mib_san "-fsanitize=address")
    elseif(MIB_SANITIZER STREQUAL "undefined")
        set(_mib_san "-fsanitize=undefined")
    elseif(MIB_SANITIZER STREQUAL "address+undefined")
        set(_mib_san "-fsanitize=address,undefined")
    else()
        message(FATAL_ERROR "Unknown MIB_SANITIZER='${MIB_SANITIZER}' "
            "(use thread|address|undefined|address+undefined)")
    endif()
    message(STATUS "MIB sanitizer enabled: ${MIB_SANITIZER} (${_mib_san})")
    add_compile_options(-g -fno-omit-frame-pointer ${_mib_san})
    add_link_options(${_mib_san})
endif()
