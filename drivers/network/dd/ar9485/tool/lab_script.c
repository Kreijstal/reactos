/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     User-mode bring-up lab: .lab script parser and program encoder.
 *
 * One statement per line.  '#' and ';' start a comment.  Numbers accept 0x
 * prefixes.  The grammar is intentionally dull: this file must never become
 * the place where bring-up logic accumulates -- that belongs in the scripts.
 */

#include "lab_script.h"

#include <stdlib.h>
#include <string.h>

/* The script currently being parsed.  Both consumers are single-threaded
 * command-line tools, so a file-static beats threading a parameter through
 * every rule. */
static LAB_SCRIPT *g_Script;

const char *
LabScriptStatusName(ULONG Status)
{
    switch (Status)
    {
        case AR9485LAB_ST_OK:           return "OK";
        case AR9485LAB_ST_BAD_OPCODE:   return "BAD_OPCODE";
        case AR9485LAB_ST_BAD_OFFSET:   return "BAD_OFFSET";
        case AR9485LAB_ST_BAD_LENGTH:   return "BAD_LENGTH";
        case AR9485LAB_ST_BAD_INDEX:    return "BAD_INDEX";
        case AR9485LAB_ST_TIMEOUT:      return "TIMEOUT";
        case AR9485LAB_ST_NO_RESOURCES: return "NO_RESOURCES";
        case AR9485LAB_ST_LIMIT:        return "LIMIT";
        case AR9485LAB_ST_NOT_CLAIMED:  return "NOT_CLAIMED";
        case AR9485LAB_ST_RESULT_FULL:  return "RESULT_FULL";
        case AR9485LAB_ST_UNSUPPORTED:  return "UNSUPPORTED";
        default:                        return "?";
    }
}

const char *
LabScriptOpName(ULONG Op)
{
    switch (Op)
    {
        case AR9485LAB_OP_READ32:    return "READ32";
        case AR9485LAB_OP_WRITE32:   return "WRITE32";
        case AR9485LAB_OP_RMW32:     return "RMW32";
        case AR9485LAB_OP_POLL:      return "POLL";
        case AR9485LAB_OP_DELAY_US:  return "DELAY";
        case AR9485LAB_OP_DUMP:      return "DUMP";
        case AR9485LAB_OP_MARK:      return "MARK";
        case AR9485LAB_OP_BARRIER:   return "BARRIER";
        case AR9485LAB_OP_PCI_READ:  return "PCIREAD";
        case AR9485LAB_OP_PCI_WRITE: return "PCIWRITE";
        case AR9485LAB_OP_DMA_LIST:  return "DMALIST";
        case AR9485LAB_OP_DMA_ALLOC: return "DMAALLOC";
        case AR9485LAB_OP_DMA_FREE:  return "DMAFREE";
        case AR9485LAB_OP_DMA_READ:  return "DMAREAD";
        case AR9485LAB_OP_DMA_WRITE: return "DMAWRITE";
        case AR9485LAB_OP_DMA_ZERO:  return "DMAZERO";
        case AR9485LAB_OP_CALL_HW:   return "CALLHW";
        case AR9485LAB_OP_WRITE_DMA_PA: return "WRITEDMAPA";
        case AR9485LAB_OP_DMA_POKE_PA:  return "DMAPOKEPA";
        default:                     return "OP?";
    }
}

static int
Emit(ULONG Op, ULONG A, ULONG B, ULONG C, ULONG D)
{
    if (g_Script->OpCount >= g_Script->OpCapacity)
    {
        fprintf(stderr, "ar9485lab: more than %lu ops\n",
                (unsigned long)g_Script->OpCapacity);
        return 0;
    }

    g_Script->Ops[g_Script->OpCount].Op = Op;
    g_Script->Ops[g_Script->OpCount].A = A;
    g_Script->Ops[g_Script->OpCount].B = B;
    g_Script->Ops[g_Script->OpCount].C = C;
    g_Script->Ops[g_Script->OpCount].D = D;
    ++g_Script->OpCount;
    return 1;
}

static int
ParseNumber(const char *Text, ULONG *Value)
{
    char *End = NULL;
    unsigned long Parsed;

    if (Text == NULL || *Text == '\0')
        return 0;

    Parsed = strtoul(Text, &End, 0);
    if (End == Text || (End != NULL && *End != '\0'))
        return 0;

    *Value = (ULONG)Parsed;
    return 1;
}

static int
HexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* dmawrite <index> <offset> <hex bytes...> -- the bytes go into the program's
 * data blob and the op references them by offset. */
static int
ParseDmaWrite(char **Tokens, int Count)
{
    ULONG Index, Offset;
    ULONG Start = g_Script->DataUsed;
    int i;

    if (Count < 4)
        return 0;
    if (!ParseNumber(Tokens[1], &Index) || !ParseNumber(Tokens[2], &Offset))
        return 0;

    for (i = 3; i < Count; ++i)
    {
        const char *Text = Tokens[i];
        size_t Length = strlen(Text);
        size_t j;

        if ((Length % 2) != 0)
        {
            fprintf(stderr, "ar9485lab: odd hex run '%s'\n", Text);
            return 0;
        }

        for (j = 0; j < Length; j += 2)
        {
            int Hi = HexNibble(Text[j]);
            int Lo = HexNibble(Text[j + 1]);

            if (Hi < 0 || Lo < 0)
            {
                fprintf(stderr, "ar9485lab: bad hex '%s'\n", Text);
                return 0;
            }
            if (g_Script->DataUsed >= LAB_SCRIPT_MAX_DATA)
            {
                fprintf(stderr, "ar9485lab: data blob overflow\n");
                return 0;
            }
            g_Script->Data[g_Script->DataUsed++] = (unsigned char)((Hi << 4) | Lo);
        }
    }

    return Emit(AR9485LAB_OP_DMA_WRITE, Index, Offset, g_Script->DataUsed - Start, Start);
}

static int
ParseCallHw(char **Tokens, int Count)
{
    ULONG Function, Argument = 0;

    if (Count < 2)
        return 0;

    if (strcmp(Tokens[1], "start") == 0)
        Function = AR9485LAB_HW_START;
    else if (strcmp(Tokens[1], "queuerx") == 0)
        Function = AR9485LAB_HW_QUEUE_RX;
    else if (strcmp(Tokens[1], "harvest") == 0)
        Function = AR9485LAB_HW_HARVEST;
    else if (strcmp(Tokens[1], "log") == 0)
        Function = AR9485LAB_HW_LOG;
    else if (strcmp(Tokens[1], "state") == 0)
        Function = AR9485LAB_HW_STATE;
    else if (!ParseNumber(Tokens[1], &Function))
        return 0;

    if (Count >= 3 && !ParseNumber(Tokens[2], &Argument))
        return 0;

    return Emit(AR9485LAB_OP_CALL_HW, Function, Argument, 0, 0);
}

static int
ParseLine(char *Line, int LineNumber)
{
    char *Tokens[64];
    int Count = 0;
    char *Cursor = Line;
    ULONG A = 0, B = 0, C = 0, D = 0;
    const char *Verb;

    /* Strip comments first so '#' inside a line ends it. */
    for (Cursor = Line; *Cursor != '\0'; ++Cursor)
    {
        if (*Cursor == '#' || *Cursor == ';')
        {
            *Cursor = '\0';
            break;
        }
    }

    Cursor = strtok(Line, " \t\r\n");
    while (Cursor != NULL && Count < 64)
    {
        Tokens[Count++] = Cursor;
        Cursor = strtok(NULL, " \t\r\n");
    }

    if (Count == 0)
        return 1;

    Verb = Tokens[0];

    if (strcmp(Verb, "r") == 0 || strcmp(Verb, "read") == 0)
    {
        if (Count != 2 || !ParseNumber(Tokens[1], &A)) goto Bad;
        return Emit(AR9485LAB_OP_READ32, A, 0, 0, 0);
    }
    if (strcmp(Verb, "w") == 0 || strcmp(Verb, "write") == 0)
    {
        if (Count != 3 || !ParseNumber(Tokens[1], &A) ||
            !ParseNumber(Tokens[2], &B)) goto Bad;
        return Emit(AR9485LAB_OP_WRITE32, A, B, 0, 0);
    }
    if (strcmp(Verb, "rmw") == 0)
    {
        if (Count != 4 || !ParseNumber(Tokens[1], &A) ||
            !ParseNumber(Tokens[2], &B) || !ParseNumber(Tokens[3], &C)) goto Bad;
        return Emit(AR9485LAB_OP_RMW32, A, B, C, 0);
    }
    if (strcmp(Verb, "poll") == 0)
    {
        if (Count != 5 || !ParseNumber(Tokens[1], &A) ||
            !ParseNumber(Tokens[2], &B) || !ParseNumber(Tokens[3], &C) ||
            !ParseNumber(Tokens[4], &D)) goto Bad;
        return Emit(AR9485LAB_OP_POLL, A, B, C, D);
    }
    if (strcmp(Verb, "delay") == 0)
    {
        if (Count != 2 || !ParseNumber(Tokens[1], &A)) goto Bad;
        return Emit(AR9485LAB_OP_DELAY_US, A, 0, 0, 0);
    }
    if (strcmp(Verb, "dump") == 0)
    {
        if (Count != 3 || !ParseNumber(Tokens[1], &A) ||
            !ParseNumber(Tokens[2], &B)) goto Bad;
        return Emit(AR9485LAB_OP_DUMP, A, B, 0, 0);
    }
    if (strcmp(Verb, "mark") == 0)
    {
        if (Count != 2 || !ParseNumber(Tokens[1], &A)) goto Bad;
        return Emit(AR9485LAB_OP_MARK, A, 0, 0, 0);
    }
    if (strcmp(Verb, "continue") == 0)
    {
        /* Keep going past a failing op.  Only for scripts that are probing
         * the interpreter itself -- a reset sequence whose third write was
         * rejected has produced meaningless results from there on. */
        if (Count != 1) goto Bad;
        g_Script->Flags |= AR9485LAB_PROGRAM_CONTINUE_ON_ERROR;
        return 1;
    }
    if (strcmp(Verb, "barrier") == 0)
    {
        if (Count != 1) goto Bad;
        return Emit(AR9485LAB_OP_BARRIER, 0, 0, 0, 0);
    }
    if (strcmp(Verb, "pciread") == 0)
    {
        if (Count != 3 || !ParseNumber(Tokens[1], &A) ||
            !ParseNumber(Tokens[2], &B)) goto Bad;
        return Emit(AR9485LAB_OP_PCI_READ, A, B, 0, 0);
    }
    if (strcmp(Verb, "pciwrite") == 0)
    {
        if (Count != 4 || !ParseNumber(Tokens[1], &A) ||
            !ParseNumber(Tokens[2], &B) || !ParseNumber(Tokens[3], &C)) goto Bad;
        return Emit(AR9485LAB_OP_PCI_WRITE, A, B, C, 0);
    }
    if (strcmp(Verb, "dmalist") == 0)
    {
        if (Count != 1) goto Bad;
        return Emit(AR9485LAB_OP_DMA_LIST, 0, 0, 0, 0);
    }
    if (strcmp(Verb, "dmaalloc") == 0)
    {
        if (Count != 3 || !ParseNumber(Tokens[1], &A) ||
            !ParseNumber(Tokens[2], &B)) goto Bad;
        return Emit(AR9485LAB_OP_DMA_ALLOC, A, B, 0, 0);
    }
    if (strcmp(Verb, "dmafree") == 0)
    {
        if (Count != 1) goto Bad;
        return Emit(AR9485LAB_OP_DMA_FREE, AR9485LAB_DMA_ALL, 0, 0, 0);
    }
    if (strcmp(Verb, "dmaread") == 0)
    {
        if (Count != 4 || !ParseNumber(Tokens[1], &A) ||
            !ParseNumber(Tokens[2], &B) || !ParseNumber(Tokens[3], &C)) goto Bad;
        return Emit(AR9485LAB_OP_DMA_READ, A, B, C, 0);
    }
    if (strcmp(Verb, "dmazero") == 0)
    {
        if (Count != 4 || !ParseNumber(Tokens[1], &A) ||
            !ParseNumber(Tokens[2], &B) || !ParseNumber(Tokens[3], &C)) goto Bad;
        return Emit(AR9485LAB_OP_DMA_ZERO, A, B, C, 0);
    }
    if (strcmp(Verb, "dmawrite") == 0)
    {
        if (!ParseDmaWrite(Tokens, Count)) goto Bad;
        return 1;
    }
    if (strcmp(Verb, "wdmapa") == 0)
    {
        if (Count != 4 || !ParseNumber(Tokens[1], &A) ||
            !ParseNumber(Tokens[2], &B) || !ParseNumber(Tokens[3], &C)) goto Bad;
        return Emit(AR9485LAB_OP_WRITE_DMA_PA, A, B, C, 0);
    }
    if (strcmp(Verb, "dmapokepa") == 0)
    {
        if (Count != 5 || !ParseNumber(Tokens[1], &A) ||
            !ParseNumber(Tokens[2], &B) || !ParseNumber(Tokens[3], &C) ||
            !ParseNumber(Tokens[4], &D)) goto Bad;
        return Emit(AR9485LAB_OP_DMA_POKE_PA, A, B, C, D);
    }
    if (strcmp(Verb, "callhw") == 0)
    {
        if (!ParseCallHw(Tokens, Count)) goto Bad;
        return 1;
    }

Bad:
    fprintf(stderr, "ar9485lab: line %d: cannot parse '%s'\n", LineNumber, Verb);
    return 0;
}

static int
ParseStream(FILE *Stream)
{
    char Line[1024];
    int LineNumber = 0;

    while (fgets(Line, sizeof(Line), Stream) != NULL)
    {
        ++LineNumber;
        if (!ParseLine(Line, LineNumber))
            return 0;
    }

    return 1;
}


int
LabScriptParse(FILE *Stream, LAB_SCRIPT *Script)
{
    g_Script = Script;
    return ParseStream(Stream);
}

ULONG
LabScriptEncodedSize(const LAB_SCRIPT *Script)
{
    return (ULONG)(sizeof(AR9485LAB_PROGRAM) +
                   Script->OpCount * sizeof(AR9485LAB_OP) + Script->DataUsed);
}

ULONG
LabScriptEncode(const LAB_SCRIPT *Script, unsigned char *Buffer,
                ULONG BufferLength)
{
    AR9485LAB_PROGRAM *Program;
    ULONG Needed = LabScriptEncodedSize(Script);

    if (Script->OpCount == 0 || BufferLength < Needed)
        return 0;

    memset(Buffer, 0, Needed);
    Program = (AR9485LAB_PROGRAM *)Buffer;
    Program->AbiVersion = AR9485LAB_ABI_VERSION;
    Program->OpCount = Script->OpCount;
    Program->DataOffset = (ULONG)(sizeof(AR9485LAB_PROGRAM) +
                                  Script->OpCount * sizeof(AR9485LAB_OP));
    Program->DataLength = Script->DataUsed;
    Program->Flags = Script->Flags;

    memcpy(Buffer + sizeof(AR9485LAB_PROGRAM), Script->Ops,
           Script->OpCount * sizeof(AR9485LAB_OP));
    if (Script->DataUsed != 0)
        memcpy(Buffer + Program->DataOffset, Script->Data, Script->DataUsed);

    return Needed;
}
