#pragma once

// Header-only MD5 and SHA256 implementation
// Based on public domain implementations
// Replaces OpenSSL dependency for offline installation

#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

namespace ai_mirror::utils {

// MD5 implementation
class MD5 {
public:
  static std::string hash(const std::string &input) {
    std::array<uint8_t, 16> digest{};
    compute(input.data(), input.size(), digest.data());

    std::ostringstream oss;
    for (unsigned int i = 0; i < 16; ++i) {
      oss << std::hex << std::setfill('0') << std::setw(2)
          << static_cast<int>(digest[i]);
    }
    return oss.str();
  }

private:
  static void compute(const char *data, size_t len, uint8_t *digest) {
    // MD5 constants
    static const uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
        0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
        0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
        0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

    static const uint32_t r[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

    // Initialize hash values
    uint32_t a0 = 0x67452301;
    uint32_t b0 = 0xefcdab89;
    uint32_t c0 = 0x98badcfe;
    uint32_t d0 = 0x10325476;

    // Pre-processing: adding padding bits
    uint64_t bit_len = len * 8;
    size_t padding_len = (56 - (len + 1) % 64 + 64) % 64;
    std::vector<uint8_t> msg(len + 1 + padding_len + 8);
    std::memcpy(msg.data(), data, len);
    msg[len] = 0x80;
    for (int i = 0; i < 8; ++i) {
      msg[len + 1 + padding_len + i] = (bit_len >> (i * 8)) & 0xff;
    }

    // Process each 512-bit chunk
    for (size_t offset = 0; offset < msg.size(); offset += 64) {
      uint32_t m[16];
      for (int i = 0; i < 16; ++i) {
        m[i] = (msg[offset + i * 4]) | (msg[offset + i * 4 + 1] << 8) |
               (msg[offset + i * 4 + 2] << 16) |
               (msg[offset + i * 4 + 3] << 24);
      }

      uint32_t a = a0, b = b0, c = c0, d = d0;

      for (int i = 0; i < 64; ++i) {
        uint32_t f, g;
        if (i < 16) {
          f = (b & c) | (~b & d);
          g = i;
        } else if (i < 32) {
          f = (d & b) | (~d & c);
          g = (5 * i + 1) % 16;
        } else if (i < 48) {
          f = b ^ c ^ d;
          g = (3 * i + 5) % 16;
        } else {
          f = c ^ (b | ~d);
          g = (7 * i) % 16;
        }

        uint32_t temp = d;
        d = c;
        c = b;
        uint32_t x = a + f + k[i] + m[g];
        b = b + ((x << r[i]) | (x >> (32 - r[i])));
        a = temp;
      }

      a0 += a;
      b0 += b;
      c0 += c;
      d0 += d;
    }

    // Produce the final hash value (little-endian)
    for (int i = 0; i < 4; ++i) {
      digest[i] = (a0 >> (i * 8)) & 0xff;
      digest[i + 4] = (b0 >> (i * 8)) & 0xff;
      digest[i + 8] = (c0 >> (i * 8)) & 0xff;
      digest[i + 12] = (d0 >> (i * 8)) & 0xff;
    }
  }
};

// SHA256 implementation
class SHA256 {
public:
  static std::string hash(const std::string &input) {
    std::array<uint8_t, 32> digest{};
    compute(input.data(), input.size(), digest.data());

    std::ostringstream oss;
    for (unsigned int i = 0; i < 32; ++i) {
      oss << std::hex << std::setfill('0') << std::setw(2)
          << static_cast<int>(digest[i]);
    }
    return oss.str();
  }

private:
  static void compute(const char *data, size_t len, uint8_t *digest) {
    // SHA256 constants
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    // Initialize hash values
    uint32_t h0 = 0x6a09e667;
    uint32_t h1 = 0xbb67ae85;
    uint32_t h2 = 0x3c6ef372;
    uint32_t h3 = 0xa54ff53a;
    uint32_t h4 = 0x510e527f;
    uint32_t h5 = 0x9b05688c;
    uint32_t h6 = 0x1f83d9ab;
    uint32_t h7 = 0x5be0cd19;

    // Pre-processing: adding padding bits
    uint64_t bit_len = len * 8;
    size_t padding_len = (56 - (len + 1) % 64 + 64) % 64;
    std::vector<uint8_t> msg(len + 1 + padding_len + 8);
    std::memcpy(msg.data(), data, len);
    msg[len] = 0x80;
    for (int i = 0; i < 8; ++i) {
      msg[len + 1 + padding_len + i] = (bit_len >> ((7 - i) * 8)) & 0xff;
    }

    // Process each 512-bit chunk
    for (size_t offset = 0; offset < msg.size(); offset += 64) {
      uint32_t w[64];
      for (int i = 0; i < 16; ++i) {
        w[i] = (msg[offset + i * 4] << 24) | (msg[offset + i * 4 + 1] << 16) |
               (msg[offset + i * 4 + 2] << 8) | (msg[offset + i * 4 + 3]);
      }
      for (int i = 16; i < 64; ++i) {
        uint32_t s0 = (w[i - 15] >> 7 | w[i - 15] << 25) ^
                      (w[i - 15] >> 18 | w[i - 15] << 14) ^ (w[i - 15] >> 3);
        uint32_t s1 = (w[i - 2] >> 17 | w[i - 2] << 15) ^
                      (w[i - 2] >> 19 | w[i - 2] << 13) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      }

      uint32_t a = h0, b = h1, c = h2, d = h3;
      uint32_t e = h4, f = h5, g = h6, h = h7;

      for (int i = 0; i < 64; ++i) {
        uint32_t S1 = (e >> 6 | e << 26) ^ (e >> 11 | e << 21) ^ (e >> 25 | e << 7);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = (a >> 2 | a << 30) ^ (a >> 13 | a << 19) ^ (a >> 22 | a << 10);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
      }

      h0 += a;
      h1 += b;
      h2 += c;
      h3 += d;
      h4 += e;
      h5 += f;
      h6 += g;
      h7 += h;
    }

    // Produce the final hash value (big-endian)
    for (int i = 0; i < 4; ++i) {
      digest[i] = (h0 >> ((3 - i) * 8)) & 0xff;
      digest[i + 4] = (h1 >> ((3 - i) * 8)) & 0xff;
      digest[i + 8] = (h2 >> ((3 - i) * 8)) & 0xff;
      digest[i + 12] = (h3 >> ((3 - i) * 8)) & 0xff;
      digest[i + 16] = (h4 >> ((3 - i) * 8)) & 0xff;
      digest[i + 20] = (h5 >> ((3 - i) * 8)) & 0xff;
      digest[i + 24] = (h6 >> ((3 - i) * 8)) & 0xff;
      digest[i + 28] = (h7 >> ((3 - i) * 8)) & 0xff;
    }
  }
};

} // namespace ai_mirror::utils
