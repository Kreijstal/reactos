#include <precomp.h>

static int
wide_digit_value(wchar_t c)
{
  static const wchar_t zeroes[] =
  {
    0x0660, 0x06f0, 0x0966, 0x09e6, 0x0a66, 0x0ae6, 0x0b66,
    0x0c66, 0x0ce6, 0x0d66, 0x0e50, 0x0ed0, 0x0f20, 0x1040,
    0x17e0, 0x1810, 0xff10
  };
  unsigned int i;

  if (c >= L'0' && c <= L'9') return c - L'0';
  for (i = 0; i < sizeof(zeroes) / sizeof(zeroes[0]); i++)
    if (c >= zeroes[i] && c <= zeroes[i] + 9) return c - zeroes[i];
  return -1;
}

/*
 * Convert a unicode string to an unsigned long integer.
 *
 * Ignores `locale' stuff.  Assumes that the upper and lower case
 * alphabets and digits are each contiguous.
 *
 * @implemented
 */
unsigned long
CDECL
wcstoul(const wchar_t *nptr, wchar_t **endptr, int base)
{
  const wchar_t *s = nptr;
  unsigned long acc;
  int c;
  unsigned long cutoff;
  int neg = 0, any, cutlim;

  /*
   * See strtol for comments as to the logic used.
   */
  do {
    c = *s++;
  } while (iswctype(c, _SPACE));
  if (c == L'-')
  {
    neg = 1;
    c = *s++;
  }
  else if (c == L'+')
    c = *s++;
  if ((base == 0 || base == 16) &&
      wide_digit_value((wchar_t)c) == 0 && (*s == L'x' || *s == L'X'))
  {
    c = s[1];
    s += 2;
    base = 16;
  }
  if (base == 0)
    base = wide_digit_value((wchar_t)c) == 0 ? 8 : 10;
  cutoff = (unsigned long)ULONG_MAX / (unsigned long)base;
  cutlim = (unsigned long)ULONG_MAX % (unsigned long)base;
  for (acc = 0, any = 0;; c = *s++)
  {
    int digit = wide_digit_value((wchar_t)c);
    if (digit >= 0)
      c = digit;
    else if (iswctype(c, _ALPHA))
      c -= iswctype(c, _UPPER) ? L'A' - 10 : L'a' - 10;
    else
      break;
    if (c >= base)
      break;
    if (any < 0 || acc > cutoff || (acc == cutoff && c > cutlim))
      any = -1;
    else {
      any = 1;
      acc *= base;
      acc += c;
    }
  }
  if (any < 0)
  {
    acc = ULONG_MAX;
  }
  if (neg)
    acc = 0-acc;
  if (endptr != 0)
    *endptr = any ? (wchar_t *)((size_t)(s - 1)) : (wchar_t *)((size_t)nptr);
  return acc;
}
