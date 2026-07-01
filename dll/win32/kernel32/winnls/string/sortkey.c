/*
 * Unicode sort key generation
 *
 * Copyright 2003 Dmitry Timoshkov
 * Copyright 2019 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stddef.h>
#include <k32.h>
#include "wine/unicode.h"

extern unsigned char K32sortdefault[];

struct sortguid
{
    GUID  id;
    UINT  flags;
    UINT  compr;
    UINT  except;
    UINT  ling_except;
    UINT  casemap;
};

#define FLAG_HAS_3_BYTE_WEIGHTS 0x01
#define FLAG_REVERSEDIACRITICS  0x10
#define FLAG_DOUBLECOMPRESSION  0x20
#define FLAG_INVERSECASING      0x40

#define SORT_INDEX_VIETNAMESE   47

struct sort_expansion
{
    WCHAR exp[2];
};

struct jamo_sort
{
    BYTE is_old;
    BYTE leading;
    BYTE vowel;
    BYTE trailing;
    BYTE weight;
    BYTE pad[3];
};

struct sort_compression
{
    UINT  offset;
    WCHAR minchar, maxchar;
    WORD  len[8];
};

static inline int compression_size(int len)
{
    return 2 + len + (len & 1);
}

union char_weights
{
    UINT val;
    struct { BYTE primary, script, diacritic, _case; };
};

#define CASE_FULLWIDTH   0x01
#define CASE_FULLSIZE    0x02
#define CASE_SUBSCRIPT   0x08
#define CASE_UPPER       0x10
#define CASE_KATAKANA    0x20
#define CASE_COMPR_2     0x40
#define CASE_COMPR_4     0x80
#define CASE_COMPR_6     0xc0

enum sortkey_script
{
    SCRIPT_UNSORTABLE = 0,
    SCRIPT_NONSPACE_MARK = 1,
    SCRIPT_EXPANSION = 2,
    SCRIPT_EASTASIA_SPECIAL = 3,
    SCRIPT_JAMO_SPECIAL = 4,
    SCRIPT_EXTENSION_A = 5,
    SCRIPT_PUNCTUATION = 6,
    SCRIPT_SYMBOL_1 = 7,
    SCRIPT_SYMBOL_2 = 8,
    SCRIPT_SYMBOL_3 = 9,
    SCRIPT_SYMBOL_4 = 10,
    SCRIPT_SYMBOL_5 = 11,
    SCRIPT_SYMBOL_6 = 12,
    SCRIPT_DIGIT = 13,
    SCRIPT_LATIN = 14,
    SCRIPT_GREEK = 15,
    SCRIPT_CYRILLIC = 16,
    SCRIPT_KANA = 34,
    SCRIPT_HEBREW = 40,
    SCRIPT_ARABIC = 41,
    SCRIPT_PUA_FIRST = 169,
    SCRIPT_PUA_LAST = 175,
    SCRIPT_CJK_FIRST = 192,
    SCRIPT_CJK_LAST = 239,
};

static struct
{
    UINT                           version;
    UINT                           guid_count;
    UINT                           exp_count;
    UINT                           compr_count;
    const UINT                    *keys;
    const struct sortguid         *guids;
    const struct sort_expansion   *expansions;
    const struct sort_compression *compressions;
    const WCHAR                   *compr_data;
    const struct jamo_sort        *jamo;
} sort;

static BOOL sort_initialized;

static void load_sortdefault_nls(void)
{
    const struct
    {
        UINT sortkeys;
        UINT casemaps;
        UINT ctypes;
        UINT sortids;
    } *header = (const void *)K32sortdefault;
    const UINT *table;
    const struct sort_compression *last_compr;
    UINT i;

    if (sort_initialized)
        return;

    sort.keys = (const UINT *)((const char *)header + header->sortkeys);

    table = (const UINT *)((const char *)header + header->sortids);
    sort.version = table[0];
    sort.guid_count = table[1];
    sort.guids = (const struct sortguid *)(table + 2);

    table = (const UINT *)(sort.guids + sort.guid_count);
    sort.exp_count = table[0];
    sort.expansions = (const struct sort_expansion *)(table + 1);

    table = (const UINT *)(sort.expansions + sort.exp_count);
    sort.compr_count = table[0];
    sort.compressions = (const struct sort_compression *)(table + 1);
    sort.compr_data = (const WCHAR *)(sort.compressions + sort.compr_count);

    last_compr = sort.compressions + sort.compr_count - 1;
    table = (const UINT *)(sort.compr_data + last_compr->offset);
    for (i = 0; i < 7; i++)
        table += last_compr->len[i] * ((i + 5) / 2);
    table += 1 + table[0] / 2;
    sort.jamo = (const struct jamo_sort *)(table + 1);

    sort_initialized = TRUE;
}

static BOOL sort_locale_is(const WCHAR *locale, const char *name)
{
    while (*name)
    {
        WCHAR ch = *locale++;
        char expected = *name++;

        if (ch >= 'A' && ch <= 'Z')
            ch += 'a' - 'A';
        if (expected >= 'A' && expected <= 'Z')
            expected += 'a' - 'A';
        if (ch == '_')
            ch = '-';
        if (ch != (WCHAR)expected)
            return FALSE;
    }
    return !*locale;
}

static int get_sort_index_for_locale(const WCHAR *locale)
{
    if (!locale)
        return 0;

    if (sort_locale_is(locale, "ja-JP"))
        return 69;
    if (sort_locale_is(locale, "ko-KR"))
        return 54;
    if (sort_locale_is(locale, "zh-HK"))
        return 56;
    if (sort_locale_is(locale, "vi-VN"))
        return 47;
    if (sort_locale_is(locale, "rm-CH"))
        return 16;
    if (sort_locale_is(locale, "fr-FR"))
        return 2;
    if (sort_locale_is(locale, "cs-CZ"))
        return 27;
    if (sort_locale_is(locale, "tr-TR"))
        return 3;

    return 0;
}

static const struct sortguid *get_language_sort(const WCHAR *locale)
{
    int index;

    load_sortdefault_nls();
    index = get_sort_index_for_locale(locale);
    return ((UINT)index < sort.guid_count) ? &sort.guids[index] : &sort.guids[0];
}

static BOOL sortid_has_inverse_case(const struct sortguid *sortid)
{
    return (sortid->flags & FLAG_INVERSECASING) ||
           (sortid == &sort.guids[SORT_INDEX_VIETNAMESE]);
}

static void apply_case_sort_flags(const struct sortguid *sortid, union char_weights *weights)
{
    if (sortid_has_inverse_case(sortid) && weights->_case > 2)
        weights->_case = 0x3f - weights->_case;
}

static union char_weights get_char_weights(WCHAR c, UINT except)
{
    union char_weights ret;

    ret.val = except ? sort.keys[sort.keys[except + (c >> 8)] + (c & 0xff)] : sort.keys[c];
    return ret;
}

struct sortkey
{
    BYTE *buf;
    BYTE *new_buf;
    UINT  size;
    UINT  max;
    UINT  len;
};

static void append_sortkey(struct sortkey *key, BYTE val)
{
    if (key->len >= key->max)
        return;

    if (key->len >= key->size)
    {
        key->new_buf = RtlAllocateHeap(RtlGetProcessHeap(), 0, key->max);
        if (key->new_buf)
            memcpy(key->new_buf, key->buf, key->len);
        else
            key->max = 0;
        key->buf = key->new_buf;
        key->size = key->max;
    }
    key->buf[key->len++] = val;
}

static void reverse_sortkey(struct sortkey *key)
{
    int i;

    for (i = 0; i < key->len / 2; i++)
    {
        BYTE tmp = key->buf[key->len - i - 1];
        key->buf[key->len - i - 1] = key->buf[i];
        key->buf[i] = tmp;
    }
}

static int compare_sortkeys(const struct sortkey *key1, const struct sortkey *key2, BOOL shorter_wins)
{
    int ret = memcmp(key1->buf, key2->buf, min(key1->len, key2->len));

    if (!ret)
        ret = shorter_wins ? key2->len - key1->len : key1->len - key2->len;
    return ret;
}

static void append_normal_weights(const struct sortguid *sortid, struct sortkey *key_primary,
                                  struct sortkey *key_diacritic, struct sortkey *key_case,
                                  union char_weights weights, DWORD flags)
{
    append_sortkey(key_primary, weights.script);
    append_sortkey(key_primary, weights.primary);

    if ((weights.script >= SCRIPT_PUA_FIRST && weights.script <= SCRIPT_PUA_LAST) ||
        ((sortid->flags & FLAG_HAS_3_BYTE_WEIGHTS) &&
         (weights.script >= SCRIPT_CJK_FIRST && weights.script <= SCRIPT_CJK_LAST)))
    {
        append_sortkey(key_primary, weights.diacritic);
        append_sortkey(key_case, weights._case);
        return;
    }

    if (weights.script <= SCRIPT_ARABIC && weights.script != SCRIPT_HEBREW)
    {
        if (flags & LINGUISTIC_IGNOREDIACRITIC)
            weights.diacritic = 2;
        if (flags & LINGUISTIC_IGNORECASE)
            weights._case = 2;
    }
    append_sortkey(key_diacritic, weights.diacritic);
    append_sortkey(key_case, weights._case);
}

static void append_nonspace_weights(struct sortkey *key, union char_weights weights, DWORD flags)
{
    if (flags & LINGUISTIC_IGNOREDIACRITIC)
        weights.diacritic = 2;
    if (key->len)
        key->buf[key->len - 1] += weights.diacritic;
    else
        append_sortkey(key, weights.diacritic);
}

static void append_expansion_weights(const struct sortguid *sortid, struct sortkey *key_primary,
                                     struct sortkey *key_diacritic, struct sortkey *key_case,
                                     union char_weights weights, DWORD flags, BOOL is_compare)
{
    if (is_compare)
    {
        if (weights.script == SCRIPT_UNSORTABLE)
            return;
        if (weights.script == SCRIPT_NONSPACE_MARK)
        {
            append_nonspace_weights(key_diacritic, weights, flags);
            return;
        }
    }
    append_normal_weights(sortid, key_primary, key_diacritic, key_case, weights, flags);
}

static const UINT *find_compression(const WCHAR *src, const WCHAR *table, int count, int len)
{
    int elem_size = compression_size(len), min_pos = 0, max_pos = count - 1;

    while (min_pos <= max_pos)
    {
        int pos = (min_pos + max_pos) / 2;
        int res = strncmpW(src, table + pos * elem_size, len);

        if (!res)
            return (const UINT *)(table + (pos + 1) * elem_size) - 1;
        if (res > 0)
            min_pos = pos + 1;
        else
            max_pos = pos - 1;
    }
    return NULL;
}

static int get_compression_weights(UINT compression, const WCHAR *compr_tables[8],
                                   const WCHAR *src, int srclen, union char_weights *weights)
{
    const struct sort_compression *compr;
    const UINT *ret;
    BYTE size = weights->_case & CASE_COMPR_6;
    int i, maxlen = 1;

    if (compression >= sort.compr_count)
        return 0;

    compr = sort.compressions + compression;
    if (size == CASE_COMPR_6)
        maxlen = 8;
    else if (size == CASE_COMPR_4)
        maxlen = 5;
    else if (size == CASE_COMPR_2)
        maxlen = 3;

    maxlen = min(maxlen, srclen);
    for (i = 0; i < maxlen; i++)
        if (src[i] < compr->minchar || src[i] > compr->maxchar)
            break;
    maxlen = i;

    if (!compr_tables[0])
    {
        compr_tables[0] = sort.compr_data + compr->offset;
        for (i = 1; i < 8; i++)
            compr_tables[i] = compr_tables[i - 1] + compr->len[i - 1] * compression_size(i + 1);
    }

    for (i = maxlen - 2; i >= 0; i--)
    {
        if (!(ret = find_compression(src, compr_tables[i], compr->len[i], i + 2)))
            continue;
        weights->val = *ret;
        return i + 1;
    }
    return 0;
}

static WCHAR get_digit_zero_char(WCHAR ch)
{
    static const WCHAR zeroes[] =
    {
        0x0030, 0x0660, 0x06f0, 0x0966, 0x09e6, 0x0a66, 0x0ae6, 0x0b66, 0x0be6, 0x0c66,
        0x0ce6, 0x0d66, 0x0e50, 0x0ed0, 0x0f20, 0x1040, 0x1090, 0x17e0, 0x1810, 0x1946,
        0x1bb0, 0x1c40, 0x1c50, 0xa620, 0xa8d0, 0xa900, 0xaa50, 0xff10
    };
    int min_pos = 0, max_pos = ARRAYSIZE(zeroes) - 1;

    while (min_pos <= max_pos)
    {
        int pos = (min_pos + max_pos) / 2;

        if (zeroes[pos] <= ch && zeroes[pos] + 9 >= ch)
            return zeroes[pos];
        if (zeroes[pos] < ch)
            min_pos = pos + 1;
        else
            max_pos = pos - 1;
    }
    return 0;
}

static int append_digit_weights(struct sortkey *key, const WCHAR *src, UINT srclen)
{
    UINT i, zero, len, lzero;
    BYTE val, values[19];

    if (!(zero = get_digit_zero_char(*src)))
        return -1;

    values[0] = *src - zero;
    for (len = 1; len < ARRAYSIZE(values) && len < srclen; len++)
    {
        if (src[len] < zero || src[len] > zero + 9)
            break;
        values[len] = src[len] - zero;
    }
    for (lzero = 0; lzero < len; lzero++)
        if (values[lzero])
            break;

    append_sortkey(key, SCRIPT_DIGIT);
    append_sortkey(key, 2);
    append_sortkey(key, 2 + len - lzero);
    for (i = lzero, val = 2; i < len; i++)
    {
        if ((len - i) % 2)
            append_sortkey(key, (val << 4) + values[i] + 2);
        else
            val = values[i] + 2;
    }
    append_sortkey(key, 0xfe - lzero);
    return len - 1;
}

static int append_extra_kana_weights(struct sortkey keys[4], const WCHAR *src, int pos, UINT except,
                                     BYTE case_mask, union char_weights *weights)
{
    BYTE extra1 = 3, case_weight = weights->_case;

    if (weights->primary <= 1)
    {
        while (pos > 0)
        {
            union char_weights prev = get_char_weights(src[--pos], except);

            if (prev.script == SCRIPT_UNSORTABLE || prev.script == SCRIPT_NONSPACE_MARK)
                continue;
            if (prev.script == SCRIPT_EXPANSION)
                return 0;
            if (prev.script != SCRIPT_EASTASIA_SPECIAL)
            {
                *weights = prev;
                return 1;
            }
            if (prev.primary <= 1)
                continue;

            case_weight = prev._case & case_mask;
            if (weights->primary == 1)
            {
                prev.primary &= 0x87;
                case_weight &= ~CASE_FULLWIDTH;
                case_weight |= weights->_case & CASE_FULLWIDTH;
            }
            extra1 = 4 + weights->primary;
            weights->primary = prev.primary;
            goto done;
        }
        return 0;
    }

done:
    append_sortkey(&keys[0], 0xc4 | (case_weight & CASE_FULLSIZE));
    append_sortkey(&keys[1], extra1);
    append_sortkey(&keys[2], 0xc4 | (case_weight & CASE_KATAKANA));
    append_sortkey(&keys[3], 0xc4 | (case_weight & CASE_FULLWIDTH));
    weights->script = SCRIPT_KANA;
    return 1;
}

#define HANGUL_SBASE  0xac00
#define HANGUL_LCOUNT 19
#define HANGUL_VCOUNT 21
#define HANGUL_TCOUNT 28

static int append_hangul_weights(struct sortkey *key, const WCHAR *src, int srclen, UINT except)
{
    int leading_idx = 0x115f - 0x1100;
    int vowel_idx = 0x1160 - 0x1100;
    int trailing_idx = -1;
    BYTE leading_off, vowel_off, trailing_off;
    union char_weights weights;
    WCHAR composed;
    BYTE filler_mask = 0;
    int pos = 0;

    if (src[pos] >= 0x1100 && src[pos] <= 0x115f)
        leading_idx = src[pos++] - 0x1100;
    else if (src[pos] >= 0xa960 && src[pos] <= 0xa97c)
        leading_idx = src[pos++] - (0xa960 - 0x100);

    if (srclen > pos)
    {
        if (src[pos] >= 0x1160 && src[pos] <= 0x11a7)
            vowel_idx = src[pos++] - 0x1100;
        else if (src[pos] >= 0xd7b0 && src[pos] <= 0xd7c6)
            vowel_idx = src[pos++] - (0xd7b0 - 0x11d);
    }

    if (srclen > pos)
    {
        if (src[pos] >= 0x11a8 && src[pos] <= 0x11ff)
            trailing_idx = src[pos++] - 0x1100;
        else if (src[pos] >= 0xd7cb && src[pos] <= 0xd7fb)
            trailing_idx = src[pos++] - (0xd7cb - 0x134);
    }

    if (!sort.jamo[leading_idx].is_old && !sort.jamo[vowel_idx].is_old &&
        (trailing_idx == -1 || !sort.jamo[trailing_idx].is_old))
    {
        pos = 1;
        vowel_idx = 0x1160 - 0x1100;
        trailing_idx = -1;
    }

    leading_off = max(sort.jamo[leading_idx].leading, sort.jamo[vowel_idx].leading);
    vowel_off = max(sort.jamo[leading_idx].vowel, sort.jamo[vowel_idx].vowel);
    trailing_off = max(sort.jamo[leading_idx].trailing, sort.jamo[vowel_idx].trailing);
    if (trailing_idx != -1)
        trailing_off = max(trailing_off, sort.jamo[trailing_idx].trailing);

    composed = HANGUL_SBASE + (leading_off * HANGUL_VCOUNT + vowel_off) * HANGUL_TCOUNT + trailing_off;
    if (leading_idx == 0x115f - 0x1100 || vowel_idx == 0x1160 - 0x1100)
    {
        filler_mask = 0x80;
        composed--;
    }
    if (composed < HANGUL_SBASE)
        composed = 0x3260;

    weights = get_char_weights(composed, except);
    append_sortkey(key, weights.script);
    append_sortkey(key, weights.primary);
    append_sortkey(key, 0xff);
    append_sortkey(key, sort.jamo[leading_idx].weight | filler_mask);
    append_sortkey(key, 0xff);
    append_sortkey(key, sort.jamo[vowel_idx].weight);
    append_sortkey(key, 0xff);
    append_sortkey(key, trailing_idx != -1 ? sort.jamo[trailing_idx].weight : 2);
    return pos - 1;
}

static int put_sortkey(BYTE *dst, int dstlen, int pos, const struct sortkey *key, BYTE terminator)
{
    if (dstlen > pos + key->len)
    {
        memcpy(dst + pos, key->buf, key->len);
        dst[pos + key->len] = terminator;
    }
    return pos + key->len + 1;
}

struct sortkey_state
{
    struct sortkey key_primary;
    struct sortkey key_diacritic;
    struct sortkey key_case;
    struct sortkey key_special;
    struct sortkey key_extra[4];
    UINT primary_pos;
    BYTE buffer[3 * 128];
};

static void init_sortkey_state(struct sortkey_state *s, DWORD flags, UINT srclen,
                               BYTE *primary_buf, UINT primary_size)
{
    BYTE *secondary_buf = s->buffer;
    UINT secondary_size;

    memset(s, 0, offsetof(struct sortkey_state, buffer));

    s->key_primary.buf = primary_buf;
    s->key_primary.size = primary_size;

    if (!(flags & NORM_IGNORENONSPACE))
    {
        secondary_size = sizeof(s->buffer) / 3;
        s->key_diacritic.buf = secondary_buf;
        s->key_diacritic.size = secondary_size;
        secondary_buf += secondary_size;
    }
    else
    {
        secondary_size = sizeof(s->buffer) / 2;
    }

    s->key_case.buf = secondary_buf;
    s->key_case.size = secondary_size;
    s->key_special.buf = secondary_buf + secondary_size;
    s->key_special.size = secondary_size;

    s->key_primary.max = srclen * 8;
    s->key_case.max = srclen * 3;
    s->key_special.max = srclen * 4;
    s->key_extra[2].max = s->key_extra[3].max = srclen;
    if (!(flags & NORM_IGNORENONSPACE))
    {
        s->key_diacritic.max = srclen * 3;
        s->key_extra[0].max = s->key_extra[1].max = srclen;
    }
}

static BOOL remove_unneeded_weights(const struct sortguid *sortid, struct sortkey_state *s)
{
    const BYTE ignore[4] = { 0xc4 | CASE_FULLSIZE, 0x03, 0xc4 | CASE_KATAKANA, 0xc4 | CASE_FULLWIDTH };
    int i, j;

    if (sortid->flags & FLAG_REVERSEDIACRITICS)
        reverse_sortkey(&s->key_diacritic);

    for (i = s->key_diacritic.len; i > 0; i--)
        if (s->key_diacritic.buf[i - 1] > 2)
            break;
    s->key_diacritic.len = i;

    for (i = s->key_case.len; i > 0; i--)
        if (s->key_case.buf[i - 1] > 2)
            break;
    s->key_case.len = i;

    if (!s->key_extra[2].len)
        return FALSE;

    for (i = 0; i < 4; i++)
    {
        for (j = s->key_extra[i].len; j > 0; j--)
            if (s->key_extra[i].buf[j - 1] != ignore[i])
                break;
        s->key_extra[i].len = j;
    }
    return TRUE;
}

static void free_sortkey_state(struct sortkey_state *s)
{
    RtlFreeHeap(RtlGetProcessHeap(), 0, s->key_primary.new_buf);
    RtlFreeHeap(RtlGetProcessHeap(), 0, s->key_diacritic.new_buf);
    RtlFreeHeap(RtlGetProcessHeap(), 0, s->key_case.new_buf);
    RtlFreeHeap(RtlGetProcessHeap(), 0, s->key_special.new_buf);
    RtlFreeHeap(RtlGetProcessHeap(), 0, s->key_extra[0].new_buf);
    RtlFreeHeap(RtlGetProcessHeap(), 0, s->key_extra[1].new_buf);
    RtlFreeHeap(RtlGetProcessHeap(), 0, s->key_extra[2].new_buf);
    RtlFreeHeap(RtlGetProcessHeap(), 0, s->key_extra[3].new_buf);
}

static int append_weights(const struct sortguid *sortid, DWORD flags,
                          const WCHAR *src, int srclen, int pos, BYTE case_mask, UINT except,
                          const WCHAR *compr_tables[8], struct sortkey_state *s, BOOL is_compare)
{
    union char_weights weights = get_char_weights(src[pos], except);
    WCHAR idx = (weights.val >> 16) & ~(CASE_COMPR_6 << 8);
    int ret = 1;

    if (weights._case & CASE_COMPR_6)
        ret += get_compression_weights(sortid->compr, compr_tables, src + pos, srclen - pos, &weights);

    weights._case &= case_mask;
    apply_case_sort_flags(sortid, &weights);

    switch (weights.script)
    {
        case SCRIPT_UNSORTABLE:
            break;

        case SCRIPT_NONSPACE_MARK:
            append_nonspace_weights(&s->key_diacritic, weights, flags);
            break;

        case SCRIPT_EXPANSION:
            while (weights.script == SCRIPT_EXPANSION)
            {
                weights = get_char_weights(sort.expansions[idx].exp[0], except);
                weights._case &= case_mask;
                apply_case_sort_flags(sortid, &weights);
                append_expansion_weights(sortid, &s->key_primary, &s->key_diacritic,
                                         &s->key_case, weights, flags, is_compare);
                weights = get_char_weights(sort.expansions[idx].exp[1], except);
                idx = weights.val >> 16;
                weights._case &= case_mask;
                apply_case_sort_flags(sortid, &weights);
            }
            append_expansion_weights(sortid, &s->key_primary, &s->key_diacritic,
                                     &s->key_case, weights, flags, is_compare);
            break;

        case SCRIPT_EASTASIA_SPECIAL:
            if (!append_extra_kana_weights(s->key_extra, src, pos, except, case_mask, &weights))
            {
                append_sortkey(&s->key_primary, 0xff);
                append_sortkey(&s->key_primary, 0xff);
                break;
            }
            weights._case = 2;
            append_normal_weights(sortid, &s->key_primary, &s->key_diacritic, &s->key_case, weights, flags);
            break;

        case SCRIPT_JAMO_SPECIAL:
            ret += append_hangul_weights(&s->key_primary, src + pos, srclen - pos, except);
            append_sortkey(&s->key_diacritic, 2);
            append_sortkey(&s->key_case, 2);
            break;

        case SCRIPT_EXTENSION_A:
            append_sortkey(&s->key_primary, 0xfd);
            append_sortkey(&s->key_primary, 0xff);
            append_sortkey(&s->key_primary, weights.primary);
            append_sortkey(&s->key_primary, weights.diacritic);
            append_sortkey(&s->key_diacritic, 2);
            append_sortkey(&s->key_case, 2);
            break;

        case SCRIPT_PUNCTUATION:
            if (flags & NORM_IGNORESYMBOLS)
                break;
            if (!(flags & SORT_STRINGSORT))
            {
                short len = (short)(-((INT)(s->key_primary.len + s->primary_pos) / 2) - 1);

                if (flags & LINGUISTIC_IGNORECASE)
                    weights._case = 2;
                if (flags & LINGUISTIC_IGNOREDIACRITIC)
                    weights.diacritic = 2;
                append_sortkey(&s->key_special, len >> 8);
                append_sortkey(&s->key_special, len & 0xff);
                append_sortkey(&s->key_special, weights.primary);
                append_sortkey(&s->key_special, weights._case | (weights.diacritic << 3));
                break;
            }
            /* fall through */

        case SCRIPT_SYMBOL_1:
        case SCRIPT_SYMBOL_2:
        case SCRIPT_SYMBOL_3:
        case SCRIPT_SYMBOL_4:
        case SCRIPT_SYMBOL_5:
        case SCRIPT_SYMBOL_6:
            if (flags & NORM_IGNORESYMBOLS)
                break;
            append_sortkey(&s->key_primary, weights.script);
            append_sortkey(&s->key_primary, weights.primary);
            append_sortkey(&s->key_diacritic, weights.diacritic);
            append_sortkey(&s->key_case, weights._case);
            break;

        case SCRIPT_DIGIT:
            if (flags & SORT_DIGITSASNUMBERS)
            {
                int len = append_digit_weights(&s->key_primary, src + pos, srclen - pos);

                if (len >= 0)
                {
                    ret += len;
                    append_sortkey(&s->key_diacritic, weights.diacritic);
                    append_sortkey(&s->key_case, weights._case);
                    break;
                }
            }
            /* fall through */

        default:
            append_normal_weights(sortid, &s->key_primary, &s->key_diacritic, &s->key_case, weights, flags);
            break;
    }

    return ret;
}

static void map_byterev_sortkey(WCHAR *dst, int len)
{
    int i;

    for (i = 0; i < len; i++)
        dst[i] = (dst[i] << 8) | (dst[i] >> 8);
}

int wine_get_sortkey(const WCHAR *locale, int flags, const WCHAR *src, int srclen, char *dst, int dstlen)
{
    const struct sortguid *sortid = get_language_sort(locale);
    struct sortkey_state s;
    BYTE primary_buf[256];
    int ret = 0, pos = 0;
    BOOL have_extra;
    BYTE case_mask = 0x3f;
    UINT except;
    const WCHAR *compr_tables[8];

    if (!sortid)
        return 0;

    except = sortid->except;
    compr_tables[0] = NULL;
    if (flags & NORM_IGNORECASE)
        case_mask &= ~(CASE_UPPER | CASE_SUBSCRIPT);
    if (flags & NORM_IGNOREWIDTH)
        case_mask &= ~CASE_FULLWIDTH;
    if (flags & NORM_IGNOREKANATYPE)
        case_mask &= ~CASE_KATAKANA;
    if ((flags & NORM_LINGUISTIC_CASING) && except && sortid->ling_except)
        except = sortid->ling_except;

    init_sortkey_state(&s, flags, srclen, primary_buf, sizeof(primary_buf));

    while (pos < srclen)
        pos += append_weights(sortid, flags, src, srclen, pos, case_mask, except, compr_tables, &s, FALSE);

    have_extra = remove_unneeded_weights(sortid, &s);

    ret = put_sortkey((BYTE *)dst, dstlen, ret, &s.key_primary, 0x01);
    ret = put_sortkey((BYTE *)dst, dstlen, ret, &s.key_diacritic, 0x01);
    ret = put_sortkey((BYTE *)dst, dstlen, ret, &s.key_case, 0x01);

    if (have_extra)
    {
        ret = put_sortkey((BYTE *)dst, dstlen, ret, &s.key_extra[0], 0xff);
        ret = put_sortkey((BYTE *)dst, dstlen, ret, &s.key_extra[1], 0x02);
        ret = put_sortkey((BYTE *)dst, dstlen, ret, &s.key_extra[2], 0xff);
        ret = put_sortkey((BYTE *)dst, dstlen, ret, &s.key_extra[3], 0xff);
    }
    if (dstlen > ret)
        dst[ret] = 0x01;
    ret++;

    ret = put_sortkey((BYTE *)dst, dstlen, ret, &s.key_special, 0);

    free_sortkey_state(&s);

    if (dstlen && dstlen < ret)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    if (flags & LCMAP_BYTEREV)
        map_byterev_sortkey((WCHAR *)dst, min(ret, dstlen) / sizeof(WCHAR));

    return ret;
}

int wine_compare_string(const WCHAR *locale, int flags, const WCHAR *str1, int len1, const WCHAR *str2, int len2)
{
    const struct sortguid *sortid = get_language_sort(locale);
    struct sortkey_state s1, s2;
    BYTE primary1[32], primary2[32];
    int i, ret, len, pos1 = 0, pos2 = 0;
    BOOL have_extra1, have_extra2;
    BYTE case_mask = 0x3f;
    UINT except;
    const WCHAR *compr_tables[8];

    if (!sortid)
        return 0;

    except = sortid->except;
    compr_tables[0] = NULL;
    if (flags & NORM_IGNORECASE)
        case_mask &= ~(CASE_UPPER | CASE_SUBSCRIPT);
    if (flags & NORM_IGNOREWIDTH)
        case_mask &= ~CASE_FULLWIDTH;
    if (flags & NORM_IGNOREKANATYPE)
        case_mask &= ~CASE_KATAKANA;
    if ((flags & NORM_LINGUISTIC_CASING) && except && sortid->ling_except)
        except = sortid->ling_except;

    init_sortkey_state(&s1, flags, len1, primary1, sizeof(primary1));
    init_sortkey_state(&s2, flags, len2, primary2, sizeof(primary2));

    while (pos1 < len1 || pos2 < len2)
    {
        while (pos1 < len1 && !s1.key_primary.len)
            pos1 += append_weights(sortid, flags, str1, len1, pos1, case_mask, except, compr_tables, &s1, TRUE);

        while (pos2 < len2 && !s2.key_primary.len)
            pos2 += append_weights(sortid, flags, str2, len2, pos2, case_mask, except, compr_tables, &s2, TRUE);

        if (!(len = min(s1.key_primary.len, s2.key_primary.len)))
            break;
        if ((ret = memcmp(primary1, primary2, len)))
            goto done;
        memmove(primary1, primary1 + len, s1.key_primary.len - len);
        memmove(primary2, primary2 + len, s2.key_primary.len - len);
        s1.key_primary.len -= len;
        s2.key_primary.len -= len;
        s1.primary_pos += len;
        s2.primary_pos += len;
    }

    if ((ret = s1.key_primary.len - s2.key_primary.len))
        goto done;

    have_extra1 = remove_unneeded_weights(sortid, &s1);
    have_extra2 = remove_unneeded_weights(sortid, &s2);

    if ((ret = compare_sortkeys(&s1.key_diacritic, &s2.key_diacritic, FALSE)))
        goto done;
    if ((ret = compare_sortkeys(&s1.key_case, &s2.key_case, FALSE)))
        goto done;

    if (have_extra1 && have_extra2)
    {
        for (i = 0; i < 4; i++)
            if ((ret = compare_sortkeys(&s1.key_extra[i], &s2.key_extra[i], i != 1)))
                goto done;
    }
    else if ((ret = have_extra1 - have_extra2))
    {
        goto done;
    }

    ret = compare_sortkeys(&s1.key_special, &s2.key_special, FALSE);

done:
    free_sortkey_state(&s1);
    free_sortkey_state(&s2);
    return ret;
}
