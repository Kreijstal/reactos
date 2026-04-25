list(APPEND TERMSRV_CORE_SOURCE
    rdpbcgr.c
    rdpbcgr.h
    rdppeer.c
    termsrv.h)

add_library(termsrv_rdp STATIC ${TERMSRV_CORE_SOURCE})
add_dependencies(termsrv_rdp psdk)

add_executable(termsrv
    listener.c
    listener.h
    service.c
    termsrv.rc)

set_module_type(termsrv win32cui UNICODE)
target_link_libraries(termsrv termsrv_rdp)
add_importlibs(termsrv advapi32 ws2_32 msvcrt kernel32 ntdll)
add_cd_file(TARGET termsrv DESTINATION reactos/system32 FOR all)
add_registry_inf(termsrv_reg.inf)
