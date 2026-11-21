#pragma once
#include <string>
#include <vector>
#include <cstdint>

inline std::vector<uint8_t> base64_decode(const std::string& in) {
    std::vector<uint8_t> out;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) {
        T["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]] = i;
    }

    size_t comma = in.find(',');
    size_t start = (comma != std::string::npos) ? comma + 1 : 0;

    uint32_t val = 0;
    int valb = -8;
    for (size_t i = start; i < in.size(); i++) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (T[c] == -1) {
            if (c == '=') break;
            continue;
        }
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}
