/*
 * Copyright (c) 2018 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <cstdio>
#include <iostream>
#include <string>

#include "crypto.hh"

namespace ArmISA {

const uint8_t
Crypto::aesSBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
    0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
    0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
    0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
    0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
    0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
    0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
    0xb0, 0x54, 0xbb, 0x16
};

const uint8_t
Crypto::aesInvSBOX[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e,
    0x81, 0xf3, 0xd7, 0xfb, 0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87,
    0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb, 0x54, 0x7b, 0x94, 0x32,
    0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49,
    0x6d, 0x8b, 0xd1, 0x25, 0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16,
    0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92, 0x6c, 0x70, 0x48, 0x50,
    0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05,
    0xb8, 0xb3, 0x45, 0x06, 0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02,
    0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b, 0x3a, 0x91, 0x11, 0x41,
    0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8,
    0x1c, 0x75, 0xdf, 0x6e, 0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89,
    0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b, 0xfc, 0x56, 0x3e, 0x4b,
    0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59,
    0x27, 0x80, 0xec, 0x5f, 0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d,
    0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef, 0xa0, 0xe0, 0x3b, 0x4d,
    0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63,
    0x55, 0x21, 0x0c, 0x7d
};

const uint8_t
Crypto::aesFFLOG[256] = {
    0x00, 0x00, 0x19, 0x01, 0x32, 0x02, 0x1a, 0xc6, 0x4b, 0xc7, 0x1b, 0x68,
    0x33, 0xee, 0xdf, 0x03, 0x64, 0x04, 0xe0, 0x0e, 0x34, 0x8d, 0x81, 0xef,
    0x4c, 0x71, 0x08, 0xc8, 0xf8, 0x69, 0x1c, 0xc1, 0x7d, 0xc2, 0x1d, 0xb5,
    0xf9, 0xb9, 0x27, 0x6a, 0x4d, 0xe4, 0xa6, 0x72, 0x9a, 0xc9, 0x09, 0x78,
    0x65, 0x2f, 0x8a, 0x05, 0x21, 0x0f, 0xe1, 0x24, 0x12, 0xf0, 0x82, 0x45,
    0x35, 0x93, 0xda, 0x8e, 0x96, 0x8f, 0xdb, 0xbd, 0x36, 0xd0, 0xce, 0x94,
    0x13, 0x5c, 0xd2, 0xf1, 0x40, 0x46, 0x83, 0x38, 0x66, 0xdd, 0xfd, 0x30,
    0xbf, 0x06, 0x8b, 0x62, 0xb3, 0x25, 0xe2, 0x98, 0x22, 0x88, 0x91, 0x10,
    0x7e, 0x6e, 0x48, 0xc3, 0xa3, 0xb6, 0x1e, 0x42, 0x3a, 0x6b, 0x28, 0x54,
    0xfa, 0x85, 0x3d, 0xba, 0x2b, 0x79, 0x0a, 0x15, 0x9b, 0x9f, 0x5e, 0xca,
    0x4e, 0xd4, 0xac, 0xe5, 0xf3, 0x73, 0xa7, 0x57, 0xaf, 0x58, 0xa8, 0x50,
    0xf4, 0xea, 0xd6, 0x74, 0x4f, 0xae, 0xe9, 0xd5, 0xe7, 0xe6, 0xad, 0xe8,
    0x2c, 0xd7, 0x75, 0x7a, 0xeb, 0x16, 0x0b, 0xf5, 0x59, 0xcb, 0x5f, 0xb0,
    0x9c, 0xa9, 0x51, 0xa0, 0x7f, 0x0c, 0xf6, 0x6f, 0x17, 0xc4, 0x49, 0xec,
    0xd8, 0x43, 0x1f, 0x2d, 0xa4, 0x76, 0x7b, 0xb7, 0xcc, 0xbb, 0x3e, 0x5a,
    0xfb, 0x60, 0xb1, 0x86, 0x3b, 0x52, 0xa1, 0x6c, 0xaa, 0x55, 0x29, 0x9d,
    0x97, 0xb2, 0x87, 0x90, 0x61, 0xbe, 0xdc, 0xfc, 0xbc, 0x95, 0xcf, 0xcd,
    0x37, 0x3f, 0x5b, 0xd1, 0x53, 0x39, 0x84, 0x3c, 0x41, 0xa2, 0x6d, 0x47,
    0x14, 0x2a, 0x9e, 0x5d, 0x56, 0xf2, 0xd3, 0xab, 0x44, 0x11, 0x92, 0xd9,
    0x23, 0x20, 0x2e, 0x89, 0xb4, 0x7c, 0xb8, 0x26, 0x77, 0x99, 0xe3, 0xa5,
    0x67, 0x4a, 0xed, 0xde, 0xc5, 0x31, 0xfe, 0x18, 0x0d, 0x63, 0x8c, 0x80,
    0xc0, 0xf7, 0x70, 0x07
};

const uint8_t
Crypto::aesFFEXP[256] = {
    0x01, 0x03, 0x05, 0x0f, 0x11, 0x33, 0x55, 0xff, 0x1a, 0x2e, 0x72, 0x96,
    0xa1, 0xf8, 0x13, 0x35, 0x5f, 0xe1, 0x38, 0x48, 0xd8, 0x73, 0x95, 0xa4,
    0xf7, 0x02, 0x06, 0x0a, 0x1e, 0x22, 0x66, 0xaa, 0xe5, 0x34, 0x5c, 0xe4,
    0x37, 0x59, 0xeb, 0x26, 0x6a, 0xbe, 0xd9, 0x70, 0x90, 0xab, 0xe6, 0x31,
    0x53, 0xf5, 0x04, 0x0c, 0x14, 0x3c, 0x44, 0xcc, 0x4f, 0xd1, 0x68, 0xb8,
    0xd3, 0x6e, 0xb2, 0xcd, 0x4c, 0xd4, 0x67, 0xa9, 0xe0, 0x3b, 0x4d, 0xd7,
    0x62, 0xa6, 0xf1, 0x08, 0x18, 0x28, 0x78, 0x88, 0x83, 0x9e, 0xb9, 0xd0,
    0x6b, 0xbd, 0xdc, 0x7f, 0x81, 0x98, 0xb3, 0xce, 0x49, 0xdb, 0x76, 0x9a,
    0xb5, 0xc4, 0x57, 0xf9, 0x10, 0x30, 0x50, 0xf0, 0x0b, 0x1d, 0x27, 0x69,
    0xbb, 0xd6, 0x61, 0xa3, 0xfe, 0x19, 0x2b, 0x7d, 0x87, 0x92, 0xad, 0xec,
    0x2f, 0x71, 0x93, 0xae, 0xe9, 0x20, 0x60, 0xa0, 0xfb, 0x16, 0x3a, 0x4e,
    0xd2, 0x6d, 0xb7, 0xc2, 0x5d, 0xe7, 0x32, 0x56, 0xfa, 0x15, 0x3f, 0x41,
    0xc3, 0x5e, 0xe2, 0x3d, 0x47, 0xc9, 0x40, 0xc0, 0x5b, 0xed, 0x2c, 0x74,
    0x9c, 0xbf, 0xda, 0x75, 0x9f, 0xba, 0xd5, 0x64, 0xac, 0xef, 0x2a, 0x7e,
    0x82, 0x9d, 0xbc, 0xdf, 0x7a, 0x8e, 0x89, 0x80, 0x9b, 0xb6, 0xc1, 0x58,
    0xe8, 0x23, 0x65, 0xaf, 0xea, 0x25, 0x6f, 0xb1, 0xc8, 0x43, 0xc5, 0x54,
    0xfc, 0x1f, 0x21, 0x63, 0xa5, 0xf4, 0x07, 0x09, 0x1b, 0x2d, 0x77, 0x99,
    0xb0, 0xcb, 0x46, 0xca, 0x45, 0xcf, 0x4a, 0xde, 0x79, 0x8b, 0x86, 0x91,
    0xa8, 0xe3, 0x3e, 0x42, 0xc6, 0x51, 0xf3, 0x0e, 0x12, 0x36, 0x5a, 0xee,
    0x29, 0x7b, 0x8d, 0x8c, 0x8f, 0x8a, 0x85, 0x94, 0xa7, 0xf2, 0x0d, 0x17,
    0x39, 0x4b, 0xdd, 0x7c, 0x84, 0x97, 0xa2, 0xfd, 0x1c, 0x24, 0x6c, 0xb4,
    0xc7, 0x52, 0xf6, 0x01
};

const uint8_t
Crypto::aesSHIFT[16] = {
    0, 5, 10, 15, 4, 9, 14, 3,
    8, 13, 2, 7, 12, 1, 6, 11
};

const uint8_t
Crypto::aesINVSHIFT[16] = {
    0, 13, 10, 7, 4, 1, 14, 11,
    8, 5, 2, 15, 12, 9, 6, 3
};

uint8_t
Crypto::aesFFMul(uint8_t a, uint8_t b)
{
    unsigned int log_prod;

    if ((a ==0)|| (b == 0)) return 0;

    log_prod = (aesFFLOG[a] + aesFFLOG[b]);

    if (log_prod > 0xff)
        log_prod = log_prod - 0xff;

    return aesFFEXP[log_prod];
}

void
Crypto::aesSubBytes(uint8_t *output, uint8_t *input)
{
    for (int i = 0; i < 16; ++i) {
        output[i] = aesSBOX[input[i]];
    }
}

void
Crypto::aesInvSubBytes(uint8_t *output, uint8_t *input)
{
    for (int i = 0; i < 16; ++i) {
        output[i] = aesInvSBOX[input[i]];
    }
}

void
Crypto::aesShiftRows(uint8_t *output, uint8_t *input)
{
    for (int i = 0; i < 16; ++i) {
        output[i] = input[aesSHIFT[i]];
    }
}

void
Crypto::aesInvShiftRows(uint8_t *output, uint8_t *input)
{
    for (int i = 0; i < 16; ++i) {
        output[i] = input[aesINVSHIFT[i]];
    }
}

void
Crypto::aesAddRoundKey(uint8_t *output, uint8_t *input,
                    uint8_t *key)
{
    for (int i = 0; i < 16; ++i) {
        output[i] = input[i] ^ key[i];
    }
}

void
Crypto::aesMixColumns(uint8_t *output, uint8_t *input)
{
    for (int j = 0; j < 4; ++j) {
        int row0 = (j * 4);
        int row1 = row0 + 1;
        int row2 = row0 + 2;
        int row3 = row0 + 3;
        uint8_t t1 = input[row0] ^ input[row1] ^
                           input[row2] ^ input[row3];

        output[row1] = input[row1] ^ t1 ^ aesFFMul2(input[row1] ^ input[row2]);
        output[row2] = input[row2] ^ t1 ^ aesFFMul2(input[row2] ^ input[row3]);
        output[row3] = input[row3] ^ t1 ^ aesFFMul2(input[row3] ^ input[row0]);
        output[row0] = input[row0] ^ t1 ^ aesFFMul2(input[row0] ^ input[row1]);
    }
}

void
Crypto::aesInvMixColumns(uint8_t *output, uint8_t *input)
{
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            int index0 = (j * 4) + i;
            int index1 = (j * 4) + ((i + 1) % 4);
            int index2 = (j * 4) + ((i + 2) % 4);
            int index3 = (j * 4) + ((i + 3) % 4);
            output [index0] =
                aesFFMul(0x0e, input[index0]) ^ aesFFMul(0x0b, input[index1]) ^
                aesFFMul(0x0d, input[index2]) ^ aesFFMul(0x09, input[index3]);
            }
    }
}

void
Crypto::aesEncrypt(uint8_t *output, uint8_t *input,
                    uint8_t *key)
{
    uint8_t temp1[16];
    uint8_t temp2[16];
    aesAddRoundKey(&temp1[0], input, key);
    aesShiftRows(&temp2[0], &temp1[0]);
    aesSubBytes(output, &temp2[0]);
}

void
Crypto::aesDecrypt(uint8_t *output, uint8_t *input,
                    uint8_t *key)
{
    uint8_t temp1[16];
    uint8_t temp2[16];
    aesAddRoundKey(&temp1[0], input, key);
    aesInvShiftRows(&temp2[0], &temp1[0]);
    aesInvSubBytes(output, &temp2[0]);
}

void
Crypto::sha256Op(
    uint32_t *X,
    uint32_t *Y,
    uint32_t *Z)
{
    uint32_t T0, T1, T2, T3;
    for (int i = 0; i < 4; ++i) {
        T0 = choose(Y[0], Y[1], Y[2]);
        T1 = majority(X[0], X[1], X[2]);
        T2 = Y[3] + sigma1(Y[0]) + T0 + Z[i];
        X[3] = T2 + X[3];
        Y[3] = T2 + sigma0(X[0]) + T1;
        // Rotate
        T3 = Y[3];
        Y[3] = Y[2]; Y[2] = Y[1]; Y[1] = Y[0]; Y[0] = X[3];
        X[3] = X[2]; X[2] = X[1]; X[1] = X[0]; X[0] = T3;
    }
}

void
Crypto::_sha1Op(
    uint32_t *X,
    uint32_t *Y,
    uint32_t *Z,
    SHAOp op)
{
    uint32_t T1, T2;

    for (int i = 0; i < 4; ++i) {
        switch (op) {
          case CHOOSE:   T1 = choose(X[1], X[2], X[3]); break;
          case PARITY:   T1 = parity(X[1], X[2], X[3]); break;
          case MAJORITY: T1 = majority(X[1], X[2], X[3]); break;
          default: return;
        }
        Y[0] += ror(X[0], 27) + T1 + Z[i];
        X[1] = ror(X[1], 2);
        T2 = Y[0];
        Y[0] = X[3];
        X[3] = X[2]; X[2] = X[1]; X[1] = X[0]; X[0] = T2;
    }
}

void
Crypto::sha256H(
    uint8_t *output,
    uint8_t *input,
    uint8_t *input2)
{
    uint32_t X[4], Y[4], Z[4];
    load3Reg(&X[0], &Y[0], &Z[0], output, input, input2);
    sha256Op(&X[0], &Y[0], &Z[0]);
    store1Reg(output, &X[0]);
}

void
Crypto::sha256H2(
    uint8_t *output,
    uint8_t *input,
    uint8_t *input2)
{
    uint32_t X[4], Y[4], Z[4];
    load3Reg(&X[0], &Y[0], &Z[0], output, input, input2);
    sha256Op(&Y[0], &X[0], &Z[0]);
    store1Reg(output, &X[0]);
}

void
Crypto::sha256Su0(uint8_t *output, uint8_t *input)
{
    uint32_t X[4], Y[4];
    uint32_t T[4];

    load2Reg(&X[0], &Y[0], output, input);

    T[3] = Y[0]; T[2] = X[3]; T[1] = X[2]; T[0] = X[1];

    T[3] = ror(T[3], 7) ^ ror(T[3], 18) ^ (T[3] >> 3);
    T[2] = ror(T[2], 7) ^ ror(T[2], 18) ^ (T[2] >> 3);
    T[1] = ror(T[1], 7) ^ ror(T[1], 18) ^ (T[1] >> 3);
    T[0] = ror(T[0], 7) ^ ror(T[0], 18) ^ (T[0] >> 3);

    X[3] += T[3];
    X[2] += T[2];
    X[1] += T[1];
    X[0] += T[0];

    store1Reg(output, &X[0]);
}

void
Crypto::sha256Su1(
    uint8_t *output,
    uint8_t *input,
    uint8_t *input2)
{
    uint32_t X[4], Y[4], Z[4];
    uint32_t T0[4], T1[4], T2[4], T3[4];

    load3Reg(&X[0], &Y[0], &Z[0], output, input, input2);

    T0[3] = Z[0]; T0[2] = Y[3]; T0[1] = Y[2]; T0[0] = Y[1];
    T1[1] = Z[3]; T1[0] = Z[2];
    T1[1] = ror(T1[1], 17) ^ ror(T1[1], 19) ^ (T1[1] >> 10);
    T1[0] = ror(T1[0], 17) ^ ror(T1[0], 19) ^ (T1[0] >> 10);
    T3[1] = X[1] + T0[1]; T3[0] = X[0] + T0[0];
    T1[1] = T3[1] + T1[1]; T1[0] = T3[0] + T1[0];
    T2[1] = ror(T1[1], 17) ^ ror(T1[1], 19) ^ (T1[1] >> 10);
    T2[0] = ror(T1[0], 17) ^ ror(T1[0], 19) ^ (T1[0] >> 10);
    T3[1] = X[3] + T0[3]; T3[0] = X[2] + T0[2];
    X[3] = T3[1] + T2[1];
    X[2] = T3[0] + T2[0];
    X[1] = T1[1]; X[0] = T1[0];

    store1Reg(output, &X[0]);
}

void
Crypto::sha1Op(
    uint8_t *output,
    uint8_t *input,
    uint8_t *input2,
    SHAOp op)
{
    uint32_t X[4], Y[4], Z[4];
    load3Reg(&X[0], &Y[0], &Z[0], output, input, input2);
    _sha1Op(&X[0], &Y[0], &Z[0], op);
    store1Reg(output, &X[0]);
}

void
Crypto::sha1C(
    uint8_t *output,
    uint8_t *input,
    uint8_t *input2)
{
    sha1Op(output, input, input2, CHOOSE);
}

void
Crypto::sha1P(
    uint8_t *output,
    uint8_t *input,
    uint8_t *input2)
{
    sha1Op(output, input, input2, PARITY);
}

void
Crypto::sha1M(
    uint8_t *output,
    uint8_t *input,
    uint8_t *input2)
{
    sha1Op(output, input, input2, MAJORITY);
}

void
Crypto::sha1H(uint8_t *output, uint8_t *input)
{
    uint32_t X[4], Y[4];
    load2Reg(&X[0], &Y[0], output, input);
    X[0] = ror(Y[0], 2);
    store1Reg(output, &X[0]);
}

void
Crypto::sha1Su0(
    uint8_t *output,
    uint8_t *input,
    uint8_t *input2)
{
    uint32_t X[4], Y[4], Z[4], T[4];
    load3Reg(&X[0], &Y[0], &Z[0], output, input, input2);

    T[3] = Y[1]; T[2] = Y[0]; T[1] = X[3]; T[0] = X[2];
    X[3] = T[3] ^ X[3] ^ Z[3];
    X[2] = T[2] ^ X[2] ^ Z[2];
    X[1] = T[1] ^ X[1] ^ Z[1];
    X[0] = T[0] ^ X[0] ^ Z[0];

    store1Reg(output, &X[0]);
}

void
Crypto::sha1Su1(uint8_t *output, uint8_t *input)
{
    uint32_t X[4], Y[4], T[4];
    load2Reg(&X[0], &Y[0], output, input);

    T[3] = X[3] ^ 0x0;
    T[2] = X[2] ^ Y[3];
    T[1] = X[1] ^ Y[2];
    T[0] = X[0] ^ Y[1];
    X[2] = ror(T[2], 31); X[1] = ror(T[1], 31); X[0] = ror(T[0], 31);
    X[3] = ror(T[3], 31) ^ ror(T[0], 30);

    store1Reg(output, &X[0]);
}

void
Crypto::load2Reg(
    uint32_t *X,
    uint32_t *Y,
    uint8_t *output,
    uint8_t *input)
{
    for (int i = 0; i < 4; ++i) {
        X[i] = *((uint32_t *)&output[i*4]);
        Y[i] = *((uint32_t *)&input[i*4]);
    }
}

void
Crypto::load3Reg(
    uint32_t *X,
    uint32_t *Y,
    uint32_t *Z,
    uint8_t *output,
    uint8_t *input,
    uint8_t *input2)
{
    for (int i = 0; i < 4; ++i) {
        X[i] = *((uint32_t *)&output[i*4]);
        Y[i] = *((uint32_t *)&input[i*4]);
        Z[i] = *((uint32_t *)&input2[i*4]);
    }
}

void
Crypto::store1Reg(uint8_t *output, uint32_t *X)
{
    for (int i = 0; i < 4; ++i) {
        output[i*4] = (uint8_t)(X[i]);
        output[i*4+1] = (uint8_t)(X[i] >> 8);
        output[i*4+2] = (uint8_t)(X[i] >> 16);
        output[i*4+3] = (uint8_t)(X[i] >> 24);
    }
}

} // namespace ArmISA
