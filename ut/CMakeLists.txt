add_executable(unit-test "")

add_subdirectory(PeerConnection)
add_subdirectory(Peer)
add_subdirectory(DiameterServer)
add_subdirectory(DiameterClient)

# Imported libraries
if(NOT TARGET ert_logger)
    add_library(ert_logger STATIC IMPORTED)
    set_property(TARGET ert_logger PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_logger.a)
endif()
if(NOT TARGET ert_diametercodec)
    add_library(ert_diametercodec STATIC IMPORTED)
    set_property(TARGET ert_diametercodec PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_diametercodec.a)
endif()
if(NOT TARGET ert_metrics)
    add_library(ert_metrics STATIC IMPORTED)
    set_property(TARGET ert_metrics PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_metrics.a)
endif()

target_link_libraries(unit-test
PRIVATE
    gtest
    gmock
    gtest_main

    ert_diametercomm
    ert_diametercodec
    ert_metrics
    ert_logger

    prometheus-cpp-pull
    prometheus-cpp-core
    z

    nlohmann_json_schema_validator

    Boost::system
    Threads::Threads
)
