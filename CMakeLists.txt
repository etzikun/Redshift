cmake_minimum_required(VERSION 3.20)
project(Redshift VERSION 1.0.0 LANGUAGES CXX RC)

option(REDSHIFT_ENABLE_CODE_ANALYSIS
    "Run MSVC code analysis on first-party Redshift C++" OFF)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Redshift requires a 64-bit Windows build.")
endif()

set(REDSHIFT_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${REDSHIFT_GENERATED_DIR}")
configure_file(src/app/version.h.in "${REDSHIFT_GENERATED_DIR}/version.h" @ONLY)
configure_file(src/app/app.manifest.in "${REDSHIFT_GENERATED_DIR}/app.manifest" @ONLY)

if(WIN32 AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(_redshift_prefix "$ENV{ProgramW6432}")
  if(NOT _redshift_prefix)
    set(_redshift_prefix "C:/Program Files")
  endif()
  file(TO_CMAKE_PATH "${_redshift_prefix}/Redshift" _redshift_prefix)
  set(CMAKE_INSTALL_PREFIX "${_redshift_prefix}" CACHE PATH "Install path prefix" FORCE)
  unset(_redshift_prefix)
endif()

set(DETOURS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/detours/src")
add_library(detours STATIC
    ${DETOURS_DIR}/detours.cpp ${DETOURS_DIR}/modules.cpp
    ${DETOURS_DIR}/disasm.cpp ${DETOURS_DIR}/image.cpp ${DETOURS_DIR}/creatwth.cpp)
target_include_directories(detours PUBLIC ${DETOURS_DIR})
target_compile_definitions(detours PRIVATE _CRT_STDIO_ARBITRARY_WIDE_SPECIFIERS)
target_compile_options(detours PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W3 /utf-8>)

add_library(RedshiftPrivacyHook SHARED
    src/hook/hook.cpp src/hook/hook.def src/common/privacy_policy.cpp)
target_compile_features(RedshiftPrivacyHook PRIVATE cxx_std_20)
target_compile_definitions(RedshiftPrivacyHook PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(RedshiftPrivacyHook PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive- /guard:cf /sdl /EHsc /utf-8>)
if(REDSHIFT_ENABLE_CODE_ANALYSIS)
  target_compile_options(RedshiftPrivacyHook PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/analyze>)
endif()
target_link_options(RedshiftPrivacyHook PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /GUARD:CF>)
target_link_libraries(RedshiftPrivacyHook PRIVATE detours ntdll)
target_include_directories(RedshiftPrivacyHook PRIVATE src/common)

add_executable(Redshift WIN32
    src/app/main.cpp src/app/Redshift.rc src/common/privacy_policy.cpp
    "${REDSHIFT_GENERATED_DIR}/app.manifest")
target_compile_features(Redshift PRIVATE cxx_std_20)
set_property(TARGET Redshift RedshiftPrivacyHook detours PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_definitions(Redshift PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(Redshift PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive- /guard:cf /sdl /EHsc /utf-8>)
if(REDSHIFT_ENABLE_CODE_ANALYSIS)
  target_compile_options(Redshift PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/analyze>)
endif()
target_link_options(Redshift PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /GUARD:CF>)
target_include_directories(Redshift PRIVATE src/app src/common "${REDSHIFT_GENERATED_DIR}")
target_link_libraries(Redshift PRIVATE detours comctl32 comdlg32 dwmapi ole32 shell32 uuid)
install(TARGETS Redshift RedshiftPrivacyHook RUNTIME DESTINATION .)
