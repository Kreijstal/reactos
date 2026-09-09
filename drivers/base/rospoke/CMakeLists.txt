
add_library(rospoke MODULE
    rospoke.c
    pci.c
    slots.c
    exec.c
    rospoke.rc)

# Must stay "kernelmodedriver" and never become "wdmdriver": the latter stamps
# IMAGE_DLLCHARACTERISTICS_WDM_DRIVER, which costs the image DRVO_LEGACY_DRIVER
# and with it the ability to be unloaded -- the entire point of this driver.
set_module_type(rospoke kernelmodedriver)
add_importlibs(rospoke ntoskrnl hal)

# No add_registry_inf(): this driver registers no service of its own.  Somebody
# has to install it deliberately (rospoke.exe --install), because anyone who can
# open its device owns the machine.
add_cd_file(TARGET rospoke DESTINATION reactos/system32/drivers FOR all)
