add_library(${ERT_DIAMETERCOMM_TARGET_NAME} STATIC
    PeerConnection.cpp
    Peer.cpp
    DiameterServer.cpp
    DiameterClient.cpp
)

target_include_directories(${ERT_DIAMETERCOMM_TARGET_NAME}
PUBLIC
    ${ERT_DIAMETERCOMM_INCLUDE_BUILD_DIR}
    ${CMAKE_PREFIX_PATH}/include
)

target_link_libraries(${ERT_DIAMETERCOMM_TARGET_NAME}
PUBLIC
    Boost::system
    Threads::Threads
)

install(TARGETS ${ERT_DIAMETERCOMM_TARGET_NAME}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/ert
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/ert
)
