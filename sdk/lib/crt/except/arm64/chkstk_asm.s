
/* CODE **********************************************************************/

    .text
    .align 2

    .globl __chkstk
    .def __chkstk; .scl 2; .type 32; .endef
__chkstk:
    ret

    .globl __alloca_probe
    .def __alloca_probe; .scl 2; .type 32; .endef
__alloca_probe:
    ret
/* EOF */
