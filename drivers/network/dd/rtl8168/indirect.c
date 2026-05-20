/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS Realtek 8168/8111 driver
 * FILE:        indirect.c
 * PURPOSE:     ERI / EPHY / GPHY-OCP / MAC-OCP / MDIO indirect register access
 *
 * Protocols ported from Linux r8169_main.c (GPLv2):
 *   _rtl_eri_read / _rtl_eri_write   (drivers/net/ethernet/realtek/r8169_main.c:954/975)
 *   rtl_ephy_write / rtl_ephy_read   (:1261/1271)
 *   r8168_phy_ocp_write/_read         (:1018/1028)
 *   r8168_mac_ocp_write/_read/_modify (:1039/1056/1078)
 *   r8169_mdio_write / _read          (:1154/1166)
 *   r8168g_mdio_write / _read         (:1107/1123)
 *
 * The indirect access registers are command/data pairs.  A write loads
 * data into the data register, sets WRITE_CMD + target in the address
 * register, and polls the FLAG bit until the chip clears it (transfer
 * complete).  A read sets READ_CMD in the address register and polls
 * the FLAG bit until the chip sets it (data ready).
 */

#include "nic.h"

#define NDEBUG
#include <debug.h>

#define ERI_TIMEOUT_US      10000   /* total budget for ERI poll */
#define EPHY_TIMEOUT_US      1000
#define OCP_TIMEOUT_US        250

VOID
NTAPI
RtlEriWrite (
    IN PRTL_ADAPTER Adapter,
    IN ULONG Addr,
    IN ULONG Mask,
    IN ULONG Val
    )
{
    ULONG cmd;
    ULONG waited = 0;

    if ((Addr & 3) || !Mask)
        return;

    cmd = ERIAR_WRITE_CMD | ERIAR_EXGMAC | Mask | (Addr & 0xFFF);
    RtlWriteReg32(Adapter, R_ERIDR, Val);
    RtlWriteReg32(Adapter, R_ERIAR, cmd);

    /* Poll FLAG=0 (write complete). */
    while ((RtlReadReg32(Adapter, R_ERIAR) & ERIAR_FLAG) && waited < ERI_TIMEOUT_US)
    {
        NdisStallExecution(10);
        waited += 10;
    }
}

ULONG
NTAPI
RtlEriRead (
    IN PRTL_ADAPTER Adapter,
    IN ULONG Addr
    )
{
    ULONG cmd = ERIAR_READ_CMD | ERIAR_EXGMAC | ERIAR_MASK_1111 | (Addr & 0xFFF);
    ULONG waited = 0;

    RtlWriteReg32(Adapter, R_ERIAR, cmd);

    /* Poll FLAG=1 (data ready). */
    while (!(RtlReadReg32(Adapter, R_ERIAR) & ERIAR_FLAG) && waited < ERI_TIMEOUT_US)
    {
        NdisStallExecution(10);
        waited += 10;
    }

    if (waited >= ERI_TIMEOUT_US)
        return ~0U;

    return RtlReadReg32(Adapter, R_ERIDR);
}

VOID
NTAPI
RtlW0w1Eri (
    IN PRTL_ADAPTER A,
    IN ULONG Addr,
    IN ULONG Set,
    IN ULONG Clr
    )
{
    ULONG v = RtlEriRead(A, Addr);
    RtlEriWrite(A, Addr, ERIAR_MASK_1111, (v & ~Clr) | Set);
}

VOID
NTAPI
RtlEriSetBits (
    IN PRTL_ADAPTER A,
    IN ULONG Addr,
    IN ULONG Bits
    )
{
    RtlW0w1Eri(A, Addr, Bits, 0);
}

VOID
NTAPI
RtlEriClearBits (
    IN PRTL_ADAPTER A,
    IN ULONG Addr,
    IN ULONG Bits
    )
{
    RtlW0w1Eri(A, Addr, 0, Bits);
}

/*--------------------------------- EPHY ---------------------------------*/

VOID
NTAPI
RtlEphyWrite (
    IN PRTL_ADAPTER A,
    IN ULONG Reg,
    IN USHORT Val
    )
{
    ULONG cmd = EPHYAR_WRITE_CMD |
                (Val & EPHYAR_DATA_MASK) |
                ((Reg & EPHYAR_REG_MASK) << EPHYAR_REG_SHIFT);
    ULONG waited = 0;

    RtlWriteReg32(A, R_EPHYAR, cmd);

    while ((RtlReadReg32(A, R_EPHYAR) & EPHYAR_FLAG) && waited < EPHY_TIMEOUT_US)
    {
        NdisStallExecution(10);
        waited += 10;
    }
}

USHORT
NTAPI
RtlEphyRead (
    IN PRTL_ADAPTER A,
    IN ULONG Reg
    )
{
    ULONG waited = 0;

    RtlWriteReg32(A, R_EPHYAR, (Reg & EPHYAR_REG_MASK) << EPHYAR_REG_SHIFT);

    while (!(RtlReadReg32(A, R_EPHYAR) & EPHYAR_FLAG) && waited < EPHY_TIMEOUT_US)
    {
        NdisStallExecution(10);
        waited += 10;
    }

    if (waited >= EPHY_TIMEOUT_US)
        return 0xFFFF;

    return (USHORT)(RtlReadReg32(A, R_EPHYAR) & EPHYAR_DATA_MASK);
}

VOID
NTAPI
RtlEphyInit (
    IN PRTL_ADAPTER A,
    IN const EPHY_INFO *e,
    IN ULONG Count
    )
{
    ULONG i;
    USHORT w;

    for (i = 0; i < Count; i++, e++)
    {
        w = (RtlEphyRead(A, e->Offset) & ~e->Mask) | e->Bits;
        RtlEphyWrite(A, e->Offset, w);
    }
}

/*--------------------------- GPHY / MAC OCP -----------------------------*/

VOID
NTAPI
RtlPhyOcpWrite (
    IN PRTL_ADAPTER A,
    IN ULONG Reg,
    IN ULONG Data
    )
{
    ULONG waited = 0;

    /* Out-of-range OCP regs silently no-op (matches Linux rtl_ocp_reg_failure). */
    if (Reg & 1)
        return;

    RtlWriteReg32(A, R_GPHY_OCP, OCPAR_FLAG | (Reg << 15) | (Data & 0xFFFF));

    while ((RtlReadReg32(A, R_GPHY_OCP) & OCPAR_FLAG) && waited < OCP_TIMEOUT_US)
    {
        NdisStallExecution(10);
        waited += 10;
    }
}

USHORT
NTAPI
RtlPhyOcpRead (
    IN PRTL_ADAPTER A,
    IN ULONG Reg
    )
{
    ULONG waited = 0;

    if (Reg & 1)
        return 0xFFFF;

    RtlWriteReg32(A, R_GPHY_OCP, Reg << 15);

    while (!(RtlReadReg32(A, R_GPHY_OCP) & OCPAR_FLAG) && waited < OCP_TIMEOUT_US)
    {
        NdisStallExecution(10);
        waited += 10;
    }

    if (waited >= OCP_TIMEOUT_US)
        return 0xFFFF;

    return (USHORT)(RtlReadReg32(A, R_GPHY_OCP) & 0xFFFF);
}

VOID
NTAPI
RtlMacOcpWrite (
    IN PRTL_ADAPTER A,
    IN ULONG Reg,
    IN ULONG Data
    )
{
    if (Reg & 1)
        return;

    RtlWriteReg32(A, R_OCPDR, OCPAR_FLAG | (Reg << 15) | (Data & 0xFFFF));
}

USHORT
NTAPI
RtlMacOcpRead (
    IN PRTL_ADAPTER A,
    IN ULONG Reg
    )
{
    if (Reg & 1)
        return 0xFFFF;

    RtlWriteReg32(A, R_OCPDR, Reg << 15);
    return (USHORT)(RtlReadReg32(A, R_OCPDR) & 0xFFFF);
}

VOID
NTAPI
RtlMacOcpModify (
    IN PRTL_ADAPTER A,
    IN ULONG Reg,
    IN USHORT Mask,
    IN USHORT Set
    )
{
    USHORT v = RtlMacOcpRead(A, Reg);
    RtlMacOcpWrite(A, Reg, (USHORT)((v & ~Mask) | Set));
}

/*-------------------------------- MDIO ----------------------------------*/

/* Direct MDIO via R_PHYAR -- chip versions <8168G use this path.
 * Linux comment: 20us delay required after write/read complete. */
static VOID
RtlMdioWriteDirect(IN PRTL_ADAPTER A, IN ULONG Reg, IN USHORT Val)
{
    ULONG waited = 0;

    RtlWriteReg32(A, R_PHYAR,
        PHYAR_FLAG | ((Reg & 0x1F) << PHYAR_REG_SHIFT) | (Val & PHYAR_DATA_MASK));

    while ((RtlReadReg32(A, R_PHYAR) & PHYAR_FLAG) && waited < 500)
    {
        NdisStallExecution(25);
        waited += 25;
    }
    NdisStallExecution(20);
}

static USHORT
RtlMdioReadDirect(IN PRTL_ADAPTER A, IN ULONG Reg)
{
    ULONG waited = 0;
    USHORT v;

    RtlWriteReg32(A, R_PHYAR, ((Reg & 0x1F) << PHYAR_REG_SHIFT));

    while (!(RtlReadReg32(A, R_PHYAR) & PHYAR_FLAG) && waited < 500)
    {
        NdisStallExecution(25);
        waited += 25;
    }

    v = (USHORT)(RtlReadReg32(A, R_PHYAR) & PHYAR_DATA_MASK);
    NdisStallExecution(20);
    return v;
}

/* On 8168G+ MDIO writes are redirected through the GPHY OCP block.
 * Writing reg 0x1F (page select) updates an internal base; subsequent
 * accesses to reg 0x10..0x1E get offset relative to that page. */
static VOID
RtlMdioWriteOcp(IN PRTL_ADAPTER A, IN ULONG Reg, IN USHORT Val)
{
    ULONG addr;

    if (Reg == 0x1F)
    {
        A->OcpBase = Val ? (USHORT)(Val << 4) : OCP_STD_PHY_BASE;
        return;
    }

    addr = A->OcpBase;
    if (addr != OCP_STD_PHY_BASE)
        Reg -= 0x10;

    RtlPhyOcpWrite(A, addr + Reg * 2, Val);
}

static USHORT
RtlMdioReadOcp(IN PRTL_ADAPTER A, IN ULONG Reg)
{
    ULONG addr;

    if (Reg == 0x1F)
        return A->OcpBase == OCP_STD_PHY_BASE ? 0 : (USHORT)(A->OcpBase >> 4);

    addr = A->OcpBase;
    if (addr != OCP_STD_PHY_BASE)
        Reg -= 0x10;

    return RtlPhyOcpRead(A, addr + Reg * 2);
}

VOID
NTAPI
RtlMdioWrite (
    IN PRTL_ADAPTER A,
    IN ULONG Reg,
    IN USHORT Val
    )
{
    if (A->OcpMdioRedirect)
        RtlMdioWriteOcp(A, Reg, Val);
    else
        RtlMdioWriteDirect(A, Reg, Val);
}

USHORT
NTAPI
RtlMdioRead (
    IN PRTL_ADAPTER A,
    IN ULONG Reg
    )
{
    if (A->OcpMdioRedirect)
        return RtlMdioReadOcp(A, Reg);
    return RtlMdioReadDirect(A, Reg);
}

/* PHY page-switched access -- writes 0x1F=page, accesses reg, writes 0x1F=0. */
VOID
NTAPI
RtlPhyWritePaged (
    IN PRTL_ADAPTER A,
    IN USHORT Page,
    IN ULONG Reg,
    IN USHORT Val
    )
{
    RtlMdioWrite(A, 0x1F, Page);
    RtlMdioWrite(A, Reg, Val);
    RtlMdioWrite(A, 0x1F, 0);
}

USHORT
NTAPI
RtlPhyReadPaged (
    IN PRTL_ADAPTER A,
    IN USHORT Page,
    IN ULONG Reg
    )
{
    USHORT v;
    RtlMdioWrite(A, 0x1F, Page);
    v = RtlMdioRead(A, Reg);
    RtlMdioWrite(A, 0x1F, 0);
    return v;
}

VOID
NTAPI
RtlPhyModifyPaged (
    IN PRTL_ADAPTER A,
    IN USHORT Page,
    IN ULONG Reg,
    IN USHORT Mask,
    IN USHORT Set
    )
{
    USHORT v;
    RtlMdioWrite(A, 0x1F, Page);
    v = RtlMdioRead(A, Reg);
    RtlMdioWrite(A, Reg, (USHORT)((v & ~Mask) | Set));
    RtlMdioWrite(A, 0x1F, 0);
}
