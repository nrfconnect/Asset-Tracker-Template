/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <modem/modem_key_mgmt.h>
#include <modem/lte_lc.h>
#include <errno.h>
#include <string.h>
#include "celestrak_client.h"
#include "certificates.h"

LOG_MODULE_REGISTER(celestrak_client, CONFIG_APP_LOG_LEVEL);

#define HTTPS_PORT "443"
#define CELESTRAK_HOST "celestrak.org"

#define TLS_SEC_TAG 42
#define TLS_PEER_HOSTNAME "celestrak.org"
#define RECV_BUFFER_SIZE 1024
#define HTTP_TIMEOUT_MS 30000
#define MAX_RETRIES 3
#define RETRY_DELAY_MS 2000
#define SOCKET_TIMEOUT_MS 10000

#define HTTP_REQUEST_TEMPLATE \
    "GET /NORAD/elements/gp.php?CATNR=%s&FORMAT=TLE HTTP/1.1\r\n" \
    "Host: %s\r\n" \
    "Connection: close\r\n\r\n"

#define HTTP_HEADER_END "\r\n\r\n"
#define URL_BUFFER_SIZE 256

K_MUTEX_DEFINE(celestrak_mutex);

/* Buffer for HTTP request */
static char http_request[512];

/* TLS setup function */
static int tls_setup(int sock)
{
    int err;
    int verify = 2; /* REQUIRED */
    sec_tag_t sec_tag_list[] = { TLS_SEC_TAG };

    err = setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify));
    if (err) {
        LOG_ERR("Failed to setup peer verification, err: %d, errno: %d", err, errno);
        return -errno;
    }

    err = setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST, sec_tag_list, sizeof(sec_tag_list));
    if (err) {
        LOG_ERR("Failed to setup TLS sec tag, err: %d, errno: %d", err, errno);
        return -errno;
    }

    err = setsockopt(sock, SOL_TLS, TLS_HOSTNAME, TLS_PEER_HOSTNAME, strlen(TLS_PEER_HOSTNAME));
    if (err) {
        LOG_ERR("Failed to setup TLS hostname, err: %d, errno: %d", err, errno);
        return -errno;
    }

    struct timeval timeout = {
        .tv_sec = SOCKET_TIMEOUT_MS / 1000,
        .tv_usec = (SOCKET_TIMEOUT_MS % 1000) * 1000,
    };

    err = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (err) {
        LOG_ERR("Failed to setup receive timeout, err: %d, errno: %d", err, errno);
        return -errno;
    }

    err = setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (err) {
        LOG_ERR("Failed to setup send timeout, err: %d, errno: %d", err, errno);
        return -errno;
    }

    return 0;
}

static bool initialized;

/* Parse HTTP response and extract status code */
static int parse_http_response(const char *response, size_t len, int *status_code)
{
    char status_str[4] = {0};
    const char *status_start;

    if (len < 12 || strncmp(response, "HTTP/1.", 7) != 0) {
        return -EINVAL;
    }

    status_start = response + 9;
    memcpy(status_str, status_start, 3);
    *status_code = atoi(status_str);

    return 0;
}

/* Find the start of response body (after headers) */
static const char *find_response_body(const char *response, size_t len)
{
    const char *body = strstr(response, HTTP_HEADER_END);
    if (body) {
        return body + strlen(HTTP_HEADER_END);
    }
    return NULL;
}

int celestrak_fetch_tle(const char *catnr, char *buffer, size_t buffer_size, size_t *bytes_written)
{
    int err;
    int sock;
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = IPPROTO_TLS_1_2
    };

    if (!initialized) {
        LOG_ERR("Celestrak client not initialized");
        return -EPERM;
    }

    if (!buffer || buffer_size == 0 || !bytes_written) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    err = k_mutex_lock(&celestrak_mutex, K_MSEC(5000));
    if (err) {
        LOG_ERR("Failed to acquire mutex: %d", err);
        return err;
    }

    /* Resolve the Celestrak hostname */
    struct addrinfo *addr;
    err = getaddrinfo(CELESTRAK_HOST, HTTPS_PORT, &hints, &addr);
    if (err) {
        LOG_ERR("Failed to resolve %s: %d", CELESTRAK_HOST, err);
        k_mutex_unlock(&celestrak_mutex);
        return -EHOSTUNREACH;
    }

    /* Create socket and connect with retries */
    int retries = 0;
    do {
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
        if (sock >= 0) {
            /* Configure TLS settings */
            err = tls_setup(sock);
            if (err) {
                LOG_ERR("Failed to setup TLS: %d", err);
                close(sock);
                sock = -1;
                continue;
            }

            /* Set socket timeouts */
            struct timeval timeout = {
                .tv_sec = SOCKET_TIMEOUT_MS / 1000,
                .tv_usec = (SOCKET_TIMEOUT_MS % 1000) * 1000,
            };

            err = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            if (err) {
                LOG_WRN("Failed to set SO_RCVTIMEO: %d", errno);
                close(sock);
                sock = -1;
                continue;
            }

            err = setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            if (err) {
                LOG_WRN("Failed to set SO_SNDTIMEO: %d", errno);
                close(sock);
                sock = -1;
                continue;
            }
        }

        if (sock < 0) {
            LOG_ERR("Failed to create socket: %d", errno);
            freeaddrinfo(addr);
            k_mutex_unlock(&celestrak_mutex);
            return -errno;
        }

        err = connect(sock, addr->ai_addr, addr->ai_addrlen);
        if (err && retries < MAX_RETRIES) {
            LOG_WRN("Connection attempt %d failed, retrying...", retries + 1);
            close(sock);
            k_sleep(K_MSEC(RETRY_DELAY_MS));
        }

    } while (err && ++retries < MAX_RETRIES);

    if (err) {
        LOG_ERR("Failed to connect after %d retries: %d", MAX_RETRIES, errno);
        close(sock);
        freeaddrinfo(addr);
        k_mutex_unlock(&celestrak_mutex);
        return -errno;
    }

    LOG_INF("Connected to %s after %d attempt(s)", CELESTRAK_HOST, retries + 1);

    freeaddrinfo(addr);

    /* Prepare and send HTTP request */
    snprintf(http_request, sizeof(http_request), HTTP_REQUEST_TEMPLATE, catnr, CELESTRAK_HOST);
    
    size_t request_len = strlen(http_request);
    size_t sent = 0;
    while (sent < request_len) {
        int bytes = send(sock, http_request + sent, request_len - sent, 0);
        if (bytes < 0) {
            LOG_ERR("Failed to send HTTP request: %d", errno);
            close(sock);
            k_mutex_unlock(&celestrak_mutex);
            return -errno;
        }
        sent += bytes;
    }

    /* Receive and process response */
    char recv_buf[RECV_BUFFER_SIZE];
    size_t total_received = 0;
    bool headers_processed = false;
    int status_code = 0;

    while (1) {
        int bytes = recv(sock, recv_buf + total_received, 
                        sizeof(recv_buf) - total_received - 1, 0);
        if (bytes < 0) {
            LOG_ERR("Failed to receive response: %d", errno);
            close(sock);
            k_mutex_unlock(&celestrak_mutex);
            return -errno;
        }
        if (bytes == 0) {
            break;  /* Connection closed by server */
        }

        total_received += bytes;
        recv_buf[total_received] = '\0';

        if (!headers_processed) {
            /* Process headers once we have them */
            const char *body = find_response_body(recv_buf, total_received);
            if (body) {
                headers_processed = true;
                
                /* Parse status code */
                err = parse_http_response(recv_buf, total_received, &status_code);
                if (err || status_code != 200) {
                    LOG_ERR("HTTP error: %d", status_code);
                    close(sock);
                    k_mutex_unlock(&celestrak_mutex);
                    return -EPROTO;
                }

                /* Copy body to output buffer */
                size_t body_len = total_received - (body - recv_buf);
                if (body_len > buffer_size - 1) {
                    body_len = buffer_size - 1;
                }
                memcpy(buffer, body, body_len);
                buffer[body_len] = '\0';
                *bytes_written = body_len;
            }
        }
    }

    /* Close socket directly since shutdown is not supported */
    close(sock);
    k_mutex_unlock(&celestrak_mutex);

    if (!headers_processed || *bytes_written == 0) {
        LOG_ERR("No valid response received");
        return -ENODATA;
    }

    return 0;
}

int celestrak_client_init(void)
{
    int err;

    /* Initialize HTTP request buffer */
    memset(http_request, 0, sizeof(http_request));

#if CONFIG_MODEM_KEY_MGMT
    bool exists;
    int mismatch;

    /* Check if certificate exists and compare */
    err = modem_key_mgmt_exists(TLS_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, &exists);
    if (err) {
        LOG_ERR("Failed to check for certificates: %d", err);
        return err;
    }

    if (exists) {
        mismatch = modem_key_mgmt_cmp(TLS_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                                     TLS_CA_CERTIFICATE, strlen(TLS_CA_CERTIFICATE));
        if (!mismatch) {
            LOG_INF("Certificate already exists and matches");
            initialized = true;
            return 0;
        }

        LOG_INF("Certificate mismatch, deleting existing certificate");
        err = modem_key_mgmt_delete(TLS_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN);
        if (err) {
            LOG_ERR("Failed to delete existing certificate: %d", err);
            return err;
        }
    }

    /* Write the CA certificate to modem */
    LOG_INF("Writing new certificate to modem");
    err = modem_key_mgmt_write(TLS_SEC_TAG,
                              MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                              TLS_CA_CERTIFICATE,
                              strlen(TLS_CA_CERTIFICATE));
    if (err) {
        LOG_ERR("Failed to write CA certificate: %d", err);
        return err;
    }
#else
    err = tls_credential_add(TLS_SEC_TAG,
                           TLS_CREDENTIAL_CA_CERTIFICATE,
                           TLS_CA_CERTIFICATE,
                           strlen(TLS_CA_CERTIFICATE));
    if (err == -EEXIST) {
        LOG_INF("Certificate already exists");
    } else if (err < 0) {
        LOG_ERR("Failed to register CA certificate: %d", err);
        return err;
    }
#endif

    initialized = true;
    LOG_INF("Celestrak client initialized");
    return 0;
}
