# ---- V2: Live2D Cubism 2.x C++ port ----
set(V2_TARGET V2)

add_subdirectory(${LIVE2D_ROOT}/V2/src)

target_include_directories(${V2_TARGET}
    PUBLIC  ${LIVE2D_ROOT}/V2/src
)

# LOCAL PATCH (see ../../PATCHES.md): optional GLES 3 backend. GtkGLArea cannot
# provide a desktop compatibility profile, so the pet builds the renderer
# against GLES, where entry points are real exported symbols and no loader is
# needed. Upstream's glad path is kept for the desktop-GL build.
if(LIVE2D_GLES)
    target_compile_definitions(${V2_TARGET} PUBLIC LIVE2D_GLES)
    target_link_libraries(${V2_TARGET} GLESv2)
elseif (NOT CMAKE_SYSTEM_NAME MATCHES "Android")
    target_link_libraries(${V2_TARGET} glad)
endif()

target_link_libraries(${V2_TARGET} Common)

# LOCAL PATCH (see ../../PATCHES.md): upstream used the keyword signature here
# while lines 11/14 use the plain one, which CMake rejects outright. Also, the
# separate stdc++fs library is obsolete - std::filesystem has been part of
# libstdc++ proper since GCC 9, and Fedora no longer ships libstdc++fs.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(${V2_TARGET} stdc++fs)
  endif()
endif()
target_compile_features(${V2_TARGET} PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(${V2_TARGET} PRIVATE "/utf-8" "/wd4018" "/wd4244" "/wd4996")
endif()

# Alias for external projects: target_link_libraries(foo Live2D::V2)
add_library(Live2D::V2 ALIAS ${V2_TARGET})
