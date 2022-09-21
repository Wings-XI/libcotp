#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <openssl/evp.h>
#include <baseencode.h>
#include <stdint.h>
#include "cotp.h"

#define SHA1_DIGEST_SIZE    20
#define SHA256_DIGEST_SIZE  32
#define SHA512_DIGEST_SIZE  64

#ifdef _MSC_VER
#define strdup _strdup
#endif

static long long int DIGITS_POWER[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000, 10000000000};

int g_digest_initialized = 0;

static char *
normalize_secret (const char *K)
{
    char *nK = calloc (1, strlen (K) + 1);
    if (nK == NULL) {
        fprintf (stderr, "Error during memory allocation\n");
        return nK;
    }

    int i = 0, j = 0;
    while (K[i] != '\0') {
        if (K[i] != ' ') {
            if (K[i] >= 'a' && K[i] <= 'z') {
                nK[j++] = (char) (K[i] - 32);
            } else {
                nK[j++] = K[i];
            }
        }
        i++;
    }
    return nK;
}


static char *
get_steam_code(unsigned const char *hmac)
{
    int offset = (hmac[SHA1_DIGEST_SIZE-1] & 0x0f);

    // Starting from the offset, take the successive 4 bytes while stripping the topmost bit to prevent it being handled as a signed integer
    size_t bin_code = ((hmac[offset] & 0x7f) << 24) | ((hmac[offset + 1] & 0xff) << 16) | ((hmac[offset + 2] & 0xff) << 8) | ((hmac[offset + 3] & 0xff));

    const char steam_alphabet[] = "23456789BCDFGHJKMNPQRTVWXY";

    char code[6];
    size_t steam_alphabet_len = strlen(steam_alphabet);
    for (int i = 0; i < 5; i++) {
        size_t mod = bin_code % steam_alphabet_len;
        bin_code = bin_code / steam_alphabet_len;
        code[i] = steam_alphabet[mod];
    }
    code[5] = '\0';

    return strdup(code);
}


static int
truncate(unsigned const char *hmac, int digits_length, int algo)
{
    // take the lower four bits of the last byte
    int offset = 0;
    switch (algo) {
        case SHA1:
            offset = (hmac[SHA1_DIGEST_SIZE-1] & 0x0f);
            break;
        case SHA256:
            offset = (hmac[SHA256_DIGEST_SIZE-1] & 0x0f);
            break;
        case SHA512:
            offset = (hmac[SHA512_DIGEST_SIZE-1] & 0x0f);
            break;
        default:
            break;
    }

    // Starting from the offset, take the successive 4 bytes while stripping the topmost bit to prevent it being handled as a signed integer
    int bin_code = ((hmac[offset] & 0x7f) << 24) | ((hmac[offset + 1] & 0xff) << 16) | ((hmac[offset + 2] & 0xff) << 8) | ((hmac[offset + 3] & 0xff));

    int token = bin_code % DIGITS_POWER[digits_length];

    return token;
}


static unsigned char *
compute_hmac(const char *K, int64_t C, int algo)
{
    baseencode_error_t err;
    size_t secret_len = (size_t) ((strlen(K) + 1.6 - 1) / 1.6);

    char *normalized_K = normalize_secret (K);
    if (normalized_K == NULL) {
        return NULL;
    }
    unsigned char *secret = base32_decode(normalized_K, strlen(normalized_K), &err);
    free (normalized_K);
    if (secret == NULL) {
        return NULL;
    }

	unsigned char C_reverse_byte_order[8];
	memset(C_reverse_byte_order, 0, sizeof(C_reverse_byte_order));
    int j, i;
    for (j = 0, i = 7; j < 8 && i >= 0; j++, i--)
        C_reverse_byte_order[i] = ((unsigned char *) &C)[j];

	if (g_digest_initialized == 0) {
		OpenSSL_add_all_digests();
		g_digest_initialized = 1;
	}

	EVP_MD_CTX* hd = NULL;
	hd = EVP_MD_CTX_create();
	if (!hd) {
		free(secret);
		return NULL;
	}
	const EVP_MD* md = NULL;
	switch (algo) {
	case SHA1:
		md = EVP_sha1();
		break;
	case SHA256:
		md = EVP_sha256();
		break;
	case SHA512:
		md = EVP_sha512();
		break;
	default:
		EVP_MD_CTX_destroy(hd);
		free(secret);
		return NULL;
	}
	EVP_PKEY* pkey = NULL;
	pkey = EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, NULL, secret, (int)secret_len);
	if (!pkey) {
		EVP_MD_CTX_destroy(hd);
		free(secret);
		return NULL;
	}
	if (EVP_DigestSignInit(hd, NULL, md, NULL, pkey) != 1) {
		EVP_PKEY_free(pkey);
		EVP_MD_CTX_destroy(hd);
		free(secret);
		return NULL;
	}
	if (EVP_DigestSignUpdate(hd, C_reverse_byte_order, sizeof(C_reverse_byte_order)) != 1) {
		EVP_PKEY_free(pkey);
		EVP_MD_CTX_destroy(hd);
		free(secret);
		return NULL;
	}
	unsigned char* hmac = malloc(EVP_MAX_MD_SIZE);
	if (!hmac) {
		EVP_PKEY_free(pkey);
		EVP_MD_CTX_destroy(hd);
		free(secret);
		return NULL;
	}
	size_t md_len = 0;
	if (EVP_DigestSignFinal(hd, hmac, &md_len) != 1) {
		free(hmac);
		EVP_PKEY_free(pkey);
		EVP_MD_CTX_destroy(hd);
		free(secret);
		return NULL;
	}
	EVP_PKEY_free(pkey);
	EVP_MD_CTX_destroy(hd);

    //gcry_md_hd_t hd;
    //gcry_md_open(&hd, algo, GCRY_MD_FLAG_HMAC);
    //gcry_md_setkey(hd, secret, secret_len);
    //gcry_md_write(hd, C_reverse_byte_order, sizeof(C_reverse_byte_order));
    //gcry_md_final (hd);
    //unsigned char *hmac = gcry_md_read(hd, algo);

    free(secret);

    return hmac;
}


static char *
finalize(int digits_length, int tk)
{
    char *token = malloc((size_t)digits_length + 1);
    if (token == NULL) {
        fprintf (stderr, "Error during memory allocation\n");
        return token;
    } else {
        int extra_char = digits_length < 10 ? 0 : 1;
        char *fmt = calloc(1, 5 + extra_char);
        if (fmt == NULL) {
            fprintf (stderr, "Error during memory allocation\n");
            free (token);
            return fmt;
        }
        memcpy (fmt, "%.", 3);
        snprintf (fmt + 2, 2 + extra_char, "%d", digits_length);
        memcpy (fmt + 3 + extra_char, "d", 2);
        snprintf (token, digits_length + 1, fmt, tk);
        free (fmt);
    }
    return token;
}


static int
check_period(int period)
{
    if (period <= 0 || period > 120) {
        return INVALID_PERIOD;
    }
    return VALID;
}


static int
check_otp_len(int digits_length)
{
    if (digits_length < 3 || digits_length > 10) {
        return INVALID_DIGITS;
    }
    return VALID;
}


static int
check_algo(int algo)
{
    if (algo != SHA1 && algo != SHA256 && algo != SHA512) {
        return INVALID_ALGO;
    } else {
        return VALID;
    }
}


char *
get_hotp(const char *secret, int64_t timestamp, int digits, int algo, cotp_error_t *err_code)
{
    if (check_algo(algo) == INVALID_ALGO) {
        *err_code = INVALID_ALGO;
        return NULL;
    }

    if (check_otp_len(digits) == INVALID_DIGITS) {
        *err_code = INVALID_DIGITS;
        return NULL;
    }

    unsigned char *hmac = compute_hmac(secret, timestamp, algo);
    if (hmac == NULL) {
        *err_code = INVALID_B32_INPUT;
        return NULL;
    }
    int tk = truncate(hmac, digits, algo);
    char *token = finalize(digits, tk);
	free(hmac);
    return token;
}


char *
get_totp(const char *secret, int digits, int period, int algo, cotp_error_t *err_code)
{
    return get_totp_at(secret, (int64_t)time(NULL), digits, period, algo, err_code);
}


char *
get_steam_totp (const char *secret, int period, cotp_error_t *err_code)
{
    // AFAIK, the secret is stored base64 encoded on the device. As I don't have time to waste on reverse engineering
    // this non-standard solution, the user is responsible for decoding the secret in whatever format this is and then
    // providing the library with the secret base32 encoded.
    return get_steam_totp_at (secret, (int64_t)time(NULL), period, err_code);
}


char *
get_totp_at(const char *secret, int64_t current_timestamp, int digits, int period, int algo, cotp_error_t *err_code)
{
    if (check_otp_len(digits) == INVALID_DIGITS) {
        *err_code = INVALID_DIGITS;
        return NULL;
    }

    if (check_period(period) == INVALID_PERIOD) {
        *err_code = INVALID_PERIOD;
        return NULL;
    }

    int64_t timestamp = current_timestamp / period;

    cotp_error_t err;
    char *token = get_hotp(secret, timestamp, digits, algo, &err);
    if (token == NULL) {
        *err_code = err;
        return NULL;
    }
    return token;
}


char *
get_steam_totp_at (const char *secret, int64_t current_timestamp, int period, cotp_error_t *err_code)
{
    if (check_period(period) == INVALID_PERIOD) {
        *err_code = INVALID_PERIOD;
        return NULL;
    }

    int64_t timestamp = current_timestamp / period;

    unsigned char *hmac = compute_hmac(secret, timestamp, SHA1);
    if (hmac == NULL) {
        *err_code = INVALID_B32_INPUT;
        return NULL;
    }

	char* code = get_steam_code(hmac);
	free(hmac);
	return code;
}


int
totp_verify(const char *secret, const char *user_totp, int digits, int period, int algo)
{
    cotp_error_t err;
    char *current_totp = get_totp(secret, digits, period, algo, &err);
    if (current_totp == NULL) {
        return err;
    }

    int token_status;
    if (strcmp(current_totp, user_totp) != 0) {
        token_status = INVALID_OTP;
    } else {
        token_status = VALID;
    }
    free(current_totp);

    return token_status;
}


int
hotp_verify(const char *K, int64_t C, int N, const char *user_hotp, int algo)
{
    cotp_error_t err;
    char *current_hotp = get_hotp(K, C, N, algo, &err);
    if (current_hotp == NULL) {
        return err;
    }

    int token_status;
    if (strcmp(current_hotp, user_hotp) != 0) {
        token_status = INVALID_OTP;
    } else {
        token_status = VALID;
    }
    free(current_hotp);

    return token_status;
}
