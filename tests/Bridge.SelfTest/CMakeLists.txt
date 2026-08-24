add_executable(CoopStory.Bridge.SelfTest
    SelfTest.cpp
)

target_link_libraries(
    CoopStory.Bridge.SelfTest
    PRIVATE
        CoopStory.Bridge.Core
)
target_compile_features(CoopStory.Bridge.SelfTest PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(
        CoopStory.Bridge.SelfTest
        PRIVATE
            /W4
            /permissive-
            /EHsc
    )
else()
    target_compile_options(
        CoopStory.Bridge.SelfTest
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
endif()

add_test(
    NAME CoopStory.Bridge.SelfTest
    COMMAND CoopStory.Bridge.SelfTest
)
