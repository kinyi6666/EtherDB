/*
 * EtDBSha256.h — Minimal SHA-256 (hex digest) for the CM_CONNECT challenge
 *
 * The client library is intentionally standalone (it must not include
 * server-side headers such as base/Sha256.h), so the 32-bit FIPS 180-4 core
 * lives here. It produces byte-identical digests to the server's
 * EtherDB::Sha256 and is used for the login proof:
 *
 *     proof = SHA256hex(nonce || SHA256hex(salt || password))
 */
#ifndef ETDB_CLIENT_SHA256_H
#define ETDB_CLIENT_SHA256_H

#include <string>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace ETDB {
namespace Client {

class MsgSha256 {
public:
    MsgSha256() { reset(); }

    void reset() {
        _len = 0;
        _totLen = 0;
        _h[0] = 0x6a09e667; _h[1] = 0xbb67ae85;
        _h[2] = 0x3c6ef372; _h[3] = 0xa54ff53a;
        _h[4] = 0x510e527f; _h[5] = 0x9b05688c;
        _h[6] = 0x1f83d9ab; _h[7] = 0x5be0cd19;
    }

    void update(const uint8_t* data, size_t length) {
        for (size_t i = 0; i < length; i++) {
            _data[_len++] = data[i];
            if (_len == 64) {
                transform(_data);
                _totLen += 512;
                _len = 0;
            }
        }
    }

    void update(const std::string& s) {
        update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    void finalize(uint8_t hash[32]) {
        uint64_t totalBits = _totLen + _len * 8;

        _data[_len++] = 0x80;
        if (_len > 56) {
            while (_len < 64) _data[_len++] = 0;
            transform(_data);
            _len = 0;
        }
        while (_len < 56) _data[_len++] = 0;
        for (int i = 7; i >= 0; i--) _data[_len++] = (uint8_t)((totalBits >> (i * 8)) & 0xff);
        transform(_data);

        for (int i = 0; i < 8; i++) {
            hash[i * 4]     = (_h[i] >> 24) & 0xff;
            hash[i * 4 + 1] = (_h[i] >> 16) & 0xff;
            hash[i * 4 + 2] = (_h[i] >> 8)  & 0xff;
            hash[i * 4 + 3] = _h[i]         & 0xff;
        }
    }

    static std::string hex(const std::string& input) {
        MsgSha256 s;
        s.update(input);
        uint8_t digest[32];
        s.finalize(digest);
        std::ostringstream oss;
        for (int i = 0; i < 32; i++)
            oss << std::hex << std::setfill('0') << std::setw(2) << (int)digest[i];
        return oss.str();
    }

private:
    static inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
    static inline uint32_t Ch  (uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
    static inline uint32_t Maj (uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
    static inline uint32_t ep0 (uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
    static inline uint32_t ep1 (uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
    static inline uint32_t sig0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
    static inline uint32_t sig1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

    void transform(const uint8_t data[64]) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
            0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
            0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
                   ((uint32_t)data[i * 4 + 2] << 8) | ((uint32_t)data[i * 4 + 3]);
        }
        for (int i = 16; i < 64; i++)
            w[i] = sig1(w[i - 2]) + w[i - 7] + sig0(w[i - 15]) + w[i - 16];

        uint32_t a = _h[0], b = _h[1], c = _h[2], d = _h[3];
        uint32_t e = _h[4], f = _h[5], g = _h[6], h = _h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = h + ep1(e) + Ch(e, f, g) + K[i] + w[i];
            uint32_t t2 = ep0(a) + Maj(a, b, c);
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        _h[0] += a; _h[1] += b; _h[2] += c; _h[3] += d;
        _h[4] += e; _h[5] += f; _h[6] += g; _h[7] += h;
    }

    uint8_t  _data[64];
    uint32_t _len;
    uint64_t _totLen;
    uint32_t _h[8];
};

} // namespace Client
} // namespace ETDB

#endif // ETDB_CLIENT_SHA256_H
