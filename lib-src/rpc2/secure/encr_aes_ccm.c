/* BLURB lgpl
			Coda File System
			    Release 6

	    Copyright (c) 2006 Carnegie Mellon University
		  Additional copyrights listed below

This  code  is  distributed "AS IS" without warranty of any kind under
the  terms of the  GNU  Library General Public Licence  Version 2,  as
shown in the file LICENSE. The technical and financial contributors to
Coda are listed in the file CREDITS.

			Additional copyrights
#*/

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <rpc2/secure.h>
#include "aes.h"
#include "grunt.h"

/* various constants that define the CCM flags, using fixed values according
 * to RFC4309. */
#define AFLAG    1	/* we have additional authenticated data */
#define NONCELEN 11	/* fixed nonce size, 3 byte salt + 8 byte IV */
#define PARM_L   (AES_BLOCK_SIZE - 1 - NONCELEN) /* size of length field == 4 */
#define CCMflags(len) (AFLAG<<6 | (((len/2)-1)<<3) | (PARM_L-1))

struct aes_ccm_ctx {
    union {
	uint32_t salt;
	uint8_t flag_n_salt[4];
    } u;
    aes_encrypt_ctx ctx;
    unsigned int icv_len;
};

static int init(void **ctx, const uint8_t *key, size_t len, size_t icv_len)
{
    struct aes_ccm_ctx *acc = malloc(sizeof(struct aes_ccm_ctx));
    if (!acc) return 0;

    /* copy salt */
    acc->u.flag_n_salt[3] = key[--len];
    acc->u.flag_n_salt[2] = key[--len];
    acc->u.flag_n_salt[1] = key[--len];
    acc->u.flag_n_salt[0] = CCMflags(icv_len);
    acc->icv_len = icv_len;

    if      (len >= bytes(256)) len = 256;
    else if (len >= bytes(192)) len = 192;
    else if (len >= bytes(128)) len = 128;
    else goto err_out;

    if (aes_encrypt_key(key, len, &acc->ctx) == 0) {
	*ctx = acc;
	return 0;
    }

err_out:
    free(acc);
    return -1;
}

static int init8 (void **ctx, const uint8_t *key, size_t len)
{
    return init(ctx, key, len, 8);
}

static int init12(void **ctx, const uint8_t *key, size_t len)
{
    return init(ctx, key, len, 12);
}

static int init16(void **ctx, const uint8_t *key, size_t len)
{
    return init(ctx, key, len, 16);
}


static void release(void **ctx)
{
    if (!*ctx) return;
    memset(*ctx, 0, sizeof(struct aes_ccm_ctx));
    free(*ctx);
    *ctx = NULL;
}

static int aes_ccm_crypt(void *ctx, const uint8_t *in, uint8_t *out, size_t len,
			 const uint8_t *iv, const uint8_t *aad,
			 size_t aad_len, int encrypt)
{
    struct aes_ccm_ctx *acc = (struct aes_ccm_ctx *)ctx;
    int i, nblocks;
    uint8_t CMAC[AES_BLOCK_SIZE], CTR[AES_BLOCK_SIZE], S0[AES_BLOCK_SIZE];
    uint8_t tmp[AES_BLOCK_SIZE];
    const uint8_t *src;

    if (!encrypt) {
	if (len < acc->icv_len)
	    return 0;
	len -= acc->icv_len;
    }

    nblocks = (len + AES_BLOCK_SIZE - 1) / AES_BLOCK_SIZE;

    /* initialize CMAC (initial seed for authentication) */
    int32(CMAC)[0] = acc->u.salt;
    int32(CMAC)[1] = int32(iv)[0];
    int32(CMAC)[2] = int32(iv)[1];
    int32(CMAC)[3] = htonl(len);
    aes_encrypt(CMAC, CMAC, &acc->ctx);

    /* initialize counter block */
    int32(CTR)[0] = acc->u.salt & 0x07ffffff;
    int32(CTR)[1] = int32(iv)[0];
    int32(CTR)[2] = int32(iv)[1];
    int32(CTR)[3] = 0;
    /* save the first counter block. we will use that for authentication. */
    aes_encrypt(CTR, S0, &acc->ctx);

    /* authenticate the header (spi and sequence number values) */
    /* kind of ugly, assumes the additional authenticated data is only 8
     * bytes long (spi & seq) */
    /* also ugly that this CCM header is not aligned on a 4-byte boundary
     * we have to copy everything byte-by-byte */

    /* assert(aad_len == 2 * sizeof(uint32_t)); */
    tmp[0] = 0;      tmp[1] = 8;      /* length of authenticated data */
    tmp[2] = aad[0]; tmp[3] = aad[1]; /* first 2 bytes of spi */
    tmp[4] = aad[2]; tmp[5] = aad[3]; /* second 2 bytes of spi */
    tmp[6] = aad[4]; tmp[7] = aad[5]; /* first 2 bytes of seq */
    tmp[8] = aad[6]; tmp[9] = aad[7]; /* second 2 bytes of seq */
    tmp[10] = tmp[11] = 0;	      /* clear the rest */
    int32(tmp)[3] = 0;

    xor128(CMAC, tmp);
    aes_encrypt(CMAC, CMAC, &acc->ctx);

    while (nblocks--)
    {
	/* increment counter */
	for (i = AES_BLOCK_SIZE-1; i >=0; i--)
	    if (++CTR[i] != 0) break;

	/* get the next keystream block */
	aes_encrypt(CTR, tmp, &acc->ctx);

	/* we don't need the counter block anymore, so re-use it so we can zero
	 * pad the last partial input block to the full block size */
	if (!nblocks && len % AES_BLOCK_SIZE) {
	    memset(CTR, 0, AES_BLOCK_SIZE);
	    memcpy(CTR, in, len % AES_BLOCK_SIZE);
	    src = CTR;
	} else
	    src = in;

	/* Checksum the plaintext before we encrypt */
	if (encrypt)
	    xor128(CMAC, src);

	if (out != src)
	    memcpy(out, src, AES_BLOCK_SIZE);
	xor128(out, tmp);

	/* Checksum the plaintext after decryption */
	if (!encrypt)
	    xor128(CMAC, out);

	aes_encrypt(CMAC, CMAC, &acc->ctx);

	in  += AES_BLOCK_SIZE;
	out += AES_BLOCK_SIZE;
    }

    /* finalize the ICV calculation */
    xor128(CMAC, S0);
    if (encrypt) {
	memcpy(out, CMAC, acc->icv_len);
	len += acc->icv_len;
    }
    else if (memcmp(in, CMAC, acc->icv_len) != 0)
	return 0;

    return len;
}

static int encrypt(void *ctx, const uint8_t *in, uint8_t *out, size_t len,
		   uint8_t *iv, const uint8_t *aad, size_t aad_len)
{
    return aes_ccm_crypt(ctx, in, out, len, iv, aad, aad_len, 1);
}

static int decrypt(void *ctx, const uint8_t *in, uint8_t *out, size_t len,
		   const uint8_t *iv, const uint8_t *aad, size_t aad_len)
{
    return aes_ccm_crypt(ctx, in, out, len, iv, aad, aad_len, 0);
}

struct secure_encr secure_ENCR_AES_CCM_8 = {
    .id	          = SECURE_ENCR_AES_CCM_8,
    .name         = "ENCR-AES-CCM-8",
    .encrypt_init = init8,
    .encrypt_free = release,
    .encrypt      = encrypt,
    .decrypt_init = init8,
    .decrypt_free = release,
    .decrypt      = decrypt,
    .min_keysize  = bytes(128) + 3,
    .max_keysize  = bytes(256) + 3,
    .blocksize    = AES_BLOCK_SIZE,
    .iv_len       = 8,
    .icv_len      = 8,
};

struct secure_encr secure_ENCR_AES_CCM_12 = {
    .id	          = SECURE_ENCR_AES_CCM_12,
    .name         = "ENCR-AES-CCM-12",
    .encrypt_init = init12,
    .encrypt_free = release,
    .encrypt      = encrypt,
    .decrypt_init = init12,
    .decrypt_free = release,
    .decrypt      = decrypt,
    .min_keysize  = bytes(128) + 3,
    .max_keysize  = bytes(256) + 3,
    .blocksize    = AES_BLOCK_SIZE,
    .iv_len       = 8,
    .icv_len      = 12,
};

struct secure_encr secure_ENCR_AES_CCM_16 = {
    .id	          = SECURE_ENCR_AES_CCM_16,
    .name         = "ENCR-AES-CCM-12",
    .encrypt_init = init16,
    .encrypt_free = release,
    .encrypt      = encrypt,
    .decrypt_init = init16,
    .decrypt_free = release,
    .decrypt      = decrypt,
    .min_keysize  = bytes(128) + 3,
    .max_keysize  = bytes(256) + 3,
    .blocksize    = AES_BLOCK_SIZE,
    .iv_len       = 8,
    .icv_len      = 16,
};

