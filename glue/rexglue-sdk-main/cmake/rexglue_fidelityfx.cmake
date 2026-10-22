# rexglue_fidelityfx.cmake — Optional AMD FidelityFX integration via FetchContent
#
# Expects REXGLUE_ENABLE_FIDELITYFX to be set before inclusion.
# On success, creates amd_fidelityfx_vk and/or amd_fidelityfx_dx12 targets
# and sets REXGLUE_FIDELITYFX_SOURCE_DIR to the fetched SDK root.

if(NOT REXGLUE_ENABLE_FIDELITYFX)
    return()
endif()

# Generated overlay paths are SDK dependency inputs. Preserve their timestamps
# across ordinary reconfiguration so Ninja keeps shader permutations up to date.
function(_rexglue_ffx_write_if_changed path content)
    if(EXISTS "${path}")
        file(READ "${path}" existing)
        if("${existing}" STREQUAL "${content}")
            return()
        endif()
    endif()
    file(WRITE "${path}" "${content}")
endfunction()

# ── Dependency validation ────────────────────────────────────────────────
if(PS5)
    # [ps5 port] No Vulkan SDK on the PS5: the headers are the vendored ones and
    # there is no loader to link (RADV is in the eboot; the few vk* functions the
    # SDK calls directly are forwarded in ui/vulkan/vulkan_instance.cpp). An empty
    # archive stands in for the loader library.
    set(_rexglue_ffx_vulkan_stub "${CMAKE_BINARY_DIR}/ffx-ps5/libvulkan_none.a")
    if(NOT EXISTS "${_rexglue_ffx_vulkan_stub}")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/ffx-ps5")
        file(WRITE "${_rexglue_ffx_vulkan_stub}" "!<arch>\n")
    endif()
    set(Vulkan_INCLUDE_DIR "${REXGLUE_ROOT}/thirdparty/vulkan-headers/include" CACHE PATH "" FORCE)
    set(Vulkan_LIBRARY "${_rexglue_ffx_vulkan_stub}" CACHE FILEPATH "" FORCE)
endif()
if(NOT WIN32)
    find_package(Vulkan QUIET)
    if(NOT Vulkan_FOUND)
        message(WARNING
            "REXGLUE_ENABLE_FIDELITYFX requires the Vulkan SDK but it was not found.\n"
            "Install the LunarG Vulkan SDK: https://vulkan.lunarg.com/sdk/home\n"
            "Disabling FidelityFX.")
        set(REXGLUE_ENABLE_FIDELITYFX OFF CACHE BOOL "" FORCE)
        return()
    elseif(Vulkan_VERSION VERSION_LESS "1.3.250")
        message(WARNING
            "REXGLUE_ENABLE_FIDELITYFX requires Vulkan SDK >= 1.3.250 "
            "(found ${Vulkan_VERSION}).\n"
            "Update via: https://vulkan.lunarg.com/sdk/home\n"
            "Disabling FidelityFX.")
        set(REXGLUE_ENABLE_FIDELITYFX OFF CACHE BOOL "" FORCE)
        return()
    endif()
    # This pinned SDK generates permutation headers with its bundled Windows
    # FidelityFX_SC/glslangValidator tools. A system glslc does not replace the
    # permutation compiler. The fork explicitly supports running it via Wine.
    find_program(FFX_WINE_EXECUTABLE NAMES wine wine64)
    if(NOT FFX_WINE_EXECUTABLE)
        message(FATAL_ERROR
            "The pinned FidelityFX SDK requires Wine for shader permutation generation "
            "on non-Windows hosts. Install Wine or disable REXGLUE_ENABLE_FIDELITYFX.")
    endif()
endif()

# ── Fetch FidelityFX SDK ─────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    fidelityfx
    GIT_REPOSITORY https://github.com/rexglue/FidelityFX-SDK.git
    GIT_TAG        eee08db1688ac3d1275a70b728f4a8ba22914213
    GIT_SHALLOW    OFF
)
FetchContent_GetProperties(fidelityfx)
if(NOT fidelityfx_POPULATED)
    FetchContent_Populate(fidelityfx)
endif()

if(NOT WIN32)
    # Ninja's shell expands the SDK's unquoted -DOPTION={0,1} into two
    # definitions. VERBATIM alone does not escape braces on Unix. Overlay this build
    # file and symlink the remaining pinned tree; leave FetchContent pristine.
    function(_rexglue_ffx_overlay source destination changed_file)
        file(MAKE_DIRECTORY "${destination}")
        string(REGEX MATCH "^[^/]+" changed_head "${changed_file}")
        set(changed_tail "")
        if(changed_file MATCHES "^[^/]+/(.*)$")
            set(changed_tail "${CMAKE_MATCH_1}")
        endif()
        file(GLOB entries RELATIVE "${source}" "${source}/*")
        foreach(entry IN LISTS entries)
            if(entry STREQUAL changed_head)
                if(changed_tail)
                    _rexglue_ffx_overlay("${source}/${entry}" "${destination}/${entry}" "${changed_tail}")
                endif()
            elseif(NOT EXISTS "${destination}/${entry}")
                file(CREATE_LINK "${source}/${entry}" "${destination}/${entry}" SYMBOLIC)
            endif()
        endforeach()
    endfunction()
    set(_rexglue_ffx_shader_build "sdk/include/FidelityFX/gpu/CMakeCompileShaders.txt")
    set(_rexglue_ffx_portable_source "${fidelityfx_BINARY_DIR}/portable-source")
    _rexglue_ffx_overlay("${fidelityfx_SOURCE_DIR}" "${_rexglue_ffx_portable_source}"
        "${_rexglue_ffx_shader_build}")
    file(READ "${fidelityfx_SOURCE_DIR}/${_rexglue_ffx_shader_build}" _rexglue_ffx_shader_commands)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    string(REPLACE "set(_FFX_PERMUTATION_OUTPUTS)"
        "set(_FFX_PERMUTATION_OUTPUTS)\nset(EXECUTABLE \"${Python3_EXECUTABLE}\" \"${CMAKE_CURRENT_LIST_DIR}/fidelityfx_shader_compiler.py\" \${EXECUTABLE})"
        _rexglue_ffx_shader_commands "${_rexglue_ffx_shader_commands}")
    string(REPLACE "set(SC_ARGS \${BASE_ARGS} \${API_BASE_ARGS} \${PERMUTATION_ARGS})"
        "set(SC_ARGS \${BASE_ARGS} \${API_BASE_ARGS} \${PERMUTATION_ARGS})\nstring(REPLACE \"{\" \"@FFX_OPEN@\" SC_ARGS \"\${SC_ARGS}\")\nstring(REPLACE \"}\" \"@FFX_CLOSE@\" SC_ARGS \"\${SC_ARGS}\")"
        _rexglue_ffx_shader_commands "${_rexglue_ffx_shader_commands}")
    string(REPLACE "add_custom_command(" "add_custom_command(VERBATIM"
        _rexglue_ffx_shader_commands "${_rexglue_ffx_shader_commands}")
    string(REPLACE "DEPENDS \${PASS_SHADER}"
        "DEPENDS \${PASS_SHADER} \"${CMAKE_CURRENT_LIST_DIR}/fidelityfx_shader_compiler.py\""
        _rexglue_ffx_shader_commands "${_rexglue_ffx_shader_commands}")
    _rexglue_ffx_write_if_changed("${_rexglue_ffx_portable_source}/${_rexglue_ffx_shader_build}"
        "${_rexglue_ffx_shader_commands}")
    if(PS5)
        # [ps5 port] One static program: the API library is shared upstream.
        # Overlay ffx-api/ (its CMakeLists.txt rewritten) in place of the link
        # to the pinned checkout's.
        set(_rexglue_ffx_api_dir "${_rexglue_ffx_portable_source}/ffx-api")
        if(IS_SYMLINK "${_rexglue_ffx_api_dir}")
            file(REMOVE "${_rexglue_ffx_api_dir}")
        endif()
        _rexglue_ffx_overlay("${fidelityfx_SOURCE_DIR}/ffx-api" "${_rexglue_ffx_api_dir}" "CMakeLists.txt")
        file(READ "${fidelityfx_SOURCE_DIR}/ffx-api/CMakeLists.txt" _rexglue_ffx_api_cmake)
        string(REPLACE "add_library(amd_fidelityfx_\${FFX_PLATFORM_NAME} SHARED"
            "add_library(amd_fidelityfx_\${FFX_PLATFORM_NAME} STATIC"
            _rexglue_ffx_api_cmake "${_rexglue_ffx_api_cmake}")
        _rexglue_ffx_write_if_changed("${_rexglue_ffx_api_dir}/CMakeLists.txt" "${_rexglue_ffx_api_cmake}")
    endif()
    set(fidelityfx_SOURCE_DIR "${_rexglue_ffx_portable_source}")
endif()

set(REXGLUE_FIDELITYFX_SOURCE_DIR "${fidelityfx_SOURCE_DIR}" CACHE INTERNAL
    "Root of the fetched FidelityFX SDK source tree")

# ── Backend selection ────────────────────────────────────────────────────
set(REXGLUE_FIDELITYFX_BACKEND "auto" CACHE STRING
    "FidelityFX backend to build (auto, vk, dx12)")
set_property(CACHE REXGLUE_FIDELITYFX_BACKEND PROPERTY STRINGS auto vk dx12)
set(_rexglue_fidelityfx_backend "${REXGLUE_FIDELITYFX_BACKEND}")
string(TOLOWER "${_rexglue_fidelityfx_backend}" _rexglue_fidelityfx_backend)

if(_rexglue_fidelityfx_backend STREQUAL "auto")
    if(WIN32 AND REXGLUE_USE_D3D12)
        set(_rexglue_fidelityfx_backend "dx12")
    elseif(REXGLUE_USE_VULKAN)
        set(_rexglue_fidelityfx_backend "vk")
    elseif(REXGLUE_USE_D3D12)
        set(_rexglue_fidelityfx_backend "dx12")
    endif()
endif()

if(_rexglue_fidelityfx_backend STREQUAL "vk")
    if(NOT REXGLUE_USE_VULKAN)
        message(FATAL_ERROR
            "REXGLUE_FIDELITYFX_BACKEND=vk requires REXGLUE_USE_VULKAN=ON")
    endif()
    set(FFX_API_BACKEND VK_X64 CACHE STRING "" FORCE)
elseif(_rexglue_fidelityfx_backend STREQUAL "dx12")
    if(NOT REXGLUE_USE_D3D12)
        message(FATAL_ERROR
            "REXGLUE_FIDELITYFX_BACKEND=dx12 requires REXGLUE_USE_D3D12=ON")
    endif()
    set(FFX_API_BACKEND DX12_X64 CACHE STRING "" FORCE)
elseif(_rexglue_fidelityfx_backend STREQUAL "")
    message(FATAL_ERROR "FidelityFX requires a supported graphics backend")
else()
    message(FATAL_ERROR
        "Invalid REXGLUE_FIDELITYFX_BACKEND='${REXGLUE_FIDELITYFX_BACKEND}' "
        "(expected auto, vk, or dx12)")
endif()

if(REXGLUE_USE_VULKAN AND REXGLUE_USE_D3D12)
    message(STATUS
        "FidelityFX backend selected for this build: ${_rexglue_fidelityfx_backend}")
endif()

# ── Build FidelityFX ─────────────────────────────────────────────────────
option(REXGLUE_FIDELITYFX_FRAME_GENERATION "Build the FidelityFX frame-generation provider" ON)
set(FFX_API_ENABLE_FRAMEGEN_PROVIDER ${REXGLUE_FIDELITYFX_FRAME_GENERATION} CACHE BOOL "" FORCE)
# Never build a runtime whose required shader permutation headers are absent.
set(FFX_API_AUTO_COMPILE_SHADERS ON CACHE BOOL "" FORCE)
add_subdirectory("${fidelityfx_SOURCE_DIR}/ffx-api" "${fidelityfx_BINARY_DIR}/ffx-api" EXCLUDE_FROM_ALL)

if(TARGET amd_fidelityfx_vk AND REXGLUE_FIDELITYFX_FRAME_GENERATION)
    # The SDK's Vulkan proxy swapchain is Win32-only. Direct interpolation is
    # portable and uses NO_SWAPCHAIN_CONTEXT_NOTIFY. Build the real interpolation
    # provider while excluding only the unsupported proxy from the registry.
    # Generate an overlay rather than modifying the pinned dependency checkout.
    # [windows port] Left out on Windows too: the renderer interpolates directly
    # everywhere (fsr_vulkan.cpp), and the proxy's MSVC-only code (temporaries
    # bound to non-const references) does not build with clang.
    set(_rexglue_ffx_provider "${fidelityfx_SOURCE_DIR}/ffx-api/src/ffx_provider.cpp")
    file(READ "${_rexglue_ffx_provider}" _rexglue_ffx_provider_source)
    string(REPLACE "#ifdef FFX_BACKEND_VK"
        "#if defined(FFX_BACKEND_VK) && defined(REXGLUE_FFX_VK_PROXY_SWAPCHAIN)"
        _rexglue_ffx_provider_source "${_rexglue_ffx_provider_source}")
    set(_rexglue_ffx_provider_overlay "${fidelityfx_BINARY_DIR}/ffx_provider_direct_vk.cpp")
    _rexglue_ffx_write_if_changed("${_rexglue_ffx_provider_overlay}" "${_rexglue_ffx_provider_source}")
    get_target_property(_rexglue_ffx_sources amd_fidelityfx_vk SOURCES)
    list(REMOVE_ITEM _rexglue_ffx_sources "${_rexglue_ffx_provider}")
    list(FILTER _rexglue_ffx_sources EXCLUDE REGEX ".*/ffx_provider_framegenerationswapchain_vk\\.(h|cpp)$")
    list(APPEND _rexglue_ffx_sources "${_rexglue_ffx_provider_overlay}")
    set_property(TARGET amd_fidelityfx_vk PROPERTY SOURCES "${_rexglue_ffx_sources}")
    target_include_directories(amd_fidelityfx_vk PRIVATE "${fidelityfx_SOURCE_DIR}/ffx-api/src")
    if(TARGET ffx_backend_vk_x64)
        # (the backend adds the proxy's sources on Win32 only)
        get_target_property(_rexglue_ffx_backend_sources ffx_backend_vk_x64 SOURCES)
        list(FILTER _rexglue_ffx_backend_sources EXCLUDE REGEX "/FrameInterpolationSwapchain/")
        set_property(TARGET ffx_backend_vk_x64 PROPERTY SOURCES "${_rexglue_ffx_backend_sources}")
    endif()
endif()

# [windows port] On Windows too: the same clang emits the same aligned stores.
if(TARGET ffx_backend_vk_x64)
    # The Vulkan backend carves its scratch buffer into arrays at 4-byte
    # offsets, but EffectContext is alignas(32): clang may store to it with
    # aligned vector moves, which fault (the PS5 crashed this way creating the
    # FSR 3 context). Lay every array out on 64-byte boundaries instead, with
    # room for the first one's padding.
    set(_rexglue_ffx_vk "${fidelityfx_SOURCE_DIR}/sdk/src/backends/vk/ffx_vk.cpp")
    file(READ "${_rexglue_ffx_vk}" _rexglue_ffx_vk_source)
    set(_rexglue_ffx_vk_patched "${_rexglue_ffx_vk_source}")
    string(REPLACE "sizeof(uint32_t));" "64);" _rexglue_ffx_vk_patched "${_rexglue_ffx_vk_patched}")
    string(REPLACE "uint8_t* pMem = (uint8_t*)((BackendContext_VK*)(backendContext + 1));"
        "uint8_t* pMem = (uint8_t*)((reinterpret_cast<uintptr_t>(backendContext + 1) + 63) & ~uintptr_t(63));"
        _rexglue_ffx_vk_patched "${_rexglue_ffx_vk_patched}")
    string(REPLACE "return FFX_ALIGN_UP(sizeof(BackendContext_VK) + extensionPropArraySize"
        "return FFX_ALIGN_UP(sizeof(BackendContext_VK) + 64 + extensionPropArraySize"
        _rexglue_ffx_vk_patched "${_rexglue_ffx_vk_patched}")
    string(FIND "${_rexglue_ffx_vk_patched}" "& ~uintptr_t(63)" _rexglue_ffx_vk_mapped)
    string(FIND "${_rexglue_ffx_vk_patched}" "sizeof(BackendContext_VK) + 64 +" _rexglue_ffx_vk_sized)
    string(FIND "${_rexglue_ffx_vk_patched}" "sizeof(uint32_t));" _rexglue_ffx_vk_left)
    if(_rexglue_ffx_vk_mapped EQUAL -1 OR _rexglue_ffx_vk_sized EQUAL -1 OR NOT _rexglue_ffx_vk_left EQUAL -1)
        message(FATAL_ERROR "FidelityFX: the Vulkan backend's scratch layout changed; update the alignment overlay")
    endif()
    if(PS5)
        # [ps5 port] The SDK reports a failed Vulkan call only as "runtime
        # error" by the time it reaches the API. Say which call it was
        # (rexglue_ffx_report, in fsr_vulkan.cpp, logs the source line).
        string(REPLACE "return FFX_ERROR_BACKEND_API_ERROR;"
            "return FfxErrorCode(rexglue_ffx_report(__LINE__, FFX_ERROR_BACKEND_API_ERROR));"
            _rexglue_ffx_vk_patched "${_rexglue_ffx_vk_patched}")
        set(_rexglue_ffx_vk_patched
            "extern \"C\" int rexglue_ffx_report(int line, int code);\n${_rexglue_ffx_vk_patched}")
        # wchar_t is 2 bytes on the PS5, but its libc++ converts UTF-8 to
        # 4-byte units (it only knows Windows has a short wchar_t): every
        # shader binding name came out as its first letter, no binding
        # matched, and creating FSR 3 failed. The names are ASCII.
        set(_rexglue_ffx_vk_from "    std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> converter;\n    wcscpy_s(outputBuffer, outputLen, converter.from_bytes(inputName).c_str());")
        set(_rexglue_ffx_vk_to "    std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> converter;\n    strcpy_s(outputBuffer, outputLen, converter.to_bytes(inputName).c_str());")
        string(FIND "${_rexglue_ffx_vk_patched}" "${_rexglue_ffx_vk_from}" _rexglue_ffx_vk_from_at)
        string(FIND "${_rexglue_ffx_vk_patched}" "${_rexglue_ffx_vk_to}" _rexglue_ffx_vk_to_at)
        if(_rexglue_ffx_vk_from_at EQUAL -1 OR _rexglue_ffx_vk_to_at EQUAL -1)
            message(FATAL_ERROR "FidelityFX: the Vulkan backend's name conversion changed; update the PS5 overlay")
        endif()
        string(REPLACE "${_rexglue_ffx_vk_from}"
            "    for (size_t i = 0; inputName && inputName[i] && i + 1 < outputLen; ++i)\n        outputBuffer[i] = wchar_t(static_cast<unsigned char>(inputName[i]));"
            _rexglue_ffx_vk_patched "${_rexglue_ffx_vk_patched}")
        string(REPLACE "${_rexglue_ffx_vk_to}"
            "    for (size_t i = 0; inputName && inputName[i] && i + 1 < outputLen; ++i)\n        outputBuffer[i] = inputName[i] < 128 ? char(inputName[i]) : '?';"
            _rexglue_ffx_vk_patched "${_rexglue_ffx_vk_patched}")
    endif()
    set(_rexglue_ffx_vk_overlay "${fidelityfx_BINARY_DIR}/ffx_vk_aligned.cpp")
    _rexglue_ffx_write_if_changed("${_rexglue_ffx_vk_overlay}" "${_rexglue_ffx_vk_patched}")
    get_target_property(_rexglue_ffx_vk_sources ffx_backend_vk_x64 SOURCES)
    list(FILTER _rexglue_ffx_vk_sources EXCLUDE REGEX "(^|/)ffx_vk\\.cpp$")
    list(APPEND _rexglue_ffx_vk_sources "${_rexglue_ffx_vk_overlay}")
    set_property(TARGET ffx_backend_vk_x64 PROPERTY SOURCES "${_rexglue_ffx_vk_sources}")
endif()

if(PS5)
    # [ps5 port] Same for the effects FSR 3 is made of: every error return
    # in their context code goes through rexglue_ffx_report with its line
    # (plus base, which tells the files apart in the log).
    function(_rexglue_ffx_report_errors target component base)
        if(NOT TARGET ${target})
            return()
        endif()
        set(source "${fidelityfx_SOURCE_DIR}/sdk/src/components/${component}/ffx_${component}.cpp")
        file(READ "${source}" text)
        set(anchor "#include \"ffx_${component}_private.h\"")
        string(FIND "${text}" "${anchor}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "FidelityFX: ${component} no longer includes its private header; update the report overlay")
        endif()
        string(REPLACE "${anchor}" "${anchor}
extern \"C\" int rexglue_ffx_report(int line, int code);
#undef FFX_RETURN_ON_ERROR
#define FFX_RETURN_ON_ERROR(x, y) if (!(x)) { return FfxErrorCode(rexglue_ffx_report(${base} + __LINE__, (y))); }
#undef FFX_VALIDATE
#define FFX_VALIDATE(x) { FfxErrorCode ret = x; FFX_RETURN_ON_ERROR(ret == FFX_OK, ret); }" text "${text}")
        set(overlay "${fidelityfx_BINARY_DIR}/ffx_${component}_reported.cpp")
        _rexglue_ffx_write_if_changed("${overlay}" "${text}")
        get_target_property(sources ${target} SOURCES)
        list(FILTER sources EXCLUDE REGEX "(^|/)ffx_${component}\\.cpp$")
        list(APPEND sources "${overlay}")
        set_property(TARGET ${target} PROPERTY SOURCES "${sources}")
        target_include_directories(${target} PRIVATE "${fidelityfx_SOURCE_DIR}/sdk/src/components/${component}")
    endfunction()
    _rexglue_ffx_report_errors(ffx_fsr3upscaler_x64 fsr3upscaler 100000)
    _rexglue_ffx_report_errors(ffx_frameinterpolation_x64 frameinterpolation 200000)
    _rexglue_ffx_report_errors(ffx_opticalflow_x64 opticalflow 300000)
endif()

# The upstream FidelityFX targets expose source-tree include paths in
# INTERFACE_INCLUDE_DIRECTORIES, which breaks our install export checks.
# We only link against these targets internally, so no public includes are
# needed on the exported interface.
if(TARGET amd_fidelityfx_vk)
    set_target_properties(amd_fidelityfx_vk PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ""
    )
    # An internal interface target gives renderer adapters the correct headers
    # and provider availability without exporting the SDK's source paths.
    add_library(rexglue_fidelityfx_vulkan INTERFACE)
    target_link_libraries(rexglue_fidelityfx_vulkan INTERFACE amd_fidelityfx_vk)
    target_include_directories(rexglue_fidelityfx_vulkan INTERFACE
        "$<BUILD_INTERFACE:${fidelityfx_SOURCE_DIR}/ffx-api/include>")
    target_compile_definitions(rexglue_fidelityfx_vulkan INTERFACE REX_HAS_FIDELITYFX_VULKAN=1)
    if(REXGLUE_FIDELITYFX_FRAME_GENERATION)
        target_compile_definitions(rexglue_fidelityfx_vulkan INTERFACE REX_HAS_FIDELITYFX_FRAMEGENERATION=1)
    endif()
endif()
if(TARGET amd_fidelityfx_dx12)
    set_target_properties(amd_fidelityfx_dx12 PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ""
    )
endif()

# FidelityFX's toolchain.cmake force-sets CMAKE_GENERATOR_PLATFORM (for VS generators).
# With Ninja this variable is invalid and poisons every subsequent try_compile() call.
# Clear it from the cache.
unset(CMAKE_GENERATOR_PLATFORM CACHE)
