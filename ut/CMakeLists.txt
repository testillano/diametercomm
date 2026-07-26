add_executable(unit-test "")

add_subdirectory(PeerConnection)
add_subdirectory(Peer)

# Imported libraries
add_library(ert_logger STATIC IMPORTED)
add_library(ert_diametercodec STATIC IMPORTED)

set_property(TARGET ert_logger PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_logger.a)
set_property(TARGET ert_diametercodec PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_diametercodec.a)

target_link_libraries(unit-test
PRIVATE
    gtest
    gmock
    gtest_main

    ert_diametercomm
    ert_diametercodec
    ert_logger

    prometheus-cpp-pull
    prometheus-cpp-core
    z

    nlohmann_json_schema_validator

    Boost::system
    Threads::Threads
)
