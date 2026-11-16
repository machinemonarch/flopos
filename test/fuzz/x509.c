/* x509.c -- the certificate checker (src/kernel/x509.c, with the RSA and
 * ECDSA code behind it), given garbage certificates.
 *
 * An input is a chain as a site would send it: each certificate after a
 * 16-bit length, then a signature after one more.  The roots come from
 * the file named in FUZZ_ROOTS (kshim.c). */
#include "flopos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *fuzz_name = "x509";
const u32 fuzz_max = 16 * 1024;

void fuzz_one(const u8 *data, u32 len, u32 mode)
{
    static const int schemes[] = { 0x0401, 0x0501, 0x0601, 0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0203 };
    const u8 *certs[6];
    u8 *own[6];
    int lens[6], n = 0, i;
    char why[160];
    u32 at = 0;

    while (n < 6 && at + 2 <= len) {
        u32 l = (u32)(data[at] << 8 | data[at + 1]);
        at += 2;
        if (l > len - at) l = len - at;
        if (!l) break;
        own[n] = malloc(l);                      /* exact, so reading past it is caught */
        memcpy(own[n], data + at, l);
        certs[n] = own[n]; lens[n] = (int)l; n++;
        at += l;
    }
    if (n > 1) {                                 /* the last piece is the signature */
        int scheme = schemes[mode % (sizeof schemes / sizeof schemes[0])];
        x509_check((const u8 *const *)certs, lens, n - 1, (mode >> 8) & 1 ? "fuzz.test" : "other.test", why, (int)sizeof why);
        if ((mode >> 9) & 1) x509_check_handshake(scheme, data, (int)(len < 200 ? len : 200), certs[n - 1], lens[n - 1]);
        else x509_check_handshake12(scheme, data, (int)(len < 200 ? len : 200), certs[n - 1], lens[n - 1]);
    } else if (n == 1)
        x509_check((const u8 *const *)certs, lens, 1, "fuzz.test", why, (int)sizeof why);
    for (i = 0; i < n; i++) free(own[i]);
}
