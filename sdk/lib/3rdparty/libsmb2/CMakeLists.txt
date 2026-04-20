
add_definitions(
    -DHAVE_CONFIG_H
    -D_WINDOWS
    -D_DLL
    -D__USE_CRTIMP
    -D_CRT_DECLARE_NONSTDC_NAMES=1
    -D_CRT_NONSTDC_NO_DEPRECATE
    -D_vscprintf_so=_vscprintf
    -D_U_=)

list(APPEND SOURCE
    lib/aes.c
    lib/aes_reference.c
    lib/aes128ccm.c
    lib/alloc.c
    lib/asn1-ber.c
    lib/compat.c
    lib/dcerpc.c
    lib/dcerpc-lsa.c
    lib/dcerpc-srvsvc.c
    lib/errors.c
    lib/hmac.c
    lib/hmac-md5.c
    lib/init.c
    lib/libsmb2.c
    lib/md4c.c
    lib/md5.c
    lib/ntlmssp.c
    lib/pdu.c
    lib/sha1.c
    lib/sha224-256.c
    lib/sha384-512.c
    lib/smb2-cmd-close.c
    lib/smb2-cmd-create.c
    lib/smb2-cmd-echo.c
    lib/smb2-cmd-error.c
    lib/smb2-cmd-flush.c
    lib/smb2-cmd-ioctl.c
    lib/smb2-cmd-lock.c
    lib/smb2-cmd-logoff.c
    lib/smb2-cmd-negotiate.c
    lib/smb2-cmd-notify-change.c
    lib/smb2-cmd-oplock-break.c
    lib/smb2-cmd-query-directory.c
    lib/smb2-cmd-query-info.c
    lib/smb2-cmd-read.c
    lib/smb2-cmd-session-setup.c
    lib/smb2-cmd-set-info.c
    lib/smb2-cmd-tree-connect.c
    lib/smb2-cmd-tree-disconnect.c
    lib/smb2-cmd-write.c
    lib/smb2-data-file-info.c
    lib/smb2-data-filesystem-info.c
    lib/smb2-data-reparse-point.c
    lib/smb2-data-security-descriptor.c
    lib/smb2-share-enum.c
    lib/smb2-signing.c
    lib/smb3-seal.c
    lib/socket.c
    lib/spnego-wrapper.c
    lib/sync.c
    lib/timestamps.c
    lib/unicode.c
    lib/usha.c
    reactos/rx-wsapoll.c
    reactos/rx-posix-stubs.c)

add_library(libsmb2 ${SOURCE})
add_dependencies(libsmb2 psdk)
# Consumers must link against ws2_32 (Winsock2).

target_include_directories(libsmb2
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/include/smb2
            ${CMAKE_CURRENT_SOURCE_DIR}/lib
            ${CMAKE_CURRENT_SOURCE_DIR}/reactos)

if(MSVC)
    target_compile_options(libsmb2 PRIVATE /FIrx-wsapoll.h)
else()
    target_compile_options(libsmb2 PRIVATE -include rx-wsapoll.h)
endif()

if(MSVC)
    target_compile_options(libsmb2 PRIVATE /wd4018 /wd4101 /wd4146 /wd4244 /wd4267 /wd4334)
else()
    target_compile_options(libsmb2 PRIVATE
        -Wno-error
        -Wno-unused-but-set-variable
        -Wno-sign-compare
        -Wno-unused-variable)
endif()
