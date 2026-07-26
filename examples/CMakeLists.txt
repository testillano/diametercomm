add_executable(echo_server echo_server.cpp)
target_link_libraries(echo_server PRIVATE
    ert_diametercomm
    Boost::system
    Threads::Threads
)

add_executable(ping_client ping_client.cpp)
target_link_libraries(ping_client PRIVATE
    ert_diametercomm
    Boost::system
    Threads::Threads
)
