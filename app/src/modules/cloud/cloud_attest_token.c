/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <modem/modem_attest_token.h>
#include <app_attest_token.h>

/* nRF Cloud Provisioning gets the attestation token with modem_attest_token_get(), which
 * sends AT%ATTESTTOKEN with nrf_modem_at_scanf(). Custom AT commands only apply to
 * nrf_modem_at_cmd(), so the command would reach the modem, which does not support it.
 * Replace the function with one that takes the token from app_attest_token instead,
 * keeping the buffer ownership contract of the original.
 */
int __wrap_modem_attest_token_get(struct nrf_attestation_token *const token)
{
	char buf[APP_ATTEST_TOKEN_BUF_SZ];
	char *cose;
	size_t attest_sz;
	size_t cose_sz;
	bool attest_alloc = false;
	int err;

	if (!token) {
		return -EINVAL;
	} else if ((token->attest && !token->attest_sz) ||
		   (token->cose && !token->cose_sz)) {
		return -EBADF;
	}

	err = app_attest_token_get(buf, sizeof(buf), NULL);
	if (err) {
		return err;
	}

	cose = strchr(buf, '.');
	if (!cose) {
		return -EBADMSG;
	}

	*cose++ = '\0';

	attest_sz = strlen(buf) + 1;
	cose_sz = strlen(cose) + 1;

	if (((token->attest) && (token->attest_sz < attest_sz)) ||
	    ((token->cose) && (token->cose_sz < cose_sz))) {
		return -EMSGSIZE;
	}

	if (!token->attest) {
		token->attest = k_calloc(attest_sz, 1);
		if (!token->attest) {
			return -ENOMEM;
		}

		attest_alloc = true;
		token->attest_sz = attest_sz;
	}

	if (!token->cose) {
		token->cose = k_calloc(cose_sz, 1);
		if (!token->cose) {
			if (attest_alloc) {
				k_free(token->attest);
				token->attest = NULL;
				token->attest_sz = 0;
			}

			return -ENOMEM;
		}

		token->cose_sz = cose_sz;
	}

	memcpy(token->attest, buf, attest_sz);
	memcpy(token->cose, cose, cose_sz);

	return 0;
}
