include(FetchContent)

FetchContent_Declare(
        helio
        GIT_REPOSITORY https://github.com/romange/helio.git
        GIT_TAG master
)

FetchContent_MakeAvailable(helio)
