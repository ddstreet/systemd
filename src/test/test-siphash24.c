/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2015 Tom Gundersen

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include "util.h"
#include "siphash24.h"

#define ITERATIONS 10000000ULL

/* see https://131002.net/siphash/siphash.pdf, Appendix A */
int main(int argc, char *argv[]) {
        struct siphash state = {};
        const uint8_t in[15]  = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e };
        const uint8_t key[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
        uint64_t out = 0;
        unsigned i, j;

        siphash24((uint8_t *)&out, in, sizeof(in), key);
        assert_se(out == htole64(0xa129ca6149be45e5));

        /* verify the internal state as given in the above paper */
        siphash24_init(&state, key);
        assert_se(state.v0 == 0x7469686173716475);
        assert_se(state.v1 == 0x6b617f6d656e6665);
        assert_se(state.v2 == 0x6b7f62616d677361);
        assert_se(state.v3 == 0x7b6b696e727e6c7b);
        siphash24_compress(in, sizeof(in), &state);
        assert_se(state.v0 == 0x4a017198de0a59e0);
        assert_se(state.v1 == 0x0d52f6f62a4f59a4);
        assert_se(state.v2 == 0x634cb3577b01fd3d);
        assert_se(state.v3 == 0xa5224d6f55c7d9c8);
        siphash24_finalize((uint8_t*)&out, &state);
        assert_se(out == htole64(0xa129ca6149be45e5));
        assert_se(state.v0 == 0xf6bcd53893fecff1);
        assert_se(state.v1 == 0x54b9964c7ea0d937);
        assert_se(state.v2 == 0x1b38329c099bb55a);
        assert_se(state.v3 == 0x1814bb89ad7be679);

        /* verify that decomposing the input in three chunks gives the
           same result */
        for (i = 0; i < sizeof(in); i++) {
                for (j = i; j < sizeof(in); j++) {
                        siphash24_init(&state, key);
                        siphash24_compress(in, i, &state);
                        siphash24_compress(&in[i], j - i, &state);
                        siphash24_compress(&in[j], sizeof(in) - j, &state);
                        siphash24_finalize((uint8_t*)&out, &state);
                        assert_se(out == htole64(0xa129ca6149be45e5));
                }
        }
}
