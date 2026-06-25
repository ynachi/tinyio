include(FetchContent)

message(STATUS "Setting up Google Abseil")

FetchContent_Declare(
        abseil_cpp
        GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
        GIT_TAG 20240116.2
        GIT_SHALLOW TRUE
)

# Configure Abseil build options BEFORE MakeAvailable
set(ABSL_BUILD_TESTING OFF CACHE BOOL "Build Abseil tests" FORCE)
set(ABSL_BUILD_DOCS OFF CACHE BOOL "Build Abseil docs" FORCE)
set(ABSL_USE_EXTERNAL_GOOGLETEST ON CACHE BOOL "Use system googletest if needed" FORCE)
set(ABSL_ENABLE_INSTALL ON CACHE BOOL "Enable abseil install" FORCE)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "Propagate CXX standard" FORCE)

FetchContent_MakeAvailable(abseil_cpp)
