
list(APPEND SOURCE
    adapter.cpp
    codec.cpp
    topology.cpp
    wave.cpp)

add_library(hdaudio MODULE
    ${SOURCE}
    hdaudio.rc)

target_link_libraries(hdaudio stdunk libcntpr uuid)
set_module_type(hdaudio wdmdriver)
add_importlibs(hdaudio portcls ntoskrnl hal)

add_cd_file(TARGET hdaudio DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(hdaudio hdaudio.inf)
