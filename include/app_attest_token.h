/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef APP_ATTEST_TOKEN_H__
#define APP_ATTEST_TOKEN_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file app_attest_token.h
 *
 * @brief Generate a device attestation token on the application core.
 * @defgroup app_attest_token Application attestation token
 * @{
 *
 */

/** Challenge size in bytes. */
#define APP_ATTEST_TOKEN_CHALLENGE_SZ 16

/** Maximum length of the attestation token string, excluding the terminating NULL. */
#define APP_ATTEST_TOKEN_STR_MAX_LEN 180

/** Minimum buffer size for the attestation token string, including the terminating NULL. */
#define APP_ATTEST_TOKEN_BUF_SZ (APP_ATTEST_TOKEN_STR_MAX_LEN + 1)

/**
 * @brief Generate a device attestation token on the application core.
 *
 * The token is written to @p buf as a NULL-terminated string in the form
 * ``base64url1.base64url2``, where the first part is the Device Identity
 * Attestation CBOR payload and the second part is the COSE authentication
 * metadata.
 *
 * @param[out] buf       Buffer receiving the NULL-terminated token string.
 * @param[in]  buf_sz    Size of @p buf, at least APP_ATTEST_TOKEN_BUF_SZ.
 * @param[in]  challenge Optional APP_ATTEST_TOKEN_CHALLENGE_SZ byte challenge to
 *                       embed as the token nonce. If NULL, a random nonce is
 *                       generated.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p buf is NULL or @p buf_sz is zero.
 * @retval -EMSGSIZE if @p buf is too small.
 * @retval -EIO on a crypto failure.
 */
int app_attest_token_get(char *buf, size_t buf_sz, const uint8_t *challenge);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_ATTEST_TOKEN_H__ */
