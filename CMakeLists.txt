cmake_minimum_required(VERSION 3.13)

project(wibo LANGUAGES ASM C CXX)

set(CMAKE_CXX_STANDARD 20)

set(WIBO_VERSION "" CACHE STRING "Version string for the wibo binary; if empty, attempts to use git describe")

if(NOT "${WIBO_VERSION}" STREQUAL "")
    set(WIBO_VERSION_STRING "${WIBO_VERSION}")
elseif(DEFINED ENV{WIBO_VERSION} AND NOT "$ENV{WIBO_VERSION}" STREQUAL "")
    set(WIBO_VERSION_STRING "$ENV{WIBO_VERSION}")
else()
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --always
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            RESULT_VARIABLE WIBO_GIT_DESCRIBE_RESULT
            OUTPUT_VARIABLE WIBO_GIT_DESCRIBE_OUTPUT
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(WIBO_GIT_DESCRIBE_RESULT EQUAL 0 AND NOT "${WIBO_GIT_DESCRIBE_OUTPUT}" STREQUAL "")
            set(WIBO_VERSION_STRING "${WIBO_GIT_DESCRIBE_OUTPUT}")
        endif()
    endif()
endif()

if(NOT DEFINED WIBO_VERSION_STRING OR "${WIBO_VERSION_STRING}" STREQUAL "")
    set(WIBO_VERSION_STRING "unknown")
endif()

set(WIBO_GENERATED_HEADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${WIBO_GENERATED_HEADER_DIR})
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/version_info.h.in
    ${WIBO_GENERATED_HEADER_DIR}/version_info.h
    @ONLY
)

# Detect if we're building for 64-bit based on the target architecture
# CMAKE_SIZEOF_VOID_P is set by the project() command and the toolchain
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WIBO_64 ON)
    message(STATUS "Detected 64-bit target architecture")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(WIBO_64 OFF)
    message(STATUS "Detected 32-bit target architecture")
else()
    message(FATAL_ERROR "Unknown pointer size: ${CMAKE_SIZEOF_VOID_P}")
endif()

option(WIBO_ENABLE_FIXTURE_TESTS "Enable Win32 fixture tests (requires i686-w64-mingw32)" ON)
option(WIBO_ENABLE_LIBURING "Enable liburing for asynchronous I/O" OFF)
option(WIBO_ENABLE_WINE_DLLS "Download Wine CRT DLLs from encounter/winedll by default" ON)
option(WIBO_BUILD_SYSTEM_PROVIDER "Build the optional Windows service adapter" OFF)
set(WIBO_GUEST_ARCH "x86" CACHE STRING "Windows guest architecture (x86 or x86_64)")
if(WIBO_BUILD_SYSTEM_PROVIDER)
    if(WIBO_GUEST_ARCH STREQUAL "x86_64")
        set(_provider_compiler x86_64-w64-mingw32-g++)
    else()
        set(_provider_compiler i686-w64-mingw32-g++)
    endif()
    find_program(WIBO_PROVIDER_CXX NAMES ${_provider_compiler} REQUIRED)
    set(_provider_image ${CMAKE_CURRENT_BINARY_DIR}/tools/system_provider.exe)
    add_custom_command(OUTPUT ${_provider_image}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/tools
        COMMAND ${WIBO_PROVIDER_CXX} -std=c++20 -Wall -Wextra -O2 -municode -static
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/system_provider.cpp
            -lole32 -loleaut32 -lwbemuuid -luuid -luser32 -lgdi32 -liphlpapi -lshell32 -lsetupapi -o ${_provider_image}
        DEPENDS tools/system_provider.cpp tools/adapter_addresses.h src/security_descriptor.h VERBATIM)
    add_custom_target(system_provider ALL DEPENDS ${_provider_image})
endif()
set_property(CACHE WIBO_GUEST_ARCH PROPERTY STRINGS x86 x86_64)
if(WIBO_GUEST_ARCH STREQUAL "x86_64")
    if(NOT WIBO_64)
        message(FATAL_ERROR "x86_64 Windows guests require a 64-bit Wibo host")
    endif()
    set(WIBO_GUEST_64 ON)
elseif(WIBO_GUEST_ARCH STREQUAL "x86")
    set(WIBO_GUEST_64 OFF)
else()
    message(FATAL_ERROR "WIBO_GUEST_ARCH must be x86 or x86_64")
endif()
set(WIBO_ENABLE_LTO "AUTO" CACHE STRING "Enable link-time optimization (LTO)")
set_property(CACHE WIBO_ENABLE_LTO PROPERTY STRINGS "AUTO" "ON" "OFF")

add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

if (WIBO_ENABLE_LTO STREQUAL "AUTO")
    if (CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
        include(CheckIPOSupported)
        check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
        if (ipo_supported)
            message(STATUS "Enabling LTO")
            set(WIBO_ENABLE_LTO ON)
        else()
            message(STATUS "LTO not supported: ${ipo_error}")
            set(WIBO_ENABLE_LTO OFF)
        endif()
    else()
        message(STATUS "Disabling LTO")
        set(WIBO_ENABLE_LTO OFF)
    endif()
elseif (NOT (WIBO_ENABLE_LTO STREQUAL "ON" OR WIBO_ENABLE_LTO STREQUAL "OFF"))
    message(FATAL_ERROR "WIBO_ENABLE_LTO must be ON, OFF, or AUTO")
endif()
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${WIBO_ENABLE_LTO})

# Fetch and configure mimalloc
set(MI_BUILD_SHARED OFF CACHE BOOL "Build shared library" FORCE)
set(MI_BUILD_STATIC OFF CACHE BOOL "Build static library" FORCE)
set(MI_BUILD_OBJECT ON CACHE BOOL "Build object library" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "Build test executables" FORCE)
# Keep mimalloc as the explicit low-address guest-heap implementation, but do
# not replace the host process allocator for Win64 guests under Rosetta. Host
# loader bookkeeping and libc++ containers must remain on macOS's native
# allocator; routing them through mimalloc has wedged translated workers inside
# mimalloc's multithreaded bookkeeping and made the process uninterruptible.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND WIBO_GUEST_64)
    set(MI_OVERRIDE OFF CACHE BOOL "Override the standard malloc interface" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(
    mimalloc
    GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
    GIT_TAG        69c5c5c402bf0414ff4a366697ecbd5c7578dc02  # dev3 (2025-11-05)
)
FetchContent_MakeAvailable(mimalloc)

# Disable `note: the alignment of '_Atomic long long int' fields changed in GCC 11.1`
target_compile_options(mimalloc-obj PRIVATE -Wno-psabi)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(mimalloc-obj PRIVATE MI_USE_BUILTIN_THREAD_POINTER=1)
endif()
if (WIBO_64)
    # Disable a noisy warning on startup
    target_compile_definitions(mimalloc-obj PRIVATE MI_DEBUG=0)
endif()

if (WIBO_ENABLE_LIBURING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    FetchContent_Declare(
        liburing
        GIT_REPOSITORY https://github.com/axboe/liburing.git
        GIT_TAG        e907d6a342e80b70874f93abd440b92b8a40b7bc  # liburing-2.12
    )
    FetchContent_MakeAvailable(liburing)

    add_custom_command(
        OUTPUT  ${liburing_SOURCE_DIR}/config-host.h
                ${liburing_SOURCE_DIR}/src/include/liburing/compat.h
        COMMAND ${CMAKE_COMMAND} -E env
                CC=${CMAKE_C_COMPILER}
                CXX=${CMAKE_CXX_COMPILER}
                ./configure --prefix=${liburing_BINARY_DIR}
        WORKING_DIRECTORY ${liburing_SOURCE_DIR}
        DEPENDS ${liburing_SOURCE_DIR}/configure
        COMMENT "Running liburing configure"
        VERBATIM
    )
    add_library(liburing STATIC
        ${liburing_SOURCE_DIR}/config-host.h
        ${liburing_SOURCE_DIR}/src/nolibc.c
        ${liburing_SOURCE_DIR}/src/queue.c
        ${liburing_SOURCE_DIR}/src/register.c
        ${liburing_SOURCE_DIR}/src/setup.c
        ${liburing_SOURCE_DIR}/src/syscall.c
        ${liburing_SOURCE_DIR}/src/version.c
        ${liburing_SOURCE_DIR}/src/include/liburing/compat.h)
    target_compile_definitions(liburing PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64 _TIME_BITS=64)
    target_compile_options(liburing PRIVATE -include config-host.h)
    target_include_directories(liburing PUBLIC ${liburing_SOURCE_DIR}/src/include)
    target_include_directories(liburing PRIVATE ${liburing_SOURCE_DIR})
endif()

set(WIBO_WINEDLL_RELEASE_TAG "2026-07-10")
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# The pinned encounter/winedll assets are PE32 and cannot be embedded into a
# Win64 Wibo. Resolve the matching PE32+ Wine CRT set explicitly instead of
# allowing a build that deterministically fails every 64-bit import.
set(WIBO_WIN64_CRT_ROOT "" CACHE PATH "Directory containing x86-64 Wine CRT DLLs")
if(WIBO_GUEST_64)
    if(NOT WIBO_WIN64_CRT_ROOT AND DEFINED ENV{WIBO_WIN64_CRT_ROOT})
        set(WIBO_WIN64_CRT_ROOT "$ENV{WIBO_WIN64_CRT_ROOT}")
    endif()
    if(NOT WIBO_WIN64_CRT_ROOT AND APPLE)
        set(_wibo_wine_stable_crt
            "/Applications/Wine Stable.app/Contents/Resources/wine/lib/wine/x86_64-windows")
        if(EXISTS "${_wibo_wine_stable_crt}/msvcrt.dll")
            set(WIBO_WIN64_CRT_ROOT "${_wibo_wine_stable_crt}")
        endif()
    endif()
    if(NOT EXISTS "${WIBO_WIN64_CRT_ROOT}/msvcrt.dll" OR
       NOT EXISTS "${WIBO_WIN64_CRT_ROOT}/ucrtbase.dll")
        message(FATAL_ERROR
            "Win64 Wibo requires PE32+ Wine CRTs; set WIBO_WIN64_CRT_ROOT to "
            "an x86_64-windows Wine DLL directory")
    endif()
    set(WIBO_WIN64_CRT_ROOT "${WIBO_WIN64_CRT_ROOT}" CACHE PATH
        "Directory containing x86-64 Wine CRT DLLs" FORCE)
    foreach(_wibo_crt_var_file IN ITEMS
            "MSVCRT_DLL;msvcrt.dll"
            "MSVCIRT_DLL;msvcirt.dll"
            "MSVCRT20_DLL;msvcrt20.dll"
            "MSVCRT40_DLL;msvcrt40.dll"
            "MSVCR71_DLL;msvcr71.dll"
            "MSVCR90_DLL;msvcr90.dll"
            "MSVCR100_DLL;msvcr100.dll"
            "MSVCR120_DLL;msvcr120.dll"
            "UCRTBASE_DLL;ucrtbase.dll")
        list(GET _wibo_crt_var_file 0 _wibo_crt_var)
        list(GET _wibo_crt_var_file 1 _wibo_crt_file)
        if(NOT EXISTS "${WIBO_WIN64_CRT_ROOT}/${_wibo_crt_file}")
            message(FATAL_ERROR
                "Win64 Wine CRT is missing: ${WIBO_WIN64_CRT_ROOT}/${_wibo_crt_file}")
        endif()
        execute_process(
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_pe_machine.py"
                --machine x86_64 "${WIBO_WIN64_CRT_ROOT}/${_wibo_crt_file}"
            RESULT_VARIABLE _wibo_crt_machine_result
            ERROR_VARIABLE _wibo_crt_machine_error
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_STRIP_TRAILING_WHITESPACE)
        if(NOT _wibo_crt_machine_result EQUAL 0)
            message(FATAL_ERROR "${_wibo_crt_machine_error}")
        endif()
        set(${_wibo_crt_var} "${WIBO_WIN64_CRT_ROOT}/${_wibo_crt_file}")
    endforeach()
endif()

function(configure_wine_dll VAR FILENAME SHA256)
    if (NOT DEFINED ${VAR})
        if (WIBO_ENABLE_WINE_DLLS)
            file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/vendor)
            set(${VAR} ${CMAKE_CURRENT_BINARY_DIR}/vendor/${FILENAME})
            file(DOWNLOAD
                https://github.com/encounter/winedll/releases/download/${WIBO_WINEDLL_RELEASE_TAG}/${FILENAME}
                ${${VAR}}
                EXPECTED_HASH SHA256=${SHA256}
            )
        else()
            set(${VAR} "")
        endif()
    endif()
    set(${VAR} "${${VAR}}" PARENT_SCOPE)
endfunction()

configure_wine_dll(MSVCRT_DLL msvcrt.dll 01f5b5a6f8b249e2842e74720376ca84ea6e8094c18b5416ec80a2183fc9ef81)
configure_wine_dll(MSVCIRT_DLL msvcirt.dll c170ef5e15c91cafe520e4342a42eef8f8ed2c007e20471b0ac1c0277e1e0333)
configure_wine_dll(MSVCRT20_DLL msvcrt20.dll fb94edfa05414460ea8f27b82acf205a0d3caccd68d66f7e7de8ae7d29ae4563)
configure_wine_dll(MSVCRT40_DLL msvcrt40.dll da49673adf20b5f8cf23e99b18278f3ea225d33ce47bd25e1455c654ff4fc5ec)
configure_wine_dll(MSVCR71_DLL msvcr71.dll ad5b9673604a205f24feb06283c93fdbd7b652fbe9c865c50dbf26505b3df463)
configure_wine_dll(MSVCR90_DLL msvcr90.dll c8682ba683665bd3d6b9ff8ee0b34af34edbcfd30e7682321b98941aefeae659)
configure_wine_dll(MSVCR100_DLL msvcr100.dll 405e44bced50a4b57c08de47e16e7d9792603c0af52bdd0fb19cff2c858b5880)
configure_wine_dll(MSVCR120_DLL msvcr120.dll fdb166b3eadd663fda65718334b72736a6f4aa3ec94a32d4c47e7a7add75b696)
configure_wine_dll(UCRTBASE_DLL ucrtbase.dll 9606bcdd8d8436ce4d5c995cada5c20ce3d6cb52c35a04a0366613950785c282)

function(embed_binary SRC FILE)
    set_source_files_properties(${SRC} PROPERTIES
        OBJECT_DEPENDS "${FILE}"
        COMPILE_DEFINITIONS "EMBED_PATH=\"${FILE}\""
    )
endfunction()

function(add_embedded_wine_dll TARGET SRC FLAG FILE)
    if ("${FILE}" STREQUAL "")
        target_compile_definitions(${TARGET} PRIVATE ${FLAG}=0)
    else()
        target_compile_definitions(${TARGET} PRIVATE ${FLAG}=1)
        target_sources(${TARGET} PRIVATE ${SRC})
        embed_binary(${SRC} "${FILE}")
    endif()
endfunction()

add_executable(wibo
    dll/advapi32.cpp
    dll/advapi32/evntrace.cpp
    dll/advapi32/evntprov.cpp
    dll/advapi32/md5.c
    dll/advapi32/processthreadsapi.cpp
    dll/advapi32/securitybaseapi.cpp
    dll/advapi32/winbase.cpp
    dll/advapi32/wincrypt.cpp
    dll/advapi32/winreg.cpp
    dll/bcrypt.cpp
    dll/dbghelp.cpp
    dll/kernel32.cpp
    dll/kernel32/debugapi.cpp
    dll/kernel32/errhandlingapi.cpp
    dll/kernel32/vectored_exception.cpp
    dll/kernel32/jobapi.cpp
    dll/kernel32/formatmessage.cpp
    dll/kernel32/fibersapi.cpp
    dll/kernel32/directory_changes.cpp
    dll/kernel32/fileapi.cpp
    dll/kernel32/handleapi.cpp
    dll/kernel32/heapapi.cpp
    dll/kernel32/interlockedapi.cpp
    dll/kernel32/ioapiset.cpp
    dll/kernel32/libloaderapi.cpp
    dll/kernel32/memoryapi.cpp
    dll/kernel32/memoryresourceapi.cpp
    dll/kernel32/memorystatus.cpp
    dll/kernel32/namedpipeapi.cpp
    dll/kernel32/processenv.cpp
    dll/kernel32/processthreadsapi.cpp
    dll/kernel32/thread_control.cpp
    dll/kernel32/unwindapi.cpp
    dll/kernel32/system_times.cpp
    dll/kernel32/profileapi.cpp
    dll/kernel32/stringapiset.cpp
    dll/kernel32/synchapi.cpp
    dll/kernel32/apc.cpp
    dll/kernel32/work_queue.cpp
    dll/kernel32/completion_port.cpp
    dll/kernel32/condition_variable.cpp
    dll/kernel32/timers.cpp
    dll/kernel32/sysinfoapi.cpp
    dll/kernel32/timezoneapi.cpp
	dll/kernel32/toolhelp.cpp
    dll/kernel32/winbase.cpp
    dll/kernel32/winprofile.cpp
    dll/kernel32/wincon.cpp
    dll/kernel32/winnls.cpp
    dll/kernel32/winnt.cpp
    dll/kernel32/wow64apiset.cpp
    dll/lmgr.cpp
    dll/mscoree.cpp
    dll/ntdll.cpp
    dll/function_table.cpp
    dll/virtual_unwind.cpp
    dll/context_x64.S
    dll/status_error.cpp
    dll/device_control.cpp
    dll/native_file_set.cpp
    dll/volume_query.cpp
    dll/native_system_query.cpp
    dll/wmi_proxy.cpp
    dll/network_proxy.cpp
    dll/ole32.cpp
    dll/combase.cpp
    dll/com_context.cpp
    dll/oleaut32.cpp
    dll/psapi.cpp
    dll/setupapi.cpp
    dll/rpcrt4.cpp
    dll/iphlpapi.cpp
    dll/shlwapi.cpp
    dll/shell32.cpp
    dll/user32.cpp
    dll/user32/image.cpp
    dll/user32/metrics.cpp
    dll/user32/window_class.cpp
    dll/vcruntime.cpp
    dll/version.cpp
    dll/winmm.cpp
    dll/ws2/async_io.cpp
    dll/ws2/overlapped.cpp
    dll/ws2/accept_ex.cpp
    dll/ws2.cpp
    dll/ws2/service_lookup.cpp
    dll/ws2/resolution.cpp
    dll/ws2/sockets.cpp
    dll/ws2/events.cpp
    dll/ws2/options.cpp
    dll/ws2/wait.cpp
    dll/ws2/receive.cpp
    src/access.cpp
    src/async_io.cpp
    src/errors.cpp
    src/system_provider.cpp
    src/file_locks.cpp
    src/files.cpp
    src/handles.cpp
    src/heap.cpp
    src/loader.cpp
    src/main.cpp
    src/modules.cpp
    src/processes_common.cpp
    src/resources.cpp
    src/setup.S
    src/strutil.cpp
    src/tls.cpp
)

if(WIBO_GUEST_64)
    target_sources(wibo PRIVATE
        dll/software_exception_prepare.cpp dll/software_exception_callback_x64.S
        dll/software_exception_decision.cpp dll/software_exception_dispatch.cpp
        dll/software_exception_dispatch_x64.S dll/software_exception_failure.cpp)
    target_compile_definitions(wibo PRIVATE WIBO_GUEST_64=1)
    set_target_properties(wibo PROPERTIES OUTPUT_NAME wibo64)
endif()
add_embedded_wine_dll(wibo dll/msvcrt.cpp WIBO_HAS_MSVCRT "${MSVCRT_DLL}")
add_embedded_wine_dll(wibo dll/msvcirt.cpp WIBO_HAS_MSVCIRT "${MSVCIRT_DLL}")
add_embedded_wine_dll(wibo dll/msvcrt20.cpp WIBO_HAS_MSVCRT20 "${MSVCRT20_DLL}")
add_embedded_wine_dll(wibo dll/msvcrt40.cpp WIBO_HAS_MSVCRT40 "${MSVCRT40_DLL}")
add_embedded_wine_dll(wibo dll/msvcr71.cpp WIBO_HAS_MSVCR71 "${MSVCR71_DLL}")
add_embedded_wine_dll(wibo dll/msvcr90.cpp WIBO_HAS_MSVCR90 "${MSVCR90_DLL}")
add_embedded_wine_dll(wibo dll/msvcr100.cpp WIBO_HAS_MSVCR100 "${MSVCR100_DLL}")
add_embedded_wine_dll(wibo dll/msvcr120.cpp WIBO_HAS_MSVCR120 "${MSVCR120_DLL}")
add_embedded_wine_dll(wibo dll/ucrtbase.cpp WIBO_HAS_UCRTBASE "${UCRTBASE_DLL}")

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(wibo PRIVATE
        src/async_io_epoll.cpp
        src/processes_linux.cpp
        src/setup_linux.cpp
    )
elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(wibo PRIVATE
        src/async_io_kqueue.cpp
        src/processes_darwin.cpp
        src/setup_darwin.cpp
    )
    set_source_files_properties(src/setup_darwin.cpp PROPERTIES COMPILE_FLAGS "-fms-extensions")
else()
    message(FATAL_ERROR "Unsupported platform for ProcessManager")
endif()
target_compile_definitions(wibo PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64 _TIME_BITS=64)
target_compile_features(wibo PRIVATE cxx_std_20)
target_compile_options(wibo PRIVATE -Wall -Wextra)
if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_options(wibo PRIVATE
        -Wl,-no_pie,-no_fixup_chains
        -Wl,-segalign,0x1000
        -Wl,-pagezero_size,0x1000
        -Wl,-image_base,0x7E001000
        -Wl,-segaddr,RESV32,0x1000
        -Wl,-segprot,RESV32,-,-
    )
else()
    target_link_options(wibo PRIVATE -no-pie -Wl,--image-base=0x70000000)
endif()
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(wibo PRIVATE -maccumulate-outgoing-args)
    target_link_options(wibo PRIVATE -maccumulate-outgoing-args)
endif()
target_include_directories(wibo PRIVATE dll src ${WIBO_GENERATED_HEADER_DIR})
target_link_libraries(wibo PRIVATE mimalloc-obj)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(wibo PRIVATE atomic)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_libraries(wibo PRIVATE c++ c++abi "-framework CoreServices" "-framework CoreFoundation")
endif()
if (WIBO_ENABLE_LIBURING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(wibo PRIVATE WIBO_ENABLE_LIBURING=1)
    target_link_libraries(wibo PRIVATE liburing)
    target_sources(wibo PRIVATE src/async_io_uring.cpp)
else()
    target_compile_definitions(wibo PRIVATE WIBO_ENABLE_LIBURING=0)
endif()
install(TARGETS wibo DESTINATION bin)

# Check for features
include(CheckSymbolExists)
set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
set(CMAKE_REQUIRED_LIBRARIES pthread)
check_symbol_exists(pthread_gettid_np "pthread.h" HAVE_PTHREAD_GETTID_NP)
if (HAVE_PTHREAD_GETTID_NP)
    target_compile_definitions(wibo PRIVATE -DHAVE_PTHREAD_GETTID_NP=1)
endif()

# Track down libclang for trampoline generation.
# find_package(Clang) ends up requiring too many dependencies,
# so a quick-and-dirty manual search is done here instead.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CLANG_ROOT "/usr" CACHE PATH "Path to Clang installation")
elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(CLANG_ROOT "/Library/Developer/CommandLineTools/usr/lib" CACHE PATH "Path to Clang installation")
endif()
set(_LLVM_MIN_VER 17)
set(_LLVM_MAX_VER 25)
set(_CLANG_LIB_SUFFIXES "")
foreach(ver RANGE ${_LLVM_MAX_VER} ${_LLVM_MIN_VER} -1)
    list(APPEND _CLANG_LIB_SUFFIXES "llvm-${ver}/lib")
endforeach()
list(APPEND _CLANG_LIB_SUFFIXES llvm/lib lib lib64)
find_library(LIBCLANG_LIBRARY
    NAMES libclang clang
    HINTS ${CLANG_ROOT}
    PATH_SUFFIXES ${_CLANG_LIB_SUFFIXES}
)
if(NOT LIBCLANG_LIBRARY)
    message(FATAL_ERROR "libclang library not found!")
endif()
message(STATUS "Using libclang library: ${LIBCLANG_LIBRARY}")

if(WIBO_BUILD_SYSTEM_PROVIDER)
    add_dependencies(wibo system_provider)
endif()

function(wibo_codegen_module)
    set(options)
    set(oneValueArgs NAME)
    set(multiValueArgs HEADERS)
    cmake_parse_arguments(PARSE_ARGV 0 module "${options}" "${oneValueArgs}" "${multiValueArgs}")

    set(codegen_system_arguments)
    foreach(directory IN LISTS CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
        list(APPEND codegen_system_arguments --clang-arg=-isystem --clang-arg=${directory})
    endforeach()

    set(out_asm ${WIBO_GENERATED_HEADER_DIR}/${module_NAME}_trampolines.S)
    set(out_hdr ${WIBO_GENERATED_HEADER_DIR}/${module_NAME}_trampolines.h)
    add_custom_command(
        OUTPUT ${out_asm} ${out_hdr}
        COMMAND ${CMAKE_COMMAND} -E env
                ${CMAKE_COMMAND} -E echo "Generating ${module_NAME} trampolines"
        COMMAND ${CMAKE_COMMAND} -E env
                LIBCLANG_PATH=${LIBCLANG_LIBRARY}
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_trampolines.py
                --dll ${module_NAME}
                --headers ${module_HEADERS}
                --namespace ${module_NAME}
                --arch $<IF:$<BOOL:${WIBO_64}>,x86_64,x86>
                --guest-arch ${WIBO_GUEST_ARCH}
                ${codegen_system_arguments}
                --out-asm ${out_asm}
                --out-hdr ${out_hdr}
                -I ${CMAKE_CURRENT_SOURCE_DIR}
                -I ${CMAKE_CURRENT_SOURCE_DIR}/dll
                -I ${CMAKE_CURRENT_SOURCE_DIR}/src
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_trampolines.py
                ${module_HEADERS}
        VERBATIM
    )
    target_sources(wibo PRIVATE ${out_asm})
endfunction()

wibo_codegen_module(NAME advapi32 HEADERS
    dll/advapi32/evntrace.h
    dll/advapi32/evntprov.h
    dll/advapi32/processthreadsapi.h
    dll/advapi32/securitybaseapi.h
    dll/advapi32/winbase.h
    dll/advapi32/wincrypt.h
    dll/advapi32/winreg.h
)
wibo_codegen_module(NAME bcrypt HEADERS dll/bcrypt.h)
wibo_codegen_module(NAME dbghelp HEADERS dll/dbghelp.h)
wibo_codegen_module(NAME entry HEADERS src/entry.h)
wibo_codegen_module(NAME mscoree HEADERS dll/mscoree.h)
wibo_codegen_module(NAME version HEADERS dll/version.h)
wibo_codegen_module(NAME winmm HEADERS dll/winmm.h)
wibo_codegen_module(NAME rpcrt4 HEADERS dll/rpcrt4.h)
wibo_codegen_module(NAME iphlpapi HEADERS dll/iphlpapi.h)
wibo_codegen_module(NAME shlwapi HEADERS dll/shlwapi.h)
wibo_codegen_module(NAME shell32 HEADERS dll/shell32.h)
wibo_codegen_module(NAME ws2 HEADERS dll/ws2.h)
wibo_codegen_module(NAME mswsock HEADERS dll/mswsock.h)
wibo_codegen_module(NAME vcruntime HEADERS dll/vcruntime.h)
wibo_codegen_module(NAME lmgr HEADERS dll/lmgr.h)
wibo_codegen_module(NAME network_proxy HEADERS dll/network_proxy.h)
wibo_codegen_module(NAME wmi_proxy HEADERS dll/wmi_proxy.h)
wibo_codegen_module(NAME ole32 HEADERS dll/ole32.h)
wibo_codegen_module(NAME combase HEADERS dll/combase.h)
wibo_codegen_module(NAME com_context HEADERS dll/com_context.h)
wibo_codegen_module(NAME oleaut32 HEADERS dll/oleaut32.h)
wibo_codegen_module(NAME psapi HEADERS dll/psapi.h)
wibo_codegen_module(NAME setupapi HEADERS dll/setupapi.h)
wibo_codegen_module(NAME user32 HEADERS dll/user32.h)
wibo_codegen_module(NAME ntdll HEADERS dll/ntdll.h)
wibo_codegen_module(NAME kernel32 HEADERS
    dll/kernel32/debugapi.h
    dll/kernel32/errhandlingapi.h
    dll/kernel32/jobapi.h
    dll/kernel32/fibersapi.h
    dll/kernel32/fileapi.h
    dll/kernel32/handleapi.h
    dll/kernel32/heapapi.h
    dll/kernel32/interlockedapi.h
    dll/kernel32/ioapiset.h
    dll/kernel32/libloaderapi.h
    dll/kernel32/memoryapi.h
    dll/kernel32/memoryresourceapi.h
    dll/kernel32/namedpipeapi.h
    dll/kernel32/processenv.h
    dll/kernel32/processthreadsapi.h
    dll/kernel32/profileapi.h
    dll/kernel32/stringapiset.h
    dll/kernel32/synchapi.h
    dll/kernel32/sysinfoapi.h
    dll/kernel32/timezoneapi.h
	dll/kernel32/toolhelp.h
    dll/kernel32/unwindapi.h
    dll/kernel32/winbase.h
    dll/kernel32/winprofile.h
    dll/kernel32/wincon.h
    dll/kernel32/winnls.h
    dll/kernel32/winnt.h
    dll/kernel32/wow64apiset.h
)

if (WIBO_ENABLE_FIXTURE_TESTS)
    include(CTest)
    add_test(NAME wibo.test_codegen_diagnostics
        COMMAND ${CMAKE_COMMAND} -E env LIBCLANG_PATH=${LIBCLANG_LIBRARY}
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/test_codegen_diagnostics.py)
    set_tests_properties(wibo.test_codegen_diagnostics PROPERTIES TIMEOUT 30)

    if(WIBO_GUEST_64)
        find_program(WIBO_MINGW_CC x86_64-w64-mingw32-gcc)
        find_program(WIBO_MINGW_WINDRES x86_64-w64-mingw32-windres)
        find_program(WIBO_MINGW_DLLTOOL x86_64-w64-mingw32-dlltool)
    else()
        find_program(WIBO_MINGW_CC i686-w64-mingw32-gcc)
        find_program(WIBO_MINGW_WINDRES i686-w64-mingw32-windres)
        find_program(WIBO_MINGW_DLLTOOL i686-w64-mingw32-dlltool)
    endif()

    set(WIBO_HAVE_MINGW_TOOLCHAIN FALSE)
    if(WIBO_MINGW_CC AND WIBO_MINGW_WINDRES AND WIBO_MINGW_DLLTOOL)
        set(WIBO_HAVE_MINGW_TOOLCHAIN TRUE)
    endif()

    if(NOT WIBO_HAVE_MINGW_TOOLCHAIN)
        message(WARNING "MinGW toolchain not found; skipping fixture tests")
    else()
        set(WIBO_TEST_BIN_DIR ${CMAKE_CURRENT_BINARY_DIR}/test)
        file(MAKE_DIRECTORY ${WIBO_TEST_BIN_DIR})

        function(wibo_add_fixture_dll)
            set(options)
            set(oneValueArgs NAME DIRECTORY)
            set(multiValueArgs SOURCES COMPILE_OPTIONS DEPENDS BYPRODUCTS)
            cmake_parse_arguments(PARSE_ARGV 0 fixture "${options}" "${oneValueArgs}" "${multiValueArgs}")

            set(fixture_OUTPUT_DIRECTORY ${WIBO_TEST_BIN_DIR})
            if(fixture_DIRECTORY)
                string(APPEND fixture_OUTPUT_DIRECTORY /${fixture_DIRECTORY})
            endif()
            set(fixture_OUTPUT ${fixture_OUTPUT_DIRECTORY}/${fixture_NAME}.dll)
            add_custom_command(
                OUTPUT ${fixture_OUTPUT}
                BYPRODUCTS ${fixture_BYPRODUCTS}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${fixture_OUTPUT_DIRECTORY}
                COMMAND ${WIBO_MINGW_CC} -Wall -Wextra -O2 -shared
                        -I${CMAKE_CURRENT_SOURCE_DIR}/test
                        -o ${fixture_OUTPUT}
                        ${fixture_SOURCES}
                        ${fixture_COMPILE_OPTIONS}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                DEPENDS ${fixture_SOURCES} ${fixture_DEPENDS})
            list(APPEND WIBO_TEST_FIXTURES ${fixture_OUTPUT})
            set(WIBO_TEST_FIXTURES ${WIBO_TEST_FIXTURES} PARENT_SCOPE)
        endfunction(wibo_add_fixture_dll)

        function(wibo_add_fixture_bin)
            set(options WILL_FAIL NO_TEST)
            set(oneValueArgs NAME)
            set(multiValueArgs SOURCES COMPILE_OPTIONS DEPENDS)
            cmake_parse_arguments(PARSE_ARGV 0 fixture "${options}" "${oneValueArgs}" "${multiValueArgs}")

            set(fixture_OUTPUT ${WIBO_TEST_BIN_DIR}/${fixture_NAME}.exe)
            add_custom_command(
                OUTPUT ${fixture_OUTPUT}
                COMMAND ${WIBO_MINGW_CC} -Wall -Wextra -O2
                        -I${CMAKE_CURRENT_SOURCE_DIR}/test
                        -o ${fixture_OUTPUT}
                        ${fixture_SOURCES}
                        ${fixture_COMPILE_OPTIONS}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                DEPENDS ${fixture_SOURCES} ${fixture_DEPENDS})
            list(APPEND WIBO_TEST_FIXTURES ${fixture_OUTPUT})
            set(WIBO_TEST_FIXTURES ${WIBO_TEST_FIXTURES} PARENT_SCOPE)

            if(NOT fixture_NO_TEST)
                add_test(NAME wibo.${fixture_NAME}
                    COMMAND $<TARGET_FILE:wibo> ${fixture_OUTPUT})
                set_tests_properties(wibo.${fixture_NAME} PROPERTIES
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test
                    DEPENDS wibo.build_fixtures)
                if(fixture_WILL_FAIL)
                    set_tests_properties(wibo.${fixture_NAME} PROPERTIES WILL_FAIL TRUE)
                endif()
            endif()
        endfunction(wibo_add_fixture_bin)

        # Define fixture tests
        wibo_add_fixture_bin(NAME test_exit_process SOURCES test/test_exit_process.c
            COMPILE_OPTIONS -nostdlib -Wl,--entry,mainCRTStartup -lkernel32)
        wibo_add_fixture_bin(NAME test_c_drive_mapping SOURCES test/test_c_drive_mapping.c)
        set_tests_properties(wibo.test_c_drive_mapping PROPERTIES
            ENVIRONMENT "WIBO_C_DRIVE=${CMAKE_CURRENT_SOURCE_DIR}/test")
        if(APPLE)
            wibo_add_fixture_bin(NAME test_missing_import_exit SOURCES test/test_missing_import_exit.c WILL_FAIL
                COMPILE_OPTIONS -nostdlib -Wl,--entry,mainCRTStartup -lkernel32)
            set_tests_properties(wibo.test_missing_import_exit PROPERTIES TIMEOUT 5)
        endif()
        # A synchronous guest fault can strand Rosetta in an unkillable UEs
        # state on macOS. Keep these destructive teardown checks on native
        # Linux hosts; ordinary Darwin validation must never poison later runs.
        if(NOT APPLE)
            wibo_add_fixture_bin(NAME test_fault_teardown SOURCES test/test_fault_teardown.c WILL_FAIL
                COMPILE_OPTIONS -nostdlib -Wl,--entry,mainCRTStartup)
            wibo_add_fixture_bin(NAME test_fault_teardown_threads SOURCES test/test_fault_teardown_threads.c WILL_FAIL
                COMPILE_OPTIONS -nostdlib -Wl,--entry,mainCRTStartup -lkernel32)
        endif()
        if(WIBO_GUEST_64)
            wibo_add_fixture_bin(NAME test_software_exception_dispatch_public
                SOURCES test/test_software_exception_dispatch_public.cpp test/test_software_exception_dispatch_x64.S
                COMPILE_OPTIONS -std=c++20 -fno-exceptions -fno-rtti
                DEPENDS dll/context_x64_offsets.h)
            set_tests_properties(wibo.test_software_exception_dispatch_public PROPERTIES TIMEOUT 15)
            wibo_add_fixture_bin(NAME test_function_table SOURCES test/test_function_table.c)
            wibo_add_fixture_bin(NAME test_function_table_callback SOURCES test/test_function_table_callback.c)
            set_tests_properties(wibo.test_function_table_callback PROPERTIES TIMEOUT 15)
            wibo_add_fixture_bin(NAME test_function_table_callback_threads SOURCES test/test_function_table_callback_threads.c)
            set_tests_properties(wibo.test_function_table_callback_threads PROPERTIES TIMEOUT 20)
            wibo_add_fixture_bin(NAME test_virtual_unwind SOURCES test/test_virtual_unwind.c)
            wibo_add_fixture_bin(NAME test_context_capture SOURCES test/test_context_capture.c test/test_context_capture_x64.S
                COMPILE_OPTIONS -DWIBO_CONTEXT_CAPTURE_ONLY=1)
            set_tests_properties(wibo.test_context_capture PROPERTIES TIMEOUT 15)
            add_executable(test_context_capture_helpers
                test/test_context_capture.c test/test_context_capture_x64.S dll/context_x64.S)
            set_source_files_properties(test/test_context_capture.c PROPERTIES LANGUAGE CXX COMPILE_FLAGS "-x c++")
            target_compile_definitions(test_context_capture_helpers PRIVATE WIBO_GUEST_64=1 WIBO_CONTEXT_HOST_FIXTURE=1)
            target_include_directories(test_context_capture_helpers PRIVATE src dll)
            add_test(NAME wibo.test_context_capture_helpers COMMAND test_context_capture_helpers)
            set_tests_properties(wibo.test_context_capture_helpers PROPERTIES TIMEOUT 15)
            add_executable(test_software_exception_capture_helpers
                dll/software_exception_capture.cpp dll/software_exception_capture_x64.S
                dll/software_exception_prepare.cpp dll/software_exception_callback_x64.S
                test/test_software_exception_capture.cpp test/test_software_exception_capture_x64.S)
            target_compile_features(test_software_exception_capture_helpers PRIVATE cxx_std_20)
            target_compile_options(test_software_exception_capture_helpers PRIVATE -fno-exceptions -fno-rtti)
            target_compile_definitions(test_software_exception_capture_helpers PRIVATE
                WIBO_GUEST_64=1 WIBO_SOFTWARE_CAPTURE_HOST_FIXTURE=1)
            target_include_directories(test_software_exception_capture_helpers PRIVATE src dll test)
            add_test(NAME wibo.test_software_exception_capture_helpers COMMAND test_software_exception_capture_helpers)
            set_tests_properties(wibo.test_software_exception_capture_helpers PROPERTIES TIMEOUT 15)
            add_executable(test_software_exception_decision_helpers
                dll/software_exception_decision.cpp dll/kernel32/vectored_exception.cpp
                dll/software_exception_capture.cpp dll/software_exception_capture_x64.S dll/context_x64.S
                dll/software_exception_prepare.cpp dll/software_exception_callback_x64.S
                test/test_software_exception_decision.cpp)
            target_compile_features(test_software_exception_decision_helpers PRIVATE cxx_std_20)
            target_compile_options(test_software_exception_decision_helpers PRIVATE -fno-exceptions -fno-rtti)
            target_compile_definitions(test_software_exception_decision_helpers PRIVATE WIBO_GUEST_64=1)
            target_include_directories(test_software_exception_decision_helpers PRIVATE src dll test)
            add_test(NAME wibo.test_software_exception_decision_helpers COMMAND test_software_exception_decision_helpers)
            set_tests_properties(wibo.test_software_exception_decision_helpers PROPERTIES TIMEOUT 15)
            function(wibo_add_software_dispatch_helpers target)
                add_executable(${target}
                    dll/software_exception_dispatch.cpp dll/software_exception_dispatch_x64.S
                    dll/software_exception_decision.cpp dll/kernel32/vectored_exception.cpp
                    dll/software_exception_capture.cpp dll/software_exception_capture_x64.S dll/context_x64.S
                    dll/software_exception_prepare.cpp dll/software_exception_callback_x64.S
                    test/test_software_exception_dispatch.cpp test/test_software_exception_dispatch_x64.S)
                target_compile_features(${target} PRIVATE cxx_std_20)
                target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
                target_compile_definitions(${target} PRIVATE WIBO_GUEST_64=1)
                target_include_directories(${target} PRIVATE src dll test)
                add_test(NAME wibo.${target} COMMAND ${target})
                set_tests_properties(wibo.${target} PROPERTIES TIMEOUT 15)
            endfunction()
            wibo_add_software_dispatch_helpers(test_software_exception_dispatch_helpers)
            if(APPLE)
                wibo_add_software_dispatch_helpers(test_software_exception_dispatch_teb)
                target_sources(test_software_exception_dispatch_teb PRIVATE src/setup_darwin.cpp)
                target_compile_definitions(test_software_exception_dispatch_teb PRIVATE WIBO_SOFTWARE_DISPATCH_REAL_TEB=1)
            endif()
            set_tests_properties(wibo.test_virtual_unwind PROPERTIES TIMEOUT 15
                ENVIRONMENT "WIBO_TEST_DESCRIBED_MACHINE_FRAME=1")
            if(APPLE)
                add_executable(test_darwin_teb_context test/test_darwin_teb_context.cpp src/setup_darwin.cpp)
                target_compile_definitions(test_darwin_teb_context PRIVATE WIBO_GUEST_64=1)
                target_compile_features(test_darwin_teb_context PRIVATE cxx_std_20)
                target_include_directories(test_darwin_teb_context PRIVATE src)
                add_test(NAME wibo.test_darwin_teb_context COMMAND test_darwin_teb_context)
            endif()
            wibo_add_fixture_bin(NAME test_win64_peb_heap SOURCES test/test_win64_peb_heap.c
                COMPILE_OPTIONS -nostdlib -Wl,--entry,mainCRTStartup -lkernel32)
            wibo_add_fixture_bin(NAME test_win64_wgetmainargs SOURCES test/test_win64_wgetmainargs.c
                COMPILE_OPTIONS -nostdlib -Wl,--entry,mainCRTStartup -lmsvcrt -lkernel32)
            wibo_add_fixture_bin(NAME test_win64_teb_import SOURCES test/test_win64_teb_import.c
                COMPILE_OPTIONS -nostdlib -Wl,--entry,mainCRTStartup -lkernel32)
            wibo_add_fixture_bin(NAME test_win64_image_headers SOURCES test/test_win64_image_headers.c
                COMPILE_OPTIONS -nostdlib -Wl,--entry,mainCRTStartup -lkernel32)
        endif()
        wibo_add_fixture_bin(NAME test_external_dll SOURCES test/test_external_dll.c)
        wibo_add_fixture_bin(NAME test_socket_byte_order SOURCES test/test_socket_byte_order.c COMPILE_OPTIONS -lws2_32)
        wibo_add_fixture_bin(NAME test_file_lock SOURCES test/test_file_lock.c)
        wibo_add_fixture_bin(NAME test_file_lock_legacy SOURCES test/test_file_lock_legacy.c)
        wibo_add_fixture_bin(NAME test_move_file SOURCES test/test_move_file.c)
        wibo_add_fixture_bin(NAME test_remove_directory SOURCES test/test_remove_directory.c)
        set_tests_properties(wibo.test_move_file PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        set_tests_properties(wibo.test_file_lock_legacy PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1" TIMEOUT 10)
        wibo_add_fixture_bin(NAME test_dll_directory SOURCES test/test_dll_directory.c)
        wibo_add_fixture_bin(NAME test_dll_attach_failure SOURCES test/test_dll_attach_failure.c)
        wibo_add_fixture_bin(NAME test_thread_notifications SOURCES test/test_thread_notifications.c)
        wibo_add_fixture_bin(NAME test_thread_release SOURCES test/test_thread_release.c)
        wibo_add_fixture_bin(NAME test_thread_notifications_stress SOURCES test/test_thread_notifications_stress.c)
        wibo_add_fixture_bin(NAME test_bcrypt SOURCES test/test_bcrypt.c COMPILE_OPTIONS -lbcrypt)
        wibo_add_fixture_bin(NAME test_dbghelp SOURCES test/test_dbghelp.c COMPILE_OPTIONS -ldbghelp)
        wibo_add_fixture_bin(NAME test_user32 SOURCES test/test_user32.c COMPILE_OPTIONS -luser32)
        wibo_add_fixture_bin(NAME test_window_message SOURCES test/test_window_message.c COMPILE_OPTIONS -luser32)
        wibo_add_fixture_bin(NAME test_command_line_to_argv SOURCES test/test_command_line_to_argv.c COMPILE_OPTIONS -lshell32)
        wibo_add_fixture_bin(NAME test_special_folder SOURCES test/test_special_folder.c COMPILE_OPTIONS -lshell32 -lole32)
        set_tests_properties(wibo.test_special_folder PROPERTIES
            ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_PROVIDER=1")
        foreach(fault truncated embedded-zero)
            add_test(NAME wibo.test_known_folder_${fault}
                COMMAND $<TARGET_FILE:wibo> ${CMAKE_CURRENT_BINARY_DIR}/test/test_special_folder.exe)
            set_tests_properties(wibo.test_known_folder_${fault} PROPERTIES
                ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_FOLDER_RESPONSE=${fault}")
        endforeach()
        wibo_add_fixture_bin(NAME test_shlwapi SOURCES test/test_shlwapi.c COMPILE_OPTIONS -lshlwapi)
        wibo_add_fixture_bin(NAME test_path_canonicalize SOURCES test/test_path_canonicalize.c COMPILE_OPTIONS -lshlwapi)
        set_tests_properties(wibo.test_path_canonicalize PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_address_resolution SOURCES test/test_address_resolution.c COMPILE_OPTIONS -lws2_32)
        set_tests_properties(wibo.test_address_resolution PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_socket_options SOURCES test/test_socket_options.c COMPILE_OPTIONS -lws2_32)
        set_tests_properties(wibo.test_socket_options PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_socket_select SOURCES test/test_socket_select.c COMPILE_OPTIONS -lws2_32)
        set_tests_properties(wibo.test_socket_select PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_socket_receive SOURCES test/test_socket_receive.c COMPILE_OPTIONS -lws2_32)
        set_tests_properties(wibo.test_socket_receive PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_address_text SOURCES test/test_address_text.c COMPILE_OPTIONS -lws2_32)
        if(APPLE)
            wibo_add_fixture_bin(NAME test_directory_changes SOURCES test/test_directory_changes.c)
            wibo_add_fixture_bin(NAME test_thread_suspend SOURCES test/test_thread_suspend.c)
            if(WIBO_GUEST_ARCH STREQUAL "x86_64")
                wibo_add_fixture_bin(NAME test_thread_context SOURCES test/test_thread_context.c)
            endif()
        endif()
        wibo_add_fixture_bin(NAME test_address_parse SOURCES test/test_address_parse.c COMPILE_OPTIONS -lws2_32)
        set_tests_properties(wibo.test_address_text PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_socket_listen SOURCES test/test_socket_listen.c COMPILE_OPTIONS -lws2_32)
        set_tests_properties(wibo.test_socket_listen PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_socket_accept SOURCES test/test_socket_accept.c COMPILE_OPTIONS -lws2_32)
        wibo_add_fixture_bin(NAME test_socket_membership SOURCES test/test_socket_membership.c COMPILE_OPTIONS -lws2_32)
        set_tests_properties(wibo.test_socket_membership PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        set_tests_properties(wibo.test_socket_accept PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_socket_error SOURCES test/test_socket_error.c COMPILE_OPTIONS -lws2_32)
        wibo_add_fixture_bin(NAME test_socket_events SOURCES test/test_socket_events.c COMPILE_OPTIONS -lws2_32)
        wibo_add_fixture_bin(NAME test_socket_connect SOURCES test/test_socket_connect.c COMPILE_OPTIONS -lws2_32)
        set_tests_properties(wibo.test_socket_connect PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_socket_overlapped SOURCES test/test_socket_overlapped.c COMPILE_OPTIONS -lws2_32)
        wibo_add_fixture_bin(NAME test_socket_shutdown SOURCES test/test_socket_shutdown.c COMPILE_OPTIONS -lws2_32)
        wibo_add_fixture_bin(NAME test_socket_cancel SOURCES test/test_socket_cancel.c COMPILE_OPTIONS -lws2_32)
        wibo_add_fixture_bin(NAME test_socket_peer SOURCES test/test_socket_peer.c COMPILE_OPTIONS -lws2_32)
        set_tests_properties(wibo.test_socket_peer PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        set_tests_properties(wibo.test_socket_cancel PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        set_tests_properties(wibo.test_socket_overlapped PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_accept_ex SOURCES test/test_accept_ex.c COMPILE_OPTIONS -lws2_32)
        set_tests_properties(wibo.test_accept_ex PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_socket_completion SOURCES test/test_socket_completion.c COMPILE_OPTIONS -lws2_32)
        wibo_add_fixture_bin(NAME test_socket_handle_flags SOURCES test/test_socket_handle_flags.c COMPILE_OPTIONS -lws2_32)
        wibo_add_fixture_bin(NAME test_socket_lifetime SOURCES test/test_socket_lifetime.c COMPILE_OPTIONS -lws2_32)
        wibo_add_fixture_bin(NAME test_service_lookup SOURCES test/test_service_lookup.c COMPILE_OPTIONS -lws2_32)
        set_tests_properties(wibo.test_service_lookup PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_ws2 SOURCES test/test_ws2.c COMPILE_OPTIONS -lws2_32)
        wibo_add_fixture_bin(NAME test_winmm SOURCES test/test_winmm.c COMPILE_OPTIONS -lwinmm)
        wibo_add_fixture_bin(NAME test_resources SOURCES test/test_resources.c ${WIBO_TEST_BIN_DIR}/test_resources_res.o COMPILE_OPTIONS -lversion)
        wibo_add_fixture_bin(NAME test_threading SOURCES test/test_threading.c)
        wibo_add_fixture_bin(NAME test_thread_yield SOURCES test/test_thread_yield.c)
        wibo_add_fixture_bin(NAME test_thread_priority_boost SOURCES test/test_thread_priority_boost.c)
        wibo_add_fixture_bin(NAME test_thread_ideal_processor SOURCES test/test_thread_ideal_processor.c)
        wibo_add_fixture_bin(NAME test_raise_exception SOURCES test/test_raise_exception.c)
        wibo_add_fixture_bin(NAME test_slist SOURCES test/test_slist.c)
        set_tests_properties(wibo.test_slist PROPERTIES TIMEOUT 15)
        wibo_add_fixture_bin(NAME test_vectored_exception_registration SOURCES test/test_vectored_exception_registration.c)
        set_tests_properties(wibo.test_vectored_exception_registration PROPERTIES TIMEOUT 15)
        wibo_add_fixture_bin(NAME test_pc_to_file_header SOURCES test/test_pc_to_file_header.c COMPILE_OPTIONS -lkernel32)
        wibo_add_fixture_bin(NAME test_wer_set_flags SOURCES test/test_wer_set_flags.c COMPILE_OPTIONS -lkernel32)
        wibo_add_fixture_bin(NAME test_runtime_exception_registration SOURCES test/test_runtime_exception_registration.c)
        set_tests_properties(wibo.test_runtime_exception_registration PROPERTIES
            ENVIRONMENT "WIBO_EXPECT_WER_UNSUPPORTED=1")
        wibo_add_fixture_bin(NAME test_open_thread_token SOURCES test/test_open_thread_token.c COMPILE_OPTIONS -ladvapi32)
        set_tests_properties(wibo.test_open_thread_token PROPERTIES TIMEOUT 15)
        wibo_add_fixture_bin(NAME test_process_job_membership SOURCES test/test_process_job_membership.c)
        set_tests_properties(wibo.test_process_job_membership PROPERTIES ENVIRONMENT "WIBO_EXPECT_NO_JOB=1")
        wibo_add_fixture_bin(NAME test_memory_resource SOURCES test/test_memory_resource.c)
        set_tests_properties(wibo.test_memory_resource PROPERTIES TIMEOUT 15
            ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_PROVIDER=1")
        foreach(fault failed truncated trailing bad-state)
            add_test(NAME wibo.test_memory_resource_${fault}
                COMMAND $<TARGET_FILE:wibo> ${CMAKE_CURRENT_BINARY_DIR}/test/test_memory_resource.exe ${fault})
            set_tests_properties(wibo.test_memory_resource_${fault} PROPERTIES TIMEOUT 15
                ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_PROVIDER=1")
        endforeach()
        add_test(NAME wibo.test_memory_resource_unavailable
            COMMAND $<TARGET_FILE:wibo> ${CMAKE_CURRENT_BINARY_DIR}/test/test_memory_resource.exe unavailable)
        set_tests_properties(wibo.test_memory_resource_unavailable PROPERTIES TIMEOUT 15
            ENVIRONMENT "WIBO_SYSTEM_PROVIDER=")
        wibo_add_fixture_bin(NAME test_setup_devices SOURCES test/test_setup_devices.c COMPILE_OPTIONS -lsetupapi)
        set_tests_properties(wibo.test_setup_devices PROPERTIES TIMEOUT 20
            ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_PROVIDER=1")
        wibo_add_fixture_bin(NAME test_module_image_handle SOURCES test/test_module_image_handle.c COMPILE_OPTIONS -lpsapi)
        wibo_add_fixture_bin(NAME test_psapi SOURCES test/test_psapi.c COMPILE_OPTIONS -lpsapi)
        wibo_add_fixture_bin(NAME test_get_thread_id SOURCES test/test_get_thread_id.c)
        wibo_add_fixture_bin(NAME test_temppath SOURCES test/test_temppath.c)
        wibo_add_fixture_bin(NAME test_gettempfilename SOURCES test/test_gettempfilename.c)
        wibo_add_fixture_bin(NAME test_getprocaddress SOURCES test/test_getprocaddress.c)
        wibo_add_fixture_bin(NAME test_api_set SOURCES test/test_api_set.c)
        set_tests_properties(wibo.test_api_set PROPERTIES
            ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py")
        foreach(fault failed truncated trailing path embedded-zero contract-host)
            add_test(NAME wibo.test_api_set_${fault}
                COMMAND $<TARGET_FILE:wibo> ${CMAKE_CURRENT_BINARY_DIR}/test/test_api_set.exe)
            set_tests_properties(wibo.test_api_set_${fault} PROPERTIES
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_API_SET_RESPONSE=${fault}")
        endforeach()
        wibo_add_fixture_bin(NAME test_error_mode SOURCES test/test_error_mode.c)
        wibo_add_fixture_bin(NAME test_getmodulehandleex SOURCES test/test_getmodulehandleex.c)
        if(NOT WIBO_GUEST_64)
            # These fixtures contain x86-specific inline FS/TLS assembly.
            wibo_add_fixture_bin(NAME test_tls SOURCES test/test_tls.c)
            wibo_add_fixture_bin(NAME test_tls_reloc SOURCES test/test_tls_reloc.c)
        endif()
        wibo_add_fixture_bin(NAME test_fls SOURCES test/test_fls.c)
        wibo_add_fixture_bin(NAME test_fpu_state SOURCES test/test_fpu_state.c)
        wibo_add_fixture_bin(NAME test_handleapi SOURCES test/test_handleapi.c)
        wibo_add_fixture_bin(NAME test_process_snapshot SOURCES test/test_process_snapshot.c)
        wibo_add_fixture_bin(NAME test_toolhelp SOURCES test/test_toolhelp.c)
        wibo_add_fixture_bin(NAME test_findfile SOURCES test/test_findfile.c)
        wibo_add_fixture_bin(NAME test_getfileattributesex SOURCES test/test_getfileattributesex.c)
        wibo_add_fixture_bin(NAME test_locale SOURCES test/test_locale.c)
        wibo_add_fixture_bin(NAME test_lstrcpyn SOURCES test/test_lstrcpyn.c)
        wibo_add_fixture_bin(NAME test_global_atoms SOURCES test/test_global_atoms.c)
        wibo_add_fixture_bin(NAME test_system_random SOURCES test/test_system_random.c)
        wibo_add_fixture_bin(NAME test_trace_registration SOURCES test/test_trace_registration.c COMPILE_OPTIONS -ladvapi32)
        wibo_add_fixture_bin(NAME test_event_registration SOURCES test/test_event_registration.c COMPILE_OPTIONS -ladvapi32)
        wibo_add_fixture_bin(NAME test_lstring SOURCES test/test_lstring.c)
        wibo_add_fixture_bin(NAME test_reg_open_legacy SOURCES test/test_reg_open_legacy.c COMPILE_OPTIONS -ladvapi32)
        wibo_add_fixture_bin(NAME test_reg_values SOURCES test/test_reg_values.c COMPILE_OPTIONS -ladvapi32)
        wibo_add_fixture_bin(NAME test_reg_get_value SOURCES test/test_reg_get_value.c COMPILE_OPTIONS -ladvapi32)
        wibo_add_fixture_bin(NAME test_critical_section_owner SOURCES test/test_critical_section_owner.c)
        wibo_add_fixture_bin(NAME test_version_conditions SOURCES test/test_version_conditions.c)
        wibo_add_fixture_bin(NAME test_expand_environment SOURCES test/test_expand_environment.c)
        wibo_add_fixture_bin(NAME test_profile_read SOURCES test/test_profile_read.c)
        wibo_add_fixture_bin(NAME test_code_pointer SOURCES test/test_code_pointer.c)
        wibo_add_fixture_bin(NAME test_stack_query SOURCES test/test_stack_query.c)
        wibo_add_fixture_bin(NAME test_native_system_info SOURCES test/test_native_system_info.c)
        wibo_add_fixture_bin(NAME test_resource_release SOURCES test/test_resource_release.c ${WIBO_TEST_BIN_DIR}/test_resource_release_res.o)
        wibo_add_fixture_bin(NAME test_critical_section SOURCES test/test_critical_section.c)
        wibo_add_fixture_bin(NAME test_condition_srw SOURCES test/test_condition_srw.c)
        wibo_add_fixture_bin(NAME test_condition_variable SOURCES test/test_condition_variable.c)
        wibo_add_fixture_bin(NAME test_synchapi SOURCES test/test_synchapi.c)
        wibo_add_fixture_bin(NAME test_waitable_timer SOURCES test/test_waitable_timer.c)
        wibo_add_fixture_bin(NAME test_completion_port SOURCES test/test_completion_port.c)
        set_tests_properties(wibo.test_completion_port PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_work_queue SOURCES test/test_work_queue.c)
        wibo_add_fixture_bin(NAME test_open_thread SOURCES test/test_open_thread.c)
        wibo_add_fixture_bin(NAME test_alertable_wait SOURCES test/test_alertable_wait.c)
        wibo_add_fixture_bin(NAME test_wait_multiple_alertable SOURCES test/test_wait_multiple_alertable.c)
        set_tests_properties(wibo.test_wait_multiple_alertable PROPERTIES TIMEOUT 15)
        wibo_add_fixture_bin(NAME test_system_metrics SOURCES test/test_system_metrics.c COMPILE_OPTIONS -luser32)
        wibo_add_fixture_bin(NAME test_native_system_query SOURCES test/test_native_system_query.c)
        set_tests_properties(wibo.test_native_system_query PROPERTIES ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_PROVIDER=1")
        wibo_add_fixture_bin(NAME test_volume_query SOURCES test/test_volume_query.c)
        set_tests_properties(wibo.test_volume_query PROPERTIES ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_PROVIDER=1")
        wibo_add_fixture_bin(NAME test_native_file_set SOURCES test/test_native_file_set.c)
        wibo_add_fixture_bin(NAME test_console_redirect SOURCES test/test_console_redirect.c)
        wibo_add_fixture_bin(NAME test_console_attach SOURCES test/test_console_attach.c COMPILE_OPTIONS -mwindows)
        wibo_add_fixture_bin(NAME test_native_file_all SOURCES test/test_native_file_all.c)
        set_tests_properties(wibo.test_native_file_all PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        set_tests_properties(wibo.test_native_file_set PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_device_control SOURCES test/test_device_control.c)
        wibo_add_fixture_bin(NAME test_status_error SOURCES test/test_status_error.c)
        set_tests_properties(wibo.test_status_error PROPERTIES ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_PROVIDER=1")
        set_tests_properties(wibo.test_system_metrics PROPERTIES ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_PROVIDER=1")
        set_tests_properties(wibo.test_waitable_timer PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_wait_multiple_race SOURCES test/test_wait_multiple_race.c)
        wibo_add_fixture_bin(NAME test_processes SOURCES test/test_processes.c)
        wibo_add_fixture_bin(NAME test_process_current_directory SOURCES test/test_process_current_directory.c)
        set_tests_properties(wibo.test_process_current_directory PROPERTIES TIMEOUT 20)
        set_tests_properties(wibo.test_processes PROPERTIES TIMEOUT 25)
        if(NOT APPLE)
            # pidfds support non-child waits but waitid cannot retrieve their exit status.
            set_tests_properties(wibo.test_processes PROPERTIES
                ENVIRONMENT "WIBO_FIXTURE_EXTERNAL_STATUS_UNAVAILABLE=1")
        endif()
        wibo_add_fixture_bin(NAME test_process_attributes SOURCES test/test_process_attributes.c)
        wibo_add_fixture_bin(NAME test_heap SOURCES test/test_heap.c)
        wibo_add_fixture_bin(NAME test_heap_cross_thread SOURCES test/test_heap_cross_thread.c)
        wibo_add_fixture_bin(NAME test_actctx SOURCES test/test_actctx.c)
        wibo_add_fixture_bin(NAME test_overlapped_io SOURCES test/test_overlapped_io.c)
        set_tests_properties(wibo.test_overlapped_io PROPERTIES TIMEOUT 15)
        wibo_add_fixture_bin(NAME test_time SOURCES test/test_time.c)
        wibo_add_fixture_bin(NAME test_system_times SOURCES test/test_system_times.c)
        wibo_add_fixture_bin(NAME test_numa_highest_node SOURCES test/test_numa_highest_node.c NO_TEST)
        foreach(mode success failed truncated trailing unavailable)
            add_test(NAME wibo.test_numa_highest_node_${mode}
                COMMAND $<TARGET_FILE:wibo> ${CMAKE_CURRENT_BINARY_DIR}/test/test_numa_highest_node.exe)
            if(mode STREQUAL "unavailable")
                set_tests_properties(wibo.test_numa_highest_node_${mode} PROPERTIES
                    ENVIRONMENT "WIBO_SYSTEM_PROVIDER=;WIBO_FIXTURE_NUMA_RESPONSE=unavailable")
            else()
                set_tests_properties(wibo.test_numa_highest_node_${mode} PROPERTIES
                    ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_PROVIDER=1"
                    TIMEOUT 15)
                if(NOT mode STREQUAL "success")
                    set_property(TEST wibo.test_numa_highest_node_${mode} APPEND PROPERTY
                        ENVIRONMENT "WIBO_FIXTURE_NUMA_RESPONSE=${mode}")
                endif()
            endif()
        endforeach()
        wibo_add_fixture_bin(NAME test_provider_stream SOURCES test/test_provider_stream.c NO_TEST)
        foreach(mode fresh concurrent exit truncated oversized timeout)
            add_test(NAME wibo.test_provider_stream_${mode}
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/check_provider_stream.py
                    $<TARGET_FILE:wibo> ${CMAKE_CURRENT_BINARY_DIR}/test/test_provider_stream.exe
                    ${CMAKE_CURRENT_SOURCE_DIR}/test/provider_stream_fixture.py ${mode})
            set_tests_properties(wibo.test_provider_stream_${mode} PROPERTIES TIMEOUT 30)
        endforeach()
        wibo_add_fixture_bin(NAME test_timezone_conversion SOURCES test/test_timezone_conversion.c)
        set_tests_properties(wibo.test_timezone_conversion PROPERTIES ENVIRONMENT "WIBO_FIXTURE_NATIVE_ZONE=1")
        add_test(NAME wibo.test_timezone_provider
            COMMAND $<TARGET_FILE:wibo> ${CMAKE_CURRENT_BINARY_DIR}/test/test_timezone_conversion.exe)
        set_tests_properties(wibo.test_timezone_provider PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_PROVIDER=1")
        foreach(fault failed truncated trailing invalid-state)
            add_test(NAME wibo.test_timezone_provider_${fault}
                COMMAND $<TARGET_FILE:wibo> ${CMAKE_CURRENT_BINARY_DIR}/test/test_timezone_conversion.exe)
            set_tests_properties(wibo.test_timezone_provider_${fault} PROPERTIES
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_ZONE_RESPONSE=${fault}")
        endforeach()
        wibo_add_fixture_bin(NAME test_dynamic_timezone SOURCES test/test_dynamic_timezone.c)
        set_tests_properties(wibo.test_dynamic_timezone PROPERTIES
            ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_PROVIDER=1")
        foreach(fault failed truncated trailing invalid-state)
            add_test(NAME wibo.test_dynamic_timezone_${fault}
                COMMAND $<TARGET_FILE:wibo> ${CMAKE_CURRENT_BINARY_DIR}/test/test_dynamic_timezone.exe)
            set_tests_properties(wibo.test_dynamic_timezone_${fault} PROPERTIES
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_ZONE_RESPONSE=${fault}")
        endforeach()
        wibo_add_fixture_bin(NAME test_logical_processor_information SOURCES test/test_logical_processor_information.c)
        wibo_add_fixture_bin(NAME test_ntdll_time SOURCES test/test_ntdll_time.c)
        wibo_add_fixture_bin(NAME test_virtualalloc SOURCES test/test_virtualalloc.c)
        wibo_add_fixture_bin(NAME test_virtual_recommit SOURCES test/test_virtual_recommit.c)
        wibo_add_fixture_bin(NAME test_virtual_release SOURCES test/test_virtual_release.c)
        wibo_add_fixture_bin(NAME test_virtualquery SOURCES test/test_virtualquery.c)
        wibo_add_fixture_bin(NAME test_createfilemapping SOURCES test/test_createfilemapping.c)
        wibo_add_fixture_bin(NAME test_image_mapping SOURCES test/test_image_mapping.c)
        set_tests_properties(wibo.test_image_mapping PROPERTIES TIMEOUT 15 ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_descriptor_control SOURCES test/test_descriptor_control.c COMPILE_OPTIONS -ladvapi32)
        wibo_add_fixture_bin(NAME test_acl_information SOURCES test/test_acl_information.c)
        set_tests_properties(wibo.test_acl_information PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_ip_address_table SOURCES test/test_ip_address_table.c COMPILE_OPTIONS -liphlpapi -lws2_32)
        set_tests_properties(wibo.test_ip_address_table PROPERTIES ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_PROVIDER=1")
        wibo_add_fixture_bin(NAME test_format_message SOURCES test/test_format_message.c)
        set_tests_properties(wibo.test_format_message PROPERTIES ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_PROVIDER=1")
        wibo_add_fixture_bin(NAME test_file_security SOURCES test/test_file_security.c)
        wibo_add_fixture_bin(NAME test_load_cursor SOURCES test/test_load_cursor.c COMPILE_OPTIONS -luser32)
        wibo_add_fixture_bin(NAME test_memory_status SOURCES test/test_memory_status.c)
        wibo_add_fixture_bin(NAME test_window_class SOURCES test/test_window_class.c COMPILE_OPTIONS -luser32)
        wibo_add_fixture_bin(NAME test_provider_account SOURCES test/test_provider_account.c)
        wibo_add_fixture_bin(NAME test_environment_defaults SOURCES test/test_environment_defaults.c)
        set_tests_properties(wibo.test_environment_defaults PROPERTIES ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_PROVIDER=1;LOCALAPPDATA=C:\\Fixture\\Override")
        foreach(fault truncated trailing)
            add_test(NAME wibo.test_environment_defaults_${fault}
                COMMAND $<TARGET_FILE:wibo> ${WIBO_TEST_BIN_DIR}/test_environment_defaults.exe unavailable)
            set_tests_properties(wibo.test_environment_defaults_${fault} PROPERTIES
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test DEPENDS wibo.build_fixtures
                ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_ENV_RESPONSE=${fault}")
        endforeach()
        wibo_add_fixture_bin(NAME test_best_route SOURCES test/test_best_route.c COMPILE_OPTIONS -liphlpapi)
        set_tests_properties(wibo.test_best_route PROPERTIES ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_RUNTIME=1;WIBO_FIXTURE_PROVIDER=1")
        wibo_add_fixture_bin(NAME test_adapter_addresses SOURCES test/test_adapter_addresses.c COMPILE_OPTIONS -liphlpapi)
        set_tests_properties(wibo.test_adapter_addresses PROPERTIES ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_RUNTIME=1;WIBO_FIXTURE_PROVIDER=1")
        wibo_add_fixture_bin(NAME test_network_connectivity SOURCES test/test_network_connectivity.c COMPILE_OPTIONS -lole32 -luuid)
        set_tests_properties(wibo.test_network_connectivity PROPERTIES ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_PROVIDER=1")
        wibo_add_fixture_bin(NAME test_management SOURCES test/test_management.c COMPILE_OPTIONS -lole32 -loleaut32 -lwbemuuid)
        wibo_add_fixture_bin(NAME test_reg_enum_values SOURCES test/test_reg_enum_values.c)
        wibo_add_fixture_bin(NAME test_provider_registry SOURCES test/test_provider_registry.c)
        set_tests_properties(wibo.test_management wibo.test_provider_registry wibo.test_provider_account wibo.test_file_security wibo.test_load_cursor wibo.test_memory_status PROPERTIES
            ENVIRONMENT "WIBO_SYSTEM_PROVIDER=${CMAKE_CURRENT_SOURCE_DIR}/test/provider_fixture.py;WIBO_FIXTURE_PROVIDER=1"
            TIMEOUT 15)
        set_property(TEST wibo.test_memory_status APPEND PROPERTY ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_bstr SOURCES test/test_bstr.c COMPILE_OPTIONS -loleaut32)
        wibo_add_fixture_bin(NAME test_clsids SOURCES test/test_clsids.c COMPILE_OPTIONS -lole32)
        wibo_add_fixture_bin(NAME test_ole_initialize SOURCES test/test_ole_initialize.c COMPILE_OPTIONS -lole32)
        wibo_add_fixture_bin(NAME test_runtime_initialize SOURCES test/test_runtime_initialize.c COMPILE_OPTIONS -lole32)
        set_tests_properties(wibo.test_runtime_initialize PROPERTIES TIMEOUT 15)
        wibo_add_fixture_bin(NAME test_com_context_token SOURCES test/test_com_context_token.c COMPILE_OPTIONS -lole32 -luuid)
        set_tests_properties(wibo.test_com_context_token PROPERTIES TIMEOUT 15)
        wibo_add_fixture_bin(NAME test_rtl SOURCES test/test_rtl.c)
        wibo_add_fixture_bin(NAME test_rtl_bitmap SOURCES test/test_rtl_bitmap.c)
        wibo_add_fixture_bin(NAME test_ntquery SOURCES test/test_ntquery.c)
        wibo_add_fixture_bin(NAME test_ntqueryfile SOURCES test/test_ntqueryfile.c)
        wibo_add_fixture_bin(NAME test_ntquerydirectory SOURCES test/test_ntquerydirectory.c)
        set_tests_properties(wibo.test_ntquerydirectory PROPERTIES ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_ntreadfile SOURCES test/test_ntreadfile.c)
        wibo_add_fixture_bin(NAME test_ntwritefile SOURCES test/test_ntwritefile.c)
        wibo_add_fixture_bin(NAME test_pipe_io SOURCES test/test_pipe_io.c)
        wibo_add_fixture_bin(NAME test_namedpipe SOURCES test/test_namedpipe.c)
        wibo_add_fixture_bin(NAME test_sysdir SOURCES test/test_sysdir.c)
        wibo_add_fixture_bin(NAME test_stringapiset SOURCES test/test_stringapiset.c)
        wibo_add_fixture_bin(NAME test_srw_lock SOURCES test/test_srw_lock.c)
        wibo_add_fixture_bin(NAME test_init_once SOURCES test/test_init_once.c)
        wibo_add_fixture_bin(NAME test_wait_on_address SOURCES test/test_wait_on_address.c COMPILE_OPTIONS -lsynchronization)
        wibo_add_fixture_bin(NAME test_env_strings SOURCES test/test_env_strings.c)
        wibo_add_fixture_bin(NAME test_load_library_ex SOURCES test/test_load_library_ex.c)
        set_tests_properties(wibo.test_load_library_ex PROPERTIES TIMEOUT 15
            ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_system_library_load SOURCES test/test_system_library_load.c)
        set_tests_properties(wibo.test_system_library_load PROPERTIES TIMEOUT 15
            ENVIRONMENT "WIBO_C_DRIVE=${WIBO_TEST_BIN_DIR}/system-drive;WIBO_SYSTEM_FIXTURE=1")
        wibo_add_fixture_bin(NAME test_module_lifetime SOURCES test/test_module_lifetime.c)
        set_tests_properties(wibo.test_module_lifetime PROPERTIES TIMEOUT 15
            ENVIRONMENT "WIBO_FIXTURE_RUNTIME=1")
        wibo_add_fixture_bin(NAME test_library_search_flags SOURCES test/test_library_search_flags.c)
        set_tests_properties(wibo.test_library_search_flags PROPERTIES TIMEOUT 15)

        # DLLs for fixture tests
        wibo_add_fixture_dll(NAME external_exports SOURCES test/external_exports.c)
        wibo_add_fixture_dll(NAME dll_attach_failure SOURCES test/dll_attach_failure.c)
        wibo_add_fixture_dll(NAME thread_notifications SOURCES test/thread_notifications.c)
        wibo_add_fixture_dll(NAME thread_release SOURCES test/thread_release.c)
        wibo_add_fixture_dll(NAME thread_notifications_stress SOURCES test/thread_notifications_stress.c)
        wibo_add_fixture_dll(NAME tls_reloc SOURCES test/tls_reloc_dll.c)
        wibo_add_fixture_dll(NAME restricted_loader_dependency SOURCES test/system_loader_dependency.c test/fixture_relocation_anchor.c
            BYPRODUCTS ${WIBO_TEST_BIN_DIR}/restricted_loader_dependency.a
            COMPILE_OPTIONS -nostdlib -Wl,--entry,0
                -Wl,--out-implib,${WIBO_TEST_BIN_DIR}/restricted_loader_dependency.a)
        wibo_add_fixture_dll(NAME restricted_loader_parent SOURCES test/system_loader_parent.c
            COMPILE_OPTIONS -nostdlib -Wl,--entry,0 ${WIBO_TEST_BIN_DIR}/restricted_loader_dependency.a
            DEPENDS ${WIBO_TEST_BIN_DIR}/restricted_loader_dependency.dll)
        wibo_add_fixture_dll(NAME system_loader_forwarder SOURCES test/system_loader_forwarder.def test/fixture_relocation_anchor.c
            BYPRODUCTS ${WIBO_TEST_BIN_DIR}/system_loader_forwarder.a
            COMPILE_OPTIONS -nostdlib -Wl,--entry,0
                -Wl,--out-implib,${WIBO_TEST_BIN_DIR}/system_loader_forwarder.a)
        wibo_add_fixture_dll(NAME restricted_loader_forward_parent SOURCES test/system_loader_parent.c
            COMPILE_OPTIONS -nostdlib -Wl,--entry,0 -DFIXTURE_DEPENDENCY_NAME=forward_value
                ${WIBO_TEST_BIN_DIR}/system_loader_forwarder.a
            DEPENDS ${WIBO_TEST_BIN_DIR}/system_loader_forwarder.dll)
        wibo_add_fixture_dll(NAME system_loader_dependency SOURCES test/system_loader_dependency.c test/fixture_relocation_anchor.c
            BYPRODUCTS ${WIBO_TEST_BIN_DIR}/system_loader_dependency.a
            COMPILE_OPTIONS -nostdlib -Wl,--entry,0 -DFIXTURE_DEPENDENCY_VALUE=99
                -Wl,--out-implib,${WIBO_TEST_BIN_DIR}/system_loader_dependency.a)
        foreach(_library module_lifetime_cycle_a module_lifetime_cycle_b module_lifetime_missing)
            add_custom_command(
                OUTPUT ${WIBO_TEST_BIN_DIR}/${_library}.a
                COMMAND ${WIBO_MINGW_DLLTOOL}
                    -d ${CMAKE_CURRENT_SOURCE_DIR}/test/${_library}.def
                    -l ${WIBO_TEST_BIN_DIR}/${_library}.a
                DEPENDS test/${_library}.def)
            list(APPEND WIBO_TEST_FIXTURES ${WIBO_TEST_BIN_DIR}/${_library}.a)
        endforeach()
        wibo_add_fixture_dll(NAME module_lifetime_cycle_a SOURCES test/module_lifetime_cycle_a.c
            COMPILE_OPTIONS -nostdlib -Wl,--entry,DllMain ${WIBO_TEST_BIN_DIR}/module_lifetime_cycle_b.a -lkernel32
            DEPENDS ${WIBO_TEST_BIN_DIR}/module_lifetime_cycle_b.a)
        wibo_add_fixture_dll(NAME module_lifetime_cycle_b SOURCES test/module_lifetime_cycle_b.c
            COMPILE_OPTIONS -nostdlib -Wl,--entry,DllMain ${WIBO_TEST_BIN_DIR}/module_lifetime_cycle_a.a -lkernel32
            DEPENDS ${WIBO_TEST_BIN_DIR}/module_lifetime_cycle_a.a)
        wibo_add_fixture_dll(NAME module_lifetime_failure SOURCES test/module_lifetime_failure.c
            COMPILE_OPTIONS -nostdlib -Wl,--entry,0 ${WIBO_TEST_BIN_DIR}/restricted_loader_dependency.a
                ${WIBO_TEST_BIN_DIR}/module_lifetime_missing.a
            DEPENDS ${WIBO_TEST_BIN_DIR}/restricted_loader_dependency.a ${WIBO_TEST_BIN_DIR}/module_lifetime_missing.a)
        wibo_add_fixture_dll(NAME system_loader_failure SOURCES test/module_lifetime_failure.c
            COMPILE_OPTIONS -nostdlib -Wl,--entry,0 ${WIBO_TEST_BIN_DIR}/system_loader_dependency.a
                ${WIBO_TEST_BIN_DIR}/module_lifetime_missing.a
            DEPENDS ${WIBO_TEST_BIN_DIR}/system_loader_dependency.a ${WIBO_TEST_BIN_DIR}/module_lifetime_missing.a)
        foreach(_library module_forward_cycle_a module_forward_cycle_b)
            wibo_add_fixture_dll(NAME ${_library} SOURCES test/${_library}.def test/fixture_relocation_anchor.c
                COMPILE_OPTIONS -nostdlib -Wl,--entry,0)
        endforeach()
        wibo_add_fixture_dll(NAME module_forward_failure SOURCES test/module_forward_failure.def test/fixture_relocation_anchor.c
            COMPILE_OPTIONS -nostdlib -Wl,--entry,0)
        wibo_add_fixture_dll(NAME module_detach_reference SOURCES test/module_detach_reference.c
            COMPILE_OPTIONS -nostdlib -Wl,--entry,DllMain -lkernel32)
        add_custom_command(
            OUTPUT ${WIBO_TEST_BIN_DIR}/failed-module/version.dll
            COMMAND ${CMAKE_COMMAND} -E make_directory ${WIBO_TEST_BIN_DIR}/failed-module
            COMMAND ${WIBO_MINGW_CC} -Wall -Wextra -O2 -shared
                ${CMAKE_CURRENT_SOURCE_DIR}/test/dll_attach_failure.c
                -o ${WIBO_TEST_BIN_DIR}/failed-module/version.dll
            DEPENDS test/dll_attach_failure.c)
        list(APPEND WIBO_TEST_FIXTURES ${WIBO_TEST_BIN_DIR}/failed-module/version.dll)
        set(_system_fixture_dir ${WIBO_TEST_BIN_DIR}/system-drive/Windows/System32)
        add_custom_command(
            OUTPUT ${_system_fixture_dir}/system_loader_dependency.dll ${_system_fixture_dir}/system_loader_parent.dll
            BYPRODUCTS ${_system_fixture_dir}/system_loader_dependency.a
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_system_fixture_dir}
            COMMAND ${WIBO_MINGW_CC} -Wall -Wextra -O2 -shared -nostdlib -Wl,--entry,0
                -o ${_system_fixture_dir}/system_loader_dependency.dll
                -Wl,--out-implib,${_system_fixture_dir}/system_loader_dependency.a
                ${CMAKE_CURRENT_SOURCE_DIR}/test/system_loader_dependency.c
                ${CMAKE_CURRENT_SOURCE_DIR}/test/fixture_relocation_anchor.c
            COMMAND ${WIBO_MINGW_CC} -Wall -Wextra -O2 -shared -nostdlib -Wl,--entry,0
                -o ${_system_fixture_dir}/system_loader_parent.dll
                ${CMAKE_CURRENT_SOURCE_DIR}/test/system_loader_parent.c
                ${_system_fixture_dir}/system_loader_dependency.a
            DEPENDS test/system_loader_dependency.c test/system_loader_parent.c test/fixture_relocation_anchor.c)
        list(APPEND WIBO_TEST_FIXTURES
            ${_system_fixture_dir}/system_loader_dependency.dll ${_system_fixture_dir}/system_loader_parent.dll)

        function(wibo_add_search_value name directory value)
            wibo_add_fixture_dll(NAME ${name} DIRECTORY ${directory}
                SOURCES test/system_loader_dependency.c test/fixture_relocation_anchor.c
                BYPRODUCTS ${WIBO_TEST_BIN_DIR}/${directory}/${name}.a
                COMPILE_OPTIONS -nostdlib -Wl,--entry,0 -DFIXTURE_DEPENDENCY_VALUE=${value}
                    -Wl,--out-implib,${WIBO_TEST_BIN_DIR}/${directory}/${name}.a)
            set(WIBO_TEST_FIXTURES ${WIBO_TEST_FIXTURES} PARENT_SCOPE)
        endfunction()

        wibo_add_search_value(dep_priority search-mode/root 11)
        wibo_add_search_value(dep_priority . 22)
        wibo_add_search_value(dep_priority search-mode/current 33)
        wibo_add_search_value(dep_priority search-mode/user 44)
        wibo_add_search_value(dep_priority search-mode/environment 55)
        wibo_add_search_value(dep_fallback . 22)
        wibo_add_search_value(dep_fallback search-mode/current 33)
        wibo_add_search_value(dep_fallback search-mode/user 44)
        wibo_add_search_value(dep_fallback search-mode/environment 55)
        wibo_add_search_value(dep_excluded search-mode/current 33)
        wibo_add_search_value(dep_excluded search-mode/environment 55)
        wibo_add_search_value(dep_environment search-mode/environment 55)
        wibo_add_search_value(dep_inherited search-mode/root 11)
        wibo_add_search_value(dep_inherited . 22)
        wibo_add_search_value(dep_inherited search-mode/user 44)

        function(wibo_add_search_parent name directory dependency)
            wibo_add_fixture_dll(NAME ${name} DIRECTORY ${directory}
                SOURCES test/system_loader_parent.c test/fixture_relocation_anchor.c
                COMPILE_OPTIONS -nostdlib -Wl,--entry,0 -DFIXTURE_VALUE_ADJUSTMENT=0 ${dependency}
                DEPENDS ${dependency})
            set(WIBO_TEST_FIXTURES ${WIBO_TEST_FIXTURES} PARENT_SCOPE)
        endfunction()
        wibo_add_search_parent(flags_priority search-mode/root ${WIBO_TEST_BIN_DIR}/search-mode/root/dep_priority.a)
        wibo_add_search_parent(flags_fallback search-mode/root ${WIBO_TEST_BIN_DIR}/./dep_fallback.a)
        wibo_add_search_parent(flags_excluded search-mode/root ${WIBO_TEST_BIN_DIR}/search-mode/current/dep_excluded.a)
        wibo_add_search_parent(flags_environment search-mode/root ${WIBO_TEST_BIN_DIR}/search-mode/environment/dep_environment.a)
        foreach(_directory . search-mode/user)
            wibo_add_fixture_dll(NAME flags_child DIRECTORY ${_directory}
                SOURCES test/system_loader_parent.c test/fixture_relocation_anchor.c
                BYPRODUCTS ${WIBO_TEST_BIN_DIR}/${_directory}/flags_child.a
                COMPILE_OPTIONS -nostdlib -Wl,--entry,0 -DFIXTURE_VALUE_ADJUSTMENT=0 -DFIXTURE_EXPORT_NAME=child_value
                    -Wl,--out-implib,${WIBO_TEST_BIN_DIR}/${_directory}/flags_child.a
                    ${WIBO_TEST_BIN_DIR}/search-mode/root/dep_inherited.a
                DEPENDS ${WIBO_TEST_BIN_DIR}/search-mode/root/dep_inherited.a)
        endforeach()
        wibo_add_fixture_dll(NAME flags_inherited DIRECTORY search-mode/root
            SOURCES test/system_loader_parent.c test/fixture_relocation_anchor.c
            COMPILE_OPTIONS -nostdlib -Wl,--entry,0 -DFIXTURE_VALUE_ADJUSTMENT=0 -DFIXTURE_DEPENDENCY_NAME=child_value
                ${WIBO_TEST_BIN_DIR}/./flags_child.a
            DEPENDS ${WIBO_TEST_BIN_DIR}/./flags_child.a)
        wibo_add_fixture_dll(NAME flags_forwarder DIRECTORY search-mode/root
            SOURCES test/library_search_forwarder.def test/fixture_relocation_anchor.c
            BYPRODUCTS ${WIBO_TEST_BIN_DIR}/search-mode/root/flags_forwarder.a
            COMPILE_OPTIONS -nostdlib -Wl,--entry,0
                -Wl,--out-implib,${WIBO_TEST_BIN_DIR}/search-mode/root/flags_forwarder.a)
        wibo_add_fixture_dll(NAME flags_forward_parent DIRECTORY search-mode/root
            SOURCES test/system_loader_parent.c test/fixture_relocation_anchor.c
            COMPILE_OPTIONS -nostdlib -Wl,--entry,0 -DFIXTURE_VALUE_ADJUSTMENT=0 -DFIXTURE_DEPENDENCY_NAME=forward_value
                ${WIBO_TEST_BIN_DIR}/search-mode/root/flags_forwarder.a
            DEPENDS ${WIBO_TEST_BIN_DIR}/search-mode/root/flags_forwarder.a)

        # Resources for test_resources
        add_custom_command(
            OUTPUT ${WIBO_TEST_BIN_DIR}/test_resources_res.o
            COMMAND ${WIBO_MINGW_WINDRES}
                    ${CMAKE_CURRENT_SOURCE_DIR}/test/test_resources.rc
                    -O coff -o test_resources_res.o
            WORKING_DIRECTORY ${WIBO_TEST_BIN_DIR}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_resources.rc)

        # Resource bytes owned by the release fixture executable.
        add_custom_command(
            OUTPUT ${WIBO_TEST_BIN_DIR}/test_resource_release_res.o
            COMMAND ${WIBO_MINGW_WINDRES}
                    ${CMAKE_CURRENT_SOURCE_DIR}/test/test_resource_release.rc
                    -O coff -o test_resource_release_res.o
            WORKING_DIRECTORY ${WIBO_TEST_BIN_DIR}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_resource_release.rc)

        add_custom_target(wibo_test_fixtures DEPENDS ${WIBO_TEST_FIXTURES})
        if(TARGET test_context_capture_helpers)
            add_dependencies(wibo_test_fixtures test_context_capture_helpers)
            set_tests_properties(wibo.test_context_capture_helpers PROPERTIES DEPENDS wibo.build_fixtures)
        endif()
        if(TARGET test_software_exception_capture_helpers)
            add_dependencies(wibo_test_fixtures test_software_exception_capture_helpers)
            set_tests_properties(wibo.test_software_exception_capture_helpers PROPERTIES DEPENDS wibo.build_fixtures)
        endif()
        if(TARGET test_software_exception_decision_helpers)
            add_dependencies(wibo_test_fixtures test_software_exception_decision_helpers)
            set_tests_properties(wibo.test_software_exception_decision_helpers PROPERTIES DEPENDS wibo.build_fixtures)
        endif()
        foreach(target test_software_exception_dispatch_helpers test_software_exception_dispatch_teb)
            if(TARGET ${target})
                add_dependencies(wibo_test_fixtures ${target})
                set_tests_properties(wibo.${target} PROPERTIES DEPENDS wibo.build_fixtures)
            endif()
        endforeach()
        if(TARGET test_darwin_teb_context)
            add_dependencies(wibo_test_fixtures test_darwin_teb_context)
            set_tests_properties(wibo.test_darwin_teb_context PROPERTIES DEPENDS wibo.build_fixtures)
        endif()

        if(CMAKE_CONFIGURATION_TYPES)
            set(_wibo_fixture_build_command
                ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG> --target wibo_test_fixtures)
        else()
            set(_wibo_fixture_build_command
                ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target wibo_test_fixtures)
        endif()

        add_test(NAME wibo.build_fixtures COMMAND ${_wibo_fixture_build_command})
    endif()
endif()

if (NOT TARGET wibo_test_fixtures)
    add_custom_target(wibo_test_fixtures)
endif()
