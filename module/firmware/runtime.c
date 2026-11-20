/* SPDX-License-Identifier: GPL-3.0-or-later
 * Freestanding byte primitives for the shared native EXT2 engine.
 * No libc imports, Linux kernel services, FUSE or EFI filesystem drivers.
 */
#include <stddef.h>
#include <string.h>
void *memcpy(void *restrict destination, const void *restrict source, size_t count)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < count; ++i) out[i] = in[i];
    return destination;
}
void *memset(void *destination, int value, size_t count)
{
    unsigned char *out = destination;
    for (size_t i = 0; i < count; ++i) out[i] = (unsigned char)value;
    return destination;
}
