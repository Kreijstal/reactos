
# drivers/directx - WDDM DirectX kernel subsystem
#
#   dxgmms1  video memory manager / scheduler helper (built, not yet used by dxgkrnl)
#   dxgkrnl  WDDM display port driver and D3DKMT kernel side
#   softgpu  Basic-Display fallback WDDM miniport (built, not registered)
#   viogpu   genuine Red Hat viogpudo.sys (virtio-win) for the QEMU virtio-gpu device

add_subdirectory(dxgmms1)
add_subdirectory(dxgkrnl)
add_subdirectory(softgpu)
add_subdirectory(viogpu)
