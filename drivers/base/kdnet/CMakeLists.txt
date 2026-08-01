spec2def(kdnet.dll kdnet.spec ADD_IMPORTLIB)

set(RTL8168_DIR ${REACTOS_SOURCE_DIR}/drivers/network/dd/rtl8168)

add_library(kdnet MODULE
    kdnet.c
    kdnet.rc
    ${RTL8168_DIR}/hardware.c
    ${RTL8168_DIR}/indirect.c
    ${RTL8168_DIR}/chipinit.c
    ${RTL8168_DIR}/phy.c
    ${CMAKE_CURRENT_BINARY_DIR}/kdnet.def)

target_compile_definitions(kdnet PRIVATE RTL8168_EARLY)
target_include_directories(kdnet PRIVATE ${RTL8168_DIR})
target_link_libraries(kdnet kdnetprotocol)

set_module_type(kdnet kerneldll ENTRYPOINT 0)
set_subsystem(kdnet native)
add_importlibs(kdnet ntoskrnl hal)
add_dependencies(kdnet psdk bugcodes)

add_cd_file(TARGET kdnet DESTINATION reactos/system32 NO_CAB FOR all)
add_cd_file(TARGET kdnet DESTINATION ${ARCH}/system32 NO_CAB FOR bootcd regtest)
