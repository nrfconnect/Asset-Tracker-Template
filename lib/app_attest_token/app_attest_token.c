/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#include <psa/crypto.h>
#include <ironside/se/boot_report.h>
#include <ironside/se/key_ids.h>
#include <modem/at_cmd_custom.h>
#include <modem/at_parser.h>
#include <app_attest_token.h>

LOG_MODULE_REGISTER(app_attest_token, CONFIG_APP_ATTEST_TOKEN_LOG_LEVEL);

/* PSA key ID for the Application domain IAK. */
#define IAK_APPLICATION CRACEN_KEY_ID_IAK

#define UUID_BYTE_SZ 16u

#if defined(CONFIG_SOC_NRF9220)
#define DEVICE_TYPE 0x11u
#else
#error "Unsupported SoC for app attestation token library"
#endif

/* DEVICE_TYPE is encoded as a single CBOR uint byte; values >= 24 need a
 * 2-byte encoding (0x18 + value) and must be handled separately.
 */
BUILD_ASSERT(DEVICE_TYPE < 24u, "DEVICE_TYPE >= 24 requires multi-byte CBOR encoding");

/*
 * Payload CBOR structure (57 bytes):
 *
 *   D9 D9 F7       tag(55799)
 *   85             array(5)
 *   01             uint(1)        payload-id = device_id_msg_v1
 *   50 <16 bytes>  bstr(16)       device-uuid
 *   11             uint(0x11)     device-type
 *   50 <16 zeros>  bstr(16)       fw-uuid (reserved, all zeros)
 *   50 <16 bytes>  bstr(16)       nonce
 */
#define PAYLOAD_SZ 57u

/*
 * Protected COSE header: {1: -7} encoded as CBOR map (3 bytes: A1 01 26).
 */
#define PROT_HDR_SZ 3u

/*
 * COSE_Sign1 structure (77 bytes):
 *
 *   D2             tag(18)
 *   84             array(4)
 *   43 A1 01 26    bstr(3)       – protected header {1: -7}
 *   A1 04 41 21    map{4: bstr(1) 0x21} – unprotected header: kid = 0x21
 *   F6             nil           – detached payload
 *   58 40 <64 B>   bstr(64)      – ECDSA P-256 signature (r||s)
 */
#define COSE_SZ 77u

/* ECDSA P-256 signature length in bytes. */
#define SIG_SZ 64u

/*
 * Sig_Structure (RFC 8152 §4.4) – note: external_aad is omitted; a nil
 * payload marker is appended instead.
 *
 *   84              array(4)
 *   6A "Signature1" tstr(10)
 *   43 A1 01 26     bstr(3) – protected header
 *   58 39 <57 B>    bstr(57) – payload
 *   F6              nil
 *
 * Total: 1+11+4+2+57+1 = 76 bytes.
 */
#define SIG_STRUCT_SZ (1u + 11u + (1u + PROT_HDR_SZ) + 2u + PAYLOAD_SZ + 1u)

/*
 * Base64url output buffer sizes (standard base64, before padding is stripped).
 * Formula: ceil(n/3)*4 + 1 (NUL).
 *   57 bytes → 19*4 + 1 = 77  (no padding, 57 % 3 == 0)
 *   77 bytes → 26*4 + 1 = 105 (one '=' pad, 77 % 3 == 2)
 */
#define ATTEST_B64_SZ  77u
#define COSE_B64_SZ   105u

#define CHALLENGE_HEX_LEN (APP_ATTEST_TOKEN_CHALLENGE_SZ * 2U)

AT_CMD_CUSTOM(app_attesttoken, "AT%ATTESTTOKEN", app_attesttoken_callback);

static int parse_challenge_from_at_cmd(const char *at_cmd, uint8_t *challenge_out,
				       const uint8_t **challenge_ptr)
{
	struct at_parser parser;
	enum at_parser_cmd_type type;
	char hex_str[CHALLENGE_HEX_LEN + 1];
	size_t hex_len = sizeof(hex_str);
	size_t count;
	size_t bin_len;
	int err;

	*challenge_ptr = NULL;

	err = at_parser_init(&parser, at_cmd);
	if (err) {
		return err;
	}

	err = at_parser_cmd_type_get(&parser, &type);
	if (err || type != AT_PARSER_CMD_TYPE_SET) {
		return -EINVAL;
	}

	err = at_parser_cmd_count_get(&parser, &count);
	if (err) {
		return err;
	}

	if (count == 1) {
		return 0;
	}

	if (count != 2) {
		return -EINVAL;
	}

	err = at_parser_string_get(&parser, 1, hex_str, &hex_len);
	if (err) {
		return err;
	}

	if (hex_len != CHALLENGE_HEX_LEN) {
		return -EINVAL;
	}

	bin_len = hex2bin(hex_str, hex_len, challenge_out, APP_ATTEST_TOKEN_CHALLENGE_SZ);
	if (bin_len != APP_ATTEST_TOKEN_CHALLENGE_SZ) {
		return -EINVAL;
	}

	*challenge_ptr = challenge_out;

	return 0;
}

static int app_attesttoken_callback(char *buf, size_t len, char *at_cmd)
{
	int err;
	char attest_token[APP_ATTEST_TOKEN_BUF_SZ] = {0};
	uint8_t challenge[APP_ATTEST_TOKEN_CHALLENGE_SZ];
	const uint8_t *challenge_ptr = NULL;

	err = parse_challenge_from_at_cmd(at_cmd, challenge, &challenge_ptr);
	if (err) {
		return at_cmd_custom_respond(buf, len, "ERROR\r\n");
	}

	err = app_attest_token_get(attest_token, sizeof(attest_token), challenge_ptr);
	if (err) {
		return at_cmd_custom_respond(buf, len, "ERROR\r\n");
	}

	return at_cmd_custom_respond(buf, len, "%%ATTESTTOKEN: \"%s\"\r\nOK\r\n", attest_token);
}

static void build_payload(uint8_t *buf, const uint8_t *uuid, const uint8_t *nonce)
{
	size_t pos = 0;

	/* tag(55799) = D9 D9 F7 */
	buf[pos++] = 0xD9u;
	buf[pos++] = 0xD9u;
	buf[pos++] = 0xF7u;

	/* array(5) */
	buf[pos++] = 0x85u;

	/* payload-id: uint(1) */
	buf[pos++] = 0x01u;

	/* device-uuid: bstr(16) */
	buf[pos++] = 0x50u;
	memcpy(&buf[pos], uuid, UUID_BYTE_SZ);
	pos += UUID_BYTE_SZ;

	/* device-type: single-byte uint */
	buf[pos++] = DEVICE_TYPE;

	/* fw-uuid: bstr(16) – reserved, all zeros */
	buf[pos++] = 0x50u;
	memset(&buf[pos], 0, UUID_BYTE_SZ);
	pos += UUID_BYTE_SZ;

	/* nonce: bstr(16) */
	buf[pos++] = 0x50u;
	memcpy(&buf[pos], nonce, APP_ATTEST_TOKEN_CHALLENGE_SZ);
	pos += APP_ATTEST_TOKEN_CHALLENGE_SZ;

	__ASSERT_NO_MSG(pos == PAYLOAD_SZ);
}

static size_t build_sig_structure(uint8_t *buf, const uint8_t *payload)
{
	static const uint8_t prot_hdr[PROT_HDR_SZ] = {0xA1u, 0x01u, 0x26u};
	static const uint8_t context[] = "Signature1";
	size_t pos = 0;

	/* array(4) */
	buf[pos++] = 0x84u;

	/* context: tstr("Signature1"), 10 chars */
	buf[pos++] = 0x6Au;
	memcpy(&buf[pos], context, 10u);
	pos += 10u;

	/* body_protected: bstr(3) */
	buf[pos++] = 0x40u | PROT_HDR_SZ;
	memcpy(&buf[pos], prot_hdr, PROT_HDR_SZ);
	pos += PROT_HDR_SZ;

	/* payload: bstr(57) – no external_aad; see SIG_STRUCT_SZ comment */
	buf[pos++] = 0x58u;
	buf[pos++] = (uint8_t)PAYLOAD_SZ;
	memcpy(&buf[pos], payload, PAYLOAD_SZ);
	pos += PAYLOAD_SZ;

	/* nil – detached payload marker */
	buf[pos++] = 0xF6u;

	return pos;
}

static size_t build_cose_sign1(uint8_t *buf, const uint8_t *signature)
{
	static const uint8_t prot_hdr[PROT_HDR_SZ] = {0xA1u, 0x01u, 0x26u};
	size_t pos = 0;

	/* tag(18) */
	buf[pos++] = 0xD2u;

	/* array(4) */
	buf[pos++] = 0x84u;

	/* protected header: bstr(3) */
	buf[pos++] = 0x40u | PROT_HDR_SZ;
	memcpy(&buf[pos], prot_hdr, PROT_HDR_SZ);
	pos += PROT_HDR_SZ;

	/* unprotected header: {4: bstr(1) 0x21}  (kid = 0x21) */
	buf[pos++] = 0xA1u;
	buf[pos++] = 0x04u;
	buf[pos++] = 0x41u;
	buf[pos++] = 0x21u;

	/* detached payload: nil */
	buf[pos++] = 0xF6u;

	/* signature: bstr(64) */
	buf[pos++] = 0x58u;
	buf[pos++] = SIG_SZ;
	memcpy(&buf[pos], signature, SIG_SZ);
	pos += SIG_SZ;

	__ASSERT_NO_MSG(pos == COSE_SZ);
	return pos;
}

static void to_base64url(char *str)
{
	char *p;

	for (p = str; *p != '\0'; ++p) {
		if (*p == '+') {
			*p = '-';
		} else if (*p == '/') {
			*p = '_';
		}
	}

	/* Remove padding. */
	p = strchr(str, '=');
	if (p != NULL) {
		*p = '\0';
	}
}

static int bin_to_base64url(const uint8_t *data, size_t data_sz, char *out, size_t out_sz)
{
	size_t olen;
	int err;

	err = base64_encode(out, out_sz, &olen, data, data_sz);
	if (err) {
		LOG_ERR("base64_encode() failed (err %d)", err);
		return -EIO;
	}

	to_base64url(out);
	return 0;
}

static void device_info_get_uuid(uint8_t *uuid_bytes)
{
	const struct ironside_se_boot_report *report = IRONSIDE_SE_BOOT_REPORT;

	memcpy(uuid_bytes, &report->device_info_uuid, IRONSIDE_SE_BOOT_REPORT_UUID_SIZE);
}

static int generate_token(char attest_out[ATTEST_B64_SZ], char cose_out[COSE_B64_SZ],
			  const uint8_t *challenge)
{
	uint8_t uuid[UUID_BYTE_SZ];
	uint8_t random_nonce[APP_ATTEST_TOKEN_CHALLENGE_SZ];
	const uint8_t *nonce_ptr = challenge;
	uint8_t payload[PAYLOAD_SZ];
	uint8_t sig_struct[SIG_STRUCT_SZ];
	uint8_t signature[SIG_SZ];
	uint8_t cose[COSE_SZ];
	size_t sig_struct_len;
	size_t cose_len;
	size_t sig_len;
	psa_status_t status;
	int err = 0;

	status = psa_crypto_init();
	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_crypto_init() failed (status %d)", status);
		return -EIO;
	}

	if (nonce_ptr == NULL) {
		status = psa_generate_random(random_nonce, sizeof(random_nonce));
		if (status != PSA_SUCCESS) {
			LOG_ERR("psa_generate_random() failed (status %d)", status);
			err = -EIO;
			goto cleanup;
		}
		nonce_ptr = random_nonce;
	}

	device_info_get_uuid(uuid);

	build_payload(payload, uuid, nonce_ptr);

	sig_struct_len = build_sig_structure(sig_struct, payload);

	status = psa_sign_message(IAK_APPLICATION,
				  PSA_ALG_ECDSA(PSA_ALG_SHA_256),
				  sig_struct, sig_struct_len,
				  signature, sizeof(signature),
				  &sig_len);
	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_sign_message() failed (status %d)", status);
		err = -EIO;
		goto cleanup;
	}

	if (sig_len != SIG_SZ) {
		LOG_ERR("Unexpected signature length %zu (expected %u)", sig_len, SIG_SZ);
		err = -EIO;
		goto cleanup;
	}

	cose_len = build_cose_sign1(cose, signature);

	err = bin_to_base64url(payload, PAYLOAD_SZ, attest_out, ATTEST_B64_SZ);
	if (err) {
		goto cleanup;
	}

	err = bin_to_base64url(cose, cose_len, cose_out, COSE_B64_SZ);

cleanup:
	(void)psa_purge_key(IAK_APPLICATION);

	return err;
}

int app_attest_token_get(char *buf, size_t buf_sz, const uint8_t *challenge)
{
	char attest_b64[ATTEST_B64_SZ] = {0};
	char cose_b64[COSE_B64_SZ] = {0};
	int err;
	int len;

	if (!buf || !buf_sz) {
		return -EINVAL;
	}

	if (buf_sz < APP_ATTEST_TOKEN_BUF_SZ) {
		return -EMSGSIZE;
	}

	err = generate_token(attest_b64, cose_b64, challenge);
	if (err) {
		return err;
	}

	len = snprintf(buf, buf_sz, "%s.%s", attest_b64, cose_b64);
	if (len < 0 || (size_t)len >= buf_sz) {
		return -EMSGSIZE;
	}

	return 0;
}
