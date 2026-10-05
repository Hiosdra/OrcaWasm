#pragma once
// OpenSSL MD5 API for WASM builds, implemented directly from RFC 1321 so no
// OpenSSL library is linked. OrcaSlicer uses it for file checksums such as the
// Metadata/plate_N.gcode.md5 entries of a sliced 3MF project.
#include <cstring>
#include <cstddef>
#include <cstdint>

#define MD5_DIGEST_LENGTH 16
#define MD5_LONG unsigned int
#define MD5_LBLOCK 16
#define MD5_CBLOCK 64

typedef struct {
    MD5_LONG A, B, C, D;
    MD5_LONG Nl, Nh;
    unsigned char data[MD5_CBLOCK];
    unsigned int num;
} MD5_CTX;

namespace orcawasm_md5 {

inline std::uint32_t rotl(std::uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

inline void transform(MD5_CTX* c, const unsigned char* block)
{
    static const std::uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    static const int S[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    std::uint32_t M[16];
    for (int i = 0; i < 16; ++i) {
        M[i] = std::uint32_t(block[i * 4]) | (std::uint32_t(block[i * 4 + 1]) << 8)
            | (std::uint32_t(block[i * 4 + 2]) << 16) | (std::uint32_t(block[i * 4 + 3]) << 24);
    }
    std::uint32_t a = c->A, b = c->B, cc = c->C, d = c->D;
    for (int i = 0; i < 64; ++i) {
        std::uint32_t f;
        int g;
        if (i < 16) { f = (b & cc) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & cc); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ cc ^ d; g = (3 * i + 5) % 16; }
        else { f = cc ^ (b | ~d); g = (7 * i) % 16; }
        const std::uint32_t next = d;
        d = cc;
        cc = b;
        b = b + rotl(a + f + K[i] + M[g], S[i]);
        a = next;
    }
    c->A += a;
    c->B += b;
    c->C += cc;
    c->D += d;
}

} // namespace orcawasm_md5

inline int MD5_Init(MD5_CTX* c)
{
    c->A = 0x67452301;
    c->B = 0xefcdab89;
    c->C = 0x98badcfe;
    c->D = 0x10325476;
    c->Nl = c->Nh = 0;
    c->num = 0;
    return 1;
}

inline int MD5_Update(MD5_CTX* c, const void* d, std::size_t n)
{
    const unsigned char* in = static_cast<const unsigned char*>(d);
    const std::uint64_t bits = (std::uint64_t(c->Nh) << 32 | c->Nl) + std::uint64_t(n) * 8;
    c->Nl = static_cast<MD5_LONG>(bits);
    c->Nh = static_cast<MD5_LONG>(bits >> 32);
    while (n > 0) {
        const std::size_t take = MD5_CBLOCK - c->num < n ? MD5_CBLOCK - c->num : n;
        std::memcpy(c->data + c->num, in, take);
        c->num += static_cast<unsigned int>(take);
        in += take;
        n -= take;
        if (c->num == MD5_CBLOCK) {
            orcawasm_md5::transform(c, c->data);
            c->num = 0;
        }
    }
    return 1;
}

inline int MD5_Final(unsigned char* md, MD5_CTX* c)
{
    unsigned char length[8];
    for (int i = 0; i < 4; ++i) {
        length[i] = static_cast<unsigned char>(c->Nl >> (8 * i));
        length[i + 4] = static_cast<unsigned char>(c->Nh >> (8 * i));
    }
    static const unsigned char padding[MD5_CBLOCK] = { 0x80 };
    const unsigned int pad = c->num < 56 ? 56 - c->num : 120 - c->num;
    MD5_Update(c, padding, pad);
    MD5_Update(c, length, 8);
    const MD5_LONG words[4] = { c->A, c->B, c->C, c->D };
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j)
            md[i * 4 + j] = static_cast<unsigned char>(words[i] >> (8 * j));
    }
    std::memset(c, 0, sizeof(*c));
    return 1;
}

inline unsigned char* MD5(const unsigned char* d, std::size_t n, unsigned char* md)
{
    static unsigned char fallback[MD5_DIGEST_LENGTH];
    if (!md) md = fallback;
    MD5_CTX c;
    MD5_Init(&c);
    MD5_Update(&c, d, n);
    MD5_Final(md, &c);
    return md;
}
