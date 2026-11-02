add_library(wpe_network STATIC
    transport.c
    transport_enet.c
    transport_memory.c)
add_library(wpe::network ALIAS wpe_network)

target_link_libraries(wpe_network
    PUBLIC wpe::core
    PRIVATE wpe_dep_enet)
wpe_configure_target(wpe_network)
