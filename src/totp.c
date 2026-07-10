#include "totp.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <ctype.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

static uint64_t last_accepted_step = 0;

static int base32_decode(const char *in, uint8_t *out, size_t out_max, size_t *out_len) {
    int buffer = 0;
    int bits_left = 0;
    size_t count = 0;
    
    for (const char *p = in; *p; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '=') continue;
        
        int val = -1;
        if (c >= 'A' && c <= 'Z') val = c - 'A';
        else if (c >= 'a' && c <= 'z') val = c - 'a';
        else if (c >= '2' && c <= '7') val = c - '2' + 26;
        
        if (val < 0) return -1; // Invalid character
        
        buffer = (buffer << 5) | val;
        bits_left += 5;
        
        if (bits_left >= 8) {
            if (count >= out_max) return -1; // Buffer too small
            out[count++] = (buffer >> (bits_left - 8)) & 0xFF;
            bits_left -= 8;
        }
    }
    *out_len = count;
    return 0;
}

static uint32_t totp_generate(const uint8_t *secret, size_t secret_len, uint64_t step) {
    uint8_t msg[8];
    for (int i = 7; i >= 0; i--) {
        msg[i] = step & 0xFF;
        step >>= 8;
    }
    
    unsigned int md_len;
    uint8_t hash[EVP_MAX_MD_SIZE];
    HMAC(EVP_sha1(), secret, secret_len, msg, 8, hash, &md_len);
    
    int offset = hash[19] & 0x0F;
    uint32_t p = (hash[offset] & 0x7F) << 24 |
                 (hash[offset + 1] & 0xFF) << 16 |
                 (hash[offset + 2] & 0xFF) << 8 |
                 (hash[offset + 3] & 0xFF);
                 
    return p % 1000000;
}

int totp_verify(const char *secret, const char *code) {
    if (!secret || !code) return 0;
    
    uint8_t sec_bytes[256];
    size_t sec_len = 0;
    if (base32_decode(secret, sec_bytes, sizeof(sec_bytes), &sec_len) != 0) {
        return 0; // Invalid secret format
    }
    
    uint32_t expected_code = 0;
    for (int i = 0; i < 6; i++) {
        if (!isdigit(code[i])) return 0;
        expected_code = expected_code * 10 + (code[i] - '0');
    }
    
    uint64_t current_step = time(NULL) / 30;
    
    for (int i = -1; i <= 1; i++) {
        uint64_t step = current_step + i;
        if (step <= last_accepted_step) continue; // Prevent replay
        
        uint32_t valid_code = totp_generate(sec_bytes, sec_len, step);
        if (valid_code == expected_code) {
            last_accepted_step = step; // Consume the code
            return 1;
        }
    }
    
    return 0;
}
