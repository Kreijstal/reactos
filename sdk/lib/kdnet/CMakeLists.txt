add_library(kdnetprotocol
    kdpacket.c
    kdsession.c
    kdnet.c
    sha256.c)

target_include_directories(kdnetprotocol PRIVATE
    ${REACTOS_SOURCE_DIR}/sdk/lib/cryptlib)
target_link_libraries(kdnetprotocol cryptlib)
add_dependencies(kdnetprotocol xdk)
