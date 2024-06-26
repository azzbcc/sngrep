/**************************************************************************
 **
 ** sngrep - SIP Messages flow viewer
 **
 ** Copyright (C) 2013-2019 Ivan Alonso (Kaian)
 ** Copyright (C) 2013-2019 Irontec SL. All rights reserved.
 **
 ** This program is free software: you can redistribute it and/or modify
 ** it under the terms of the GNU General Public License as published by
 ** the Free Software Foundation, either version 3 of the License, or
 ** (at your option) any later version.
 **
 ** This program is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 ** GNU General Public License for more details.
 **
 ** You should have received a copy of the GNU General Public License
 ** along with this program.  If not, see <http://www.gnu.org/licenses/>.
 **
 ****************************************************************************/
/**
 * @file packet_tls.c
 * @author Ivan Alonso [aka Kaian] <kaian@irontec.com>
 *
 * @brief Functions to manage TLS transport for messages
 *
 * This file contains the functions and structures to manage SSL protocol.
 *
 */
#include "config.h"
#include <glib.h>
#include <gnutls/gnutls.h>
#include "glib-extra/glib.h"
#include "capture/capture_input.h"
#include "storage/address.h"
#include "packet_ip.h"
#include "packet_tcp.h"
#include "packet_tls.h"

struct CipherData ciphers[] = {
    /*  { number, encoder,    ivlen, bits, digest, diglen, mode }, */
    { 0x002F, ENC_AES,    16, 128, DIG_SHA1,   20, MODE_CBC },   /* TLS_RSA_WITH_AES_128_CBC_SHA     */
    { 0x0035, ENC_AES256, 16, 256, DIG_SHA1,   20, MODE_CBC },   /* TLS_RSA_WITH_AES_256_CBC_SHA     */
    { 0x009d, ENC_AES256, 4,  256, DIG_SHA384, 48, MODE_GCM },   /* TLS_RSA_WITH_AES_256_GCM_SHA384  */
    { 0,      0,          0,  0,   0,          0,  0 }
};

G_DEFINE_TYPE(PacketDissectorTls, packet_dissector_tls, PACKET_TYPE_DISSECTOR)

GQuark
packet_tls_error_quark()
{
    return g_quark_from_static_string("sngrep-gnutls");
}

#define TLS_DEBUG 0

#if TLS_DEBUG == 1
static void
packet_tls_debug_print_hex(gchar *desc, gconstpointer ptr, gint len)
{
    gint i;
    guchar buff[17];
    guchar *data = (guchar *) ptr;

    printf("%s [%d]:\n", desc, len);
    if (len == 0) return;
    for (i = 0; i < len; i++) {
        if ((i % 16) == 0) {
            if (i != 0)
                printf(" |%s|\n", buff);
            printf("|");
        }
        printf(" %02x", data[i]);
        if ((data[i] < 0x20) || (data[i] > 0x7e))
            buff[i % 16] = '.';
        else
            buff[i % 16] = data[i];
        buff[(i % 16) + 1] = '\0';
    }
    while ((i % 16) != 0) {
        printf("   ");
        i++;
    }
    printf(" |%-16s|\n\n", buff);
}
#else

static void
packet_tls_debug_print_hex(G_GNUC_UNUSED gchar *desc,
                           G_GNUC_UNUSED gconstpointer ptr,
                           G_GNUC_UNUSED gint len)
{
}

#endif

static int
packet_tls_hash_function(const gchar *digest, guchar *dest, gint dlen, guchar *secret, gint sslen, guchar *seed,
                         int slen)
{
    guchar hmac[48];
    uint32_t hlen;
    gcry_md_hd_t md;
    uint32_t tmpslen;
    guchar tmpseed[slen];
    guchar *out = dest;
    gint pending = dlen;
    gint algo = gcry_md_map_name(digest);
    gint algolen = gcry_md_get_algo_dlen(algo);

    // Copy initial seed
    memcpy(tmpseed, seed, slen);
    tmpslen = slen;

    // Calculate enough data to fill destination
    while (pending > 0) {
        gcry_md_open(&md, algo, GCRY_MD_FLAG_HMAC);
        gcry_md_setkey(md, secret, sslen);
        gcry_md_write(md, tmpseed, tmpslen);
        memcpy(tmpseed, gcry_md_read(md, algo), algolen);
        tmpslen = algolen;
        gcry_md_close(md);

        gcry_md_open(&md, algo, GCRY_MD_FLAG_HMAC);
        gcry_md_setkey(md, secret, sslen);
        gcry_md_write(md, tmpseed, tmpslen);
        gcry_md_write(md, seed, slen);
        memcpy(hmac, gcry_md_read(md, algo), algolen);
        hlen = algolen;

        hlen = ((gint) hlen > pending) ? pending : (gint) hlen;
        memcpy(out, hmac, hlen);
        out += hlen;
        pending -= hlen;
    }

    return hlen;
}

static int
packet_tls_prf_function(SSLConnection *conn,
                        unsigned char *dest, int dlen, unsigned char *pre_master_secret,
                        int plen, unsigned char *label, unsigned char *seed, int slen)
{
    int i;

    if (conn->version < 3) {
        // Split the secret by half to generate MD5 and SHA secret parts
        int hplen = plen / 2 + plen % 2;
        unsigned char md5_secret[hplen];
        unsigned char sha_secret[hplen];
        memcpy(md5_secret, pre_master_secret, hplen);
        memcpy(sha_secret, pre_master_secret + plen / 2, plen / 2);

        // This vars will store the values of P_MD5 and P_SHA-1
        unsigned char h_md5[dlen];
        unsigned char h_sha[dlen];

        // Concatenate given seed to the label to get the final seed
        int llen = strlen((const char *) label);
        unsigned char fseed[slen + llen];
        memcpy(fseed, label, llen);
        memcpy(fseed + llen, seed, slen);

        // Get enough MD5 and SHA1 data to fill output len
        packet_tls_hash_function("MD5", h_md5, dlen, pre_master_secret, hplen, fseed, slen + llen);
        packet_tls_hash_function("SHA1", h_sha, dlen, pre_master_secret + hplen, hplen, fseed, slen + llen);

        // Final output will be MD5 and SHA1 X-ORed
        for (i = 0; i < dlen; i++)
            dest[i] = h_md5[i] ^ h_sha[i];

    } else {
        // Concatenate given seed to the label to get the final seed
        int llen = strlen((const char *) label);
        unsigned char fseed[slen + llen];
        memcpy(fseed, label, llen);
        memcpy(fseed + llen, seed, slen);

        // Get enough SHA data to fill output len
        switch (conn->cipher_data.digest) {
            case DIG_SHA1:
                packet_tls_hash_function("SHA256", dest, dlen, pre_master_secret, plen, fseed, slen + llen);
                break;
            case DIG_SHA256:
                packet_tls_hash_function("SHA256", dest, dlen, pre_master_secret, plen, fseed, slen + llen);
                break;
            case DIG_SHA384:
                packet_tls_hash_function("SHA384", dest, dlen, pre_master_secret, plen, fseed, slen + llen);
                break;
            default:
                break;
        }
    }

    packet_tls_debug_print_hex("PRF out", dest, dlen);
    return dlen;
}

static gboolean
packet_tls_valid_version(struct ProtocolVersion version)
{

    switch (version.major) {
        case 0x03:
            switch (version.minor) {
                case 0x01:
                case 0x02:
                case 0x03:
                    return TRUE;
                default:
                    return FALSE;
            }
        default:
            return FALSE;
    }
}

/**
 * FIXME Replace this with a tls_load_key function and use it
 * in tls_connection_create.
 *
 * Most probably we only need one context and key for all connections
 */
gboolean
packet_tls_privkey_check(const gchar *keyfile, GError **error)
{
    gnutls_x509_privkey_t key;
    int ret;

    gnutls_global_init();

    g_autoptr(GByteArray) key_bytes = g_byte_array_new();
    if (!g_file_get_contents(keyfile, (gchar **) &key_bytes->data, (gsize *) &key_bytes->len, error)) {
        return FALSE;
    }

    // Check we have read something from keyfile
    if (key_bytes->len == 0) {
        g_set_error(error,
                    TLS_ERROR,
                    TLS_ERROR_KEYFILE_EMTPY,
                    "Unable to read keyfile contents");
        return FALSE;
    }

    // Initialize keyfile structure
    ret = gnutls_x509_privkey_init(&key);
    if (ret < GNUTLS_E_SUCCESS) {
        g_set_error(error,
                    TLS_ERROR,
                    TLS_ERROR_PRIVATE_INIT,
                    "Unable to initializing keyfile: %s",
                    gnutls_strerror(ret));
        return FALSE;
    }

    // Import RSA keyfile
    gnutls_datum_t keycontent = { key_bytes->data, key_bytes->len };
    ret = gnutls_x509_privkey_import(key, &keycontent, GNUTLS_X509_FMT_PEM);
    if (ret < GNUTLS_E_SUCCESS) {
        g_set_error(error,
                    TLS_ERROR,
                    TLS_ERROR_PRIVATE_LOAD,
                    "Unable to loading keyfile: %s",
                    gnutls_strerror(ret));
        return FALSE;
    }

    // Deallocate private key data
    gnutls_x509_privkey_deinit(key);

    return TRUE;
}

static int
packet_tls_privkey_decrypt_data(gnutls_x509_privkey_t key, G_GNUC_UNUSED unsigned int flags,
                                const gnutls_datum_t *ciphertext, gnutls_datum_t *plaintext)
{
    gsize decr_len = 0, i = 0;
    gcry_sexp_t s_data = NULL, s_plain = NULL;
    gcry_mpi_t encr_mpi = NULL, text = NULL;
    gsize tmp_size;
    gnutls_datum_t rsa_datum[6];
    gcry_mpi_t rsa_params[6];
    gcry_sexp_t rsa_priv_key = NULL;

    // Extract data from RSA key
    gnutls_x509_privkey_export_rsa_raw(key,
                                       &rsa_datum[0], &rsa_datum[1], &rsa_datum[2],
                                       &rsa_datum[3], &rsa_datum[4], &rsa_datum[5]);

    // Convert to RSA params
    for (i = 0; i < 6; i++) {
        gcry_mpi_scan(&rsa_params[i], GCRYMPI_FMT_USG, rsa_datum[i].data, rsa_datum[i].size, &tmp_size);
    }

    if (gcry_mpi_cmp(rsa_params[3], rsa_params[4]) > 0)
        gcry_mpi_swap(rsa_params[3], rsa_params[4]);

    // Convert to sexp
    gcry_mpi_invm(rsa_params[5], rsa_params[3], rsa_params[4]);
    gcry_sexp_build(&rsa_priv_key, NULL,
                    "(private-key(rsa((n%m)(e%m)(d%m)(p%m)(q%m)(u%m))))",
                    rsa_params[0], rsa_params[1], rsa_params[2],
                    rsa_params[3], rsa_params[4], rsa_params[5]);

    // Free not longer required data
    for (i = 0; i < 6; i++) {
        gcry_mpi_release(rsa_params[i]);
        gnutls_free(rsa_datum[i].data);
    }

    gcry_mpi_scan(&encr_mpi, GCRYMPI_FMT_USG, ciphertext->data, ciphertext->size, NULL);
    gcry_sexp_build(&s_data, NULL, "(enc-val(rsa(a%m)))", encr_mpi);
    gcry_pk_decrypt(&s_plain, s_data, rsa_priv_key);
    text = gcry_sexp_nth_mpi(s_plain, 0, 0);
    gcry_mpi_print(GCRYMPI_FMT_USG, NULL, 0, &decr_len, text);
    gcry_mpi_print(GCRYMPI_FMT_USG, ciphertext->data, ciphertext->size, &decr_len, text);

    int pad = 0;
    for (i = 1; i < decr_len; i++) {
        if (ciphertext->data[i] == 0) {
            pad = i + 1;
            break;
        }
    }

    plaintext->size = decr_len - pad;
    plaintext->data = gnutls_malloc(plaintext->size);
    memmove(plaintext->data, ciphertext->data + pad, plaintext->size);
    gcry_sexp_release(s_data);
    gcry_sexp_release(s_plain);
    gcry_mpi_release(encr_mpi);
    gcry_mpi_release(text);
    return (int) decr_len;
}

static gboolean
packet_tls_connection_load_cipher(SSLConnection *conn)
{
    guint ciphnum = (conn->cipher_suite.cs1 << 8) | conn->cipher_suite.cs2;

    // Check if this is one of the supported ciphers
    for (guint i = 0; ciphers[i].enc; i++) {
        if (ciphnum == ciphers[i].num) {
            conn->cipher_data = ciphers[i];
            break;
        }
    }

    // Set proper cipher encoder
    switch (conn->cipher_data.enc) {
        case ENC_AES:
            conn->ciph = gcry_cipher_map_name("AES");
            break;
        case ENC_AES256:
            conn->ciph = gcry_cipher_map_name("AES256");
            break;
        default:
            return FALSE;
    }

    return TRUE;
}

static SSLConnection *
packet_tls_connection_create(Address caddr, Address saddr)
{
    SSLConnection *conn = NULL;
    gnutls_datum_t keycontent = { NULL, 0 };
    FILE *keyfp;
    gnutls_x509_privkey_t spkey;
    int ret;

    // Allocate memory for this connection
    conn = g_malloc0(sizeof(SSLConnection));

    conn->client_addr = address_new(address_get_ip(caddr), address_get_port(caddr));
    conn->server_addr = address_new(address_get_ip(saddr), address_get_port(saddr));

    gnutls_global_init();

    if (gnutls_init(&conn->ssl, GNUTLS_SERVER) < GNUTLS_E_SUCCESS)
        return NULL;

    if (!(keyfp = fopen(capture_keyfile(capture_manager_get_instance()), "rb")))
        return NULL;

    fseek(keyfp, 0, SEEK_END);
    keycontent.size = ftell(keyfp);
    fseek(keyfp, 0, SEEK_SET);
    keycontent.data = g_malloc0(keycontent.size);
    G_GNUC_UNUSED gsize rbytes = fread(keycontent.data, 1, keycontent.size, keyfp);
    fclose(keyfp);

    gnutls_x509_privkey_init(&spkey);

    // Import PEM key data
    ret = gnutls_x509_privkey_import(spkey, &keycontent, GNUTLS_X509_FMT_PEM);
    if (ret != GNUTLS_E_SUCCESS)
        return NULL;

    g_free(keycontent.data);

    // Check this is a valid RSA key
    if (gnutls_x509_privkey_get_pk_algorithm(spkey) != GNUTLS_PK_RSA)
        return NULL;

    // Store this key into the connection
    conn->server_private_key = spkey;

    return conn;
}

static void
packet_tls_connection_destroy(SSLConnection *conn)
{
    // Deallocate connection memory
    gnutls_deinit(conn->ssl);
    address_free(conn->client_addr);
    address_free(conn->server_addr);
    gnutls_x509_privkey_deinit(conn->server_private_key);
    g_free(conn->key_material.client_write_MAC_key);
    g_free(conn->key_material.server_write_MAC_key);
    g_free(conn->key_material.client_write_IV);
    g_free(conn->key_material.server_write_IV);
    g_free(conn->key_material.client_write_key);
    g_free(conn->key_material.server_write_key);
    g_free(conn);
}

static int
packet_tls_connection_dir(SSLConnection *conn, Address addr)
{
    if (addressport_equals(conn->client_addr, addr))
        return 0;
    if (addressport_equals(conn->server_addr, addr))
        return 1;
    return -1;
}

static SSLConnection *
packet_dissector_tls_connection_find(PacketDissectorTls *dissector, Address src, Address dst)
{
    for (GSList *l = dissector->connections; l != NULL; l = l->next) {
        SSLConnection *conn = l->data;

        if (packet_tls_connection_dir(conn, src) == 0 &&
            packet_tls_connection_dir(conn, dst) == 1) {
            return conn;
        }
        if (packet_tls_connection_dir(conn, src) == 1 &&
            packet_tls_connection_dir(conn, dst) == 0) {
            return conn;
        }
    }
    return NULL;
}


static GBytes *
packet_tls_process_record_decode(SSLConnection *conn, GBytes *data)
{
    gcry_cipher_hd_t *evp;
    guint8 nonce[16] = { 0 };

    packet_tls_debug_print_hex("Ciphertext", g_bytes_get_data(data, NULL), g_bytes_get_size(data));

    if (conn->direction == 0) {
        evp = &conn->client_cipher_ctx;
    } else {
        evp = &conn->server_cipher_ctx;
    }

    if (conn->cipher_data.mode == MODE_CBC) {
        // TLS 1.1 and later extract explicit IV
        if (conn->version >= 2 && g_bytes_get_size(data) > 16) {
            gcry_cipher_setiv(*evp, g_bytes_get_data(data, NULL), 16);
            data = g_bytes_offset(data, 16);
        }
    }

    if (conn->cipher_data.mode == MODE_GCM) {
        if (conn->direction == 0) {
            memcpy(nonce, conn->key_material.client_write_IV, conn->cipher_data.ivblock);
            memcpy(nonce + conn->cipher_data.ivblock, g_bytes_get_data(data, NULL), 8);
            nonce[15] = 2;
        } else {
            memcpy(nonce, conn->key_material.server_write_IV, conn->cipher_data.ivblock);
            memcpy(nonce + conn->cipher_data.ivblock, g_bytes_get_data(data, NULL), 8);
            nonce[15] = 2;
        }
        gcry_cipher_setctr(*evp, nonce, sizeof(nonce));
        data = g_bytes_offset(data, 8);
    }

    GByteArray *out = g_byte_array_sized_new(g_bytes_get_size(data));
    g_byte_array_set_size(out, g_bytes_get_size(data));
    gcry_cipher_decrypt(*evp, out->data, out->len, g_bytes_get_data(data, NULL), g_bytes_get_size(data));
    packet_tls_debug_print_hex("Plaintext", out->data, out->len);

    // Strip mac from the decoded data
    if (conn->cipher_data.mode == MODE_CBC) {
        // Get padding counter and remove from data
        guint8 pad = out->data[out->len - 1];
        g_byte_array_set_size(out, out->len - pad - 1);

        // Remove mac from data
        guint mac_len = conn->cipher_data.diglen;
        packet_tls_debug_print_hex("Mac", out->data + out->len - mac_len - 1, mac_len);
        g_byte_array_set_size(out, out->len - mac_len);
    }

    // Strip auth tag from decoded data
    if (conn->cipher_data.mode == MODE_GCM) {
        g_byte_array_remove_range(out, out->len - 16, 16);
    }

    // Return decoded data
    return g_byte_array_free_to_bytes(out);
}


static gboolean
packet_tls_record_handshake_is_ssl2(G_GNUC_UNUSED SSLConnection *conn, GBytes *data)
{
    g_return_val_if_fail(data != NULL, FALSE);

    const guint8 *content = g_bytes_get_data(data, NULL);

    // This magic belongs to wireshark people <3
    if (g_bytes_get_size(data) < 3) return 0;
    // v2 client hello should start this way
    if (content[0] != 0x80) return 0;
    // v2 client hello msg type
    if (content[2] != 0x01) return 0;
    // Seems SSLv2
    return 1;
}

static GBytes *
packet_tls_process_record_ssl2(SSLConnection *conn, GBytes *data)
{
    int record_len_len;
    uint32_t record_len;
    uint8_t record_type;
    const opaque *fragment;
    int flen;

    // No record data here!
    if (g_bytes_get_size(data) == 0)
        return NULL;

    // Record header length
    const guint8 *content = g_bytes_get_data(data, NULL);
    record_len_len = (content[0] & 0x80) ? 2 : 3;

    // Two bytes SSLv2 record length field
    if (record_len_len == 2) {
        record_len = (content[0] & 0x7f) << 8;
        record_len += (content[1]);
        record_type = content[2];
        fragment = content + 3;
        flen = record_len - 1 /* record type */;
    } else {
        record_len = (content[0] & 0x3f) << 8;
        record_len += content[1];
        record_len += content[2];
        record_type = content[3];
        fragment = content + 4;
        flen = record_len - 1 /* record type */;
    }

    // We only handle Client Hello handshake SSLv2 records
    if (record_type == 0x01 && (guint) flen > sizeof(struct ClientHelloSSLv2)) {
        // Client Hello SSLv2
        struct ClientHelloSSLv2 *clienthello = (struct ClientHelloSSLv2 *) fragment;

        // Store TLS version
        conn->version = clienthello->client_version.minor;

        // Calculate where client random starts
        const opaque *random = fragment + sizeof(struct ClientHelloSSLv2)
                               + UINT16_INT(clienthello->cipherlist_len)
                               + UINT16_INT(clienthello->sessionid_len);

        // Get Client random
        memcpy(&conn->client_random, random, sizeof(struct Random));
    }

    return data;
}

static gboolean
packet_tls_process_record_client_hello(SSLConnection *conn, GBytes *data)
{
    // Store client random
    struct ClientHello clienthello;
    memcpy(&clienthello, g_bytes_get_data(data, NULL), sizeof(struct ClientHello));

    // Store client random
    memcpy(&conn->client_random, &clienthello.random, sizeof(struct Random));

    // Check we have a TLS handshake
    if (!packet_tls_valid_version(clienthello.client_version)) {
        return FALSE;
    }

    // Store TLS version
    conn->version = clienthello.client_version.minor;

    return TRUE;
}

static gboolean
packet_tls_process_record_server_hello(SSLConnection *conn, GBytes *data)
{
    // Store server random
    struct ServerHello serverhello;
    memcpy(&serverhello, g_bytes_get_data(data, NULL), sizeof(struct ServerHello));

    memcpy(&conn->server_random, &serverhello.random, sizeof(struct Random));

    // Get the selected cipher
    const guint8 *content = g_bytes_get_data(data, NULL);
    memcpy(&conn->cipher_suite,
           content + sizeof(struct ServerHello) + serverhello.session_id_length,
           sizeof(guint16));

    // Check if we have a handled cipher
    return packet_tls_connection_load_cipher(conn);
}

static gboolean
packet_tls_process_record_key_exchange(SSLConnection *conn, GBytes *data)
{
    // Decrypt PreMasterKey
    struct ClientKeyExchange *clientkeyex = (struct ClientKeyExchange *) g_bytes_get_data(data, NULL);

    gnutls_datum_t exkeys, pms;
    exkeys.size = UINT16_INT(clientkeyex->length);
    exkeys.data = (unsigned char *) &clientkeyex->pre_master_secret;
    packet_tls_debug_print_hex("exchange keys", exkeys.data, exkeys.size);

    packet_tls_privkey_decrypt_data(conn->server_private_key, 0, &exkeys, &pms);
    if (!pms.data) return FALSE;

    memcpy(&conn->pre_master_secret, pms.data, pms.size);
    packet_tls_debug_print_hex("pre_master_secret", pms.data, pms.size);
    packet_tls_debug_print_hex("client_random", &conn->client_random, sizeof(struct Random));
    packet_tls_debug_print_hex("server_random", &conn->server_random, sizeof(struct Random));

    // Get MasterSecret
    uint8_t *seed = g_malloc0(sizeof(struct Random) * 2);
    memcpy(seed, &conn->client_random, sizeof(struct Random));
    memcpy(seed + sizeof(struct Random), &conn->server_random, sizeof(struct Random));
    packet_tls_prf_function(conn, (unsigned char *) &conn->master_secret, sizeof(struct MasterSecret),
                            (unsigned char *) &conn->pre_master_secret, sizeof(struct PreMasterSecret),
                            (unsigned char *) "master secret", seed, sizeof(struct Random) * 2);

    packet_tls_debug_print_hex("master_secret", conn->master_secret.random, sizeof(struct MasterSecret));

    memcpy(seed, &conn->server_random, sizeof(struct Random) * 2);
    memcpy(seed + sizeof(struct Random), &conn->client_random, sizeof(struct Random));

    int key_material_len = 0;
    key_material_len += conn->cipher_data.diglen * 2;
    key_material_len += conn->cipher_data.ivblock * 2;
    key_material_len += conn->cipher_data.bits / 4;

    // Generate MACs, Write Keys and IVs
    uint8_t *key_material = g_malloc0(key_material_len);
    packet_tls_prf_function(conn, key_material, key_material_len,
                            (unsigned char *) &conn->master_secret, sizeof(struct MasterSecret),
                            (unsigned char *) "key expansion", seed, sizeof(struct Random) * 2);

    // Get write mac keys
    if (conn->cipher_data.mode == MODE_GCM) {
        // AEAD ciphers
        conn->key_material.client_write_MAC_key = 0;
        conn->key_material.server_write_MAC_key = 0;
    } else {
        // Copy prf output to ssl connection key material
        int mk_len = conn->cipher_data.diglen;
        conn->key_material.client_write_MAC_key = g_malloc0(mk_len);
        memcpy(conn->key_material.client_write_MAC_key, key_material, mk_len);
        packet_tls_debug_print_hex("client_write_MAC_key", key_material, mk_len);
        key_material += mk_len;
        conn->key_material.server_write_MAC_key = g_malloc0(mk_len);
        packet_tls_debug_print_hex("server_write_MAC_key", key_material, mk_len);
        memcpy(conn->key_material.server_write_MAC_key, key_material, mk_len);
        key_material += mk_len;
    }

    // Get write keys
    int wk_len = conn->cipher_data.bits / 8;
    conn->key_material.client_write_key = g_malloc0(wk_len);
    memcpy(conn->key_material.client_write_key, key_material, wk_len);
    packet_tls_debug_print_hex("client_write_key", key_material, wk_len);
    key_material += wk_len;

    conn->key_material.server_write_key = g_malloc0(wk_len);
    memcpy(conn->key_material.server_write_key, key_material, wk_len);
    packet_tls_debug_print_hex("server_write_key", key_material, wk_len);
    key_material += wk_len;

    // Get IV blocks
    conn->key_material.client_write_IV = g_malloc0(conn->cipher_data.ivblock);
    memcpy(conn->key_material.client_write_IV, key_material, conn->cipher_data.ivblock);
    packet_tls_debug_print_hex("client_write_IV", key_material, conn->cipher_data.ivblock);
    key_material += conn->cipher_data.ivblock;
    conn->key_material.server_write_IV = g_malloc0(conn->cipher_data.ivblock);
    memcpy(conn->key_material.server_write_IV, key_material, conn->cipher_data.ivblock);
    packet_tls_debug_print_hex("server_write_IV", key_material, conn->cipher_data.ivblock);
    /* key_material+=conn->cipher_data.ivblock; */

    // Free temporally allocated memory
    g_free(seed);

    int mode = 0;
    if (conn->cipher_data.mode == MODE_CBC) {
        mode = GCRY_CIPHER_MODE_CBC;
    } else if (conn->cipher_data.mode == MODE_GCM) {
        mode = GCRY_CIPHER_MODE_CTR;
    } else {
        return FALSE;
    }

    // Create Client decoder
    gcry_cipher_open(&conn->client_cipher_ctx, conn->ciph, mode, 0);
    gcry_cipher_setkey(conn->client_cipher_ctx,
                       conn->key_material.client_write_key,
                       gcry_cipher_get_algo_keylen(conn->ciph));
    gcry_cipher_setiv(conn->client_cipher_ctx,
                      conn->key_material.client_write_IV,
                      gcry_cipher_get_algo_blklen(conn->ciph));

    // Create Server decoder
    gcry_cipher_open(&conn->server_cipher_ctx, conn->ciph, mode, 0);
    gcry_cipher_setkey(conn->server_cipher_ctx,
                       conn->key_material.server_write_key,
                       gcry_cipher_get_algo_keylen(conn->ciph));
    gcry_cipher_setiv(conn->server_cipher_ctx,
                      conn->key_material.server_write_IV,
                      gcry_cipher_get_algo_blklen(conn->ciph));

    return TRUE;
}

static gboolean
packet_tls_process_record_handshake(SSLConnection *conn, GBytes *data)
{
    // Get Handshake data
    struct Handshake handshake;
    memcpy(&handshake, g_bytes_get_data(data, NULL), sizeof(struct Handshake));
    data = g_bytes_offset(data, sizeof(struct Handshake));

    if (UINT24_INT(handshake.length) < 0) {
        return FALSE;
    }

    switch (handshake.type) {
        case GNUTLS_HANDSHAKE_HELLO_REQUEST:
            break;
        case GNUTLS_HANDSHAKE_CLIENT_HELLO:
            return packet_tls_process_record_client_hello(conn, data);
        case GNUTLS_HANDSHAKE_SERVER_HELLO:
            return packet_tls_process_record_server_hello(conn, data);
        case GNUTLS_HANDSHAKE_CERTIFICATE_PKT:
        case GNUTLS_HANDSHAKE_CERTIFICATE_REQUEST:
        case GNUTLS_HANDSHAKE_SERVER_HELLO_DONE:
        case GNUTLS_HANDSHAKE_CERTIFICATE_VERIFY:
            break;
        case GNUTLS_HANDSHAKE_CLIENT_KEY_EXCHANGE:
            return packet_tls_process_record_key_exchange(conn, data);
        case GNUTLS_HANDSHAKE_FINISHED:
            break;
        default:
            break;
    }

    return TRUE;
}

static GBytes *
packet_tls_process_record(SSLConnection *conn, GBytes *data, GBytes **out)
{
    // No record data here!
    if (g_bytes_get_size(data) == 0)
        return data;

    // Get Record data
    struct TLSPlaintext record;
    memcpy(&record, g_bytes_get_data(data, NULL), sizeof(struct TLSPlaintext));
    data = g_bytes_offset(data, sizeof(struct TLSPlaintext));

    // Process record fragment
    if (UINT16_INT(record.length) > 0) {
        if (UINT16_INT(record.length) > (int) g_bytes_get_size(data)) {
            return g_bytes_offset(data, g_bytes_get_size(data));
        }
        // TLSPlaintext fragment pointer
        g_autoptr(GBytes) fragment = g_bytes_new(g_bytes_get_data(data, NULL), g_bytes_get_size(data));
        data = g_bytes_offset(data, UINT16_INT(record.length));

        switch (record.type) {
            case HANDSHAKE:
                // Decode before parsing
                if (conn->encrypted) {
                    fragment = packet_tls_process_record_decode(conn, fragment);
                }
                // Hanshake Record, Try to get MasterSecret data
                if (!packet_tls_process_record_handshake(conn, fragment))
                    return NULL;
                break;
            case CHANGE_CIPHER_SPEC:
                // From now on, this connection will be encrypted using MasterSecret
                if (conn->client_cipher_ctx && conn->server_cipher_ctx)
                    conn->encrypted = 1;
                break;
            case APPLICATION_DATA:
                if (conn->encrypted) {
                    // Decrypt application data using MasterSecret
                    *out = packet_tls_process_record_decode(conn, fragment);
                }
                break;
            default:
                return data;
        }
    }

    return data;
}

static GBytes *
packet_dissector_tls_dissect(PacketDissector *self, Packet *packet, GBytes *data)
{
    SSLConnection *conn = NULL;
    GBytes *out = NULL;

    // Get TLS dissector information
    g_return_val_if_fail(PACKET_DISSECTOR_IS_TLS(self), NULL);
    PacketDissectorTls *dissector = PACKET_DISSECTOR_TLS(self);

    // Get manager information
    CaptureManager *manager = capture_manager_get_instance();
    if (capture_keyfile(manager) == NULL) {
        return data;
    }

    Address tlsserver = capture_tls_server(manager);

    // Get TCP/IP data from this packet
    PacketTcpData *tcpdata = packet_get_protocol_data(packet, PACKET_PROTO_TCP);
    g_return_val_if_fail(tcpdata != NULL, NULL);

    // Get packet addresses
    Address src = packet_src_address(packet);
    Address dst = packet_dst_address(packet);

    // Try to find a session for this ip
    if ((conn = packet_dissector_tls_connection_find(dissector, src, dst))) {
        // Update last connection direction
        conn->direction = packet_tls_connection_dir(conn, src);

        // Check current connection state
        switch (conn->state) {
            case TCP_STATE_SYN:
                // First SYN received, this package must be SYN/ACK
                if (tcpdata->syn == 1 && tcpdata->ack == 1)
                    conn->state = TCP_STATE_SYN_ACK;
                break;
            case TCP_STATE_SYN_ACK:
                // We expect an ACK packet here
                if (tcpdata->syn == 0 && tcpdata->ack == 1)
                    conn->state = TCP_STATE_ESTABLISHED;
                break;
            case TCP_STATE_ACK:
            case TCP_STATE_ESTABLISHED:
                // Check if we have a SSLv2 Handshake
                if (packet_tls_record_handshake_is_ssl2(conn, data)) {
                    out = packet_tls_process_record_ssl2(conn, data);
                    if (out == NULL) {
                        dissector->connections = g_slist_remove(dissector->connections, conn);
                        packet_tls_connection_destroy(conn);
                    }
                } else {
                    // Process data segment!
                    while (g_bytes_get_size(data) > 0) {
                        data = packet_tls_process_record(conn, data, &out);
                        if (data == NULL) {
                            dissector->connections = g_slist_remove(dissector->connections, conn);
                            packet_tls_connection_destroy(conn);
                            break;
                        }
                    }
                }

                // This seems a SIP TLS packet ;-)
                if (out != NULL && g_bytes_get_size(out) > 0) {
                    return packet_dissector_next(self, packet, out);
                }
                break;
            case TCP_STATE_FIN:
            case TCP_STATE_CLOSED:
                // We can delete this connection
                dissector->connections = g_slist_remove(dissector->connections, conn);
                packet_tls_connection_destroy(conn);
                break;
        }
    } else {
        if (tcpdata->syn != 0 && tcpdata->ack == 0) {
            // Only create new connections whose destination is tlsserver
            if (address_get_ip(tlsserver) && address_get_port(tlsserver)) {
                if (addressport_equals(tlsserver, dst)) {
                    // New connection, store it status and leave
                    dissector->connections =
                        g_slist_append(dissector->connections, packet_tls_connection_create(src, dst));
                }
            } else {
                // New connection, store it status and leave
                dissector->connections =
                    g_slist_append(dissector->connections, packet_tls_connection_create(src, dst));
            }
        } else {
            return data;
        }
    }

    return data;
}

static void
packet_dissector_tls_class_init(PacketDissectorTlsClass *klass)
{
    PacketDissectorClass *dissector_class = PACKET_DISSECTOR_CLASS(klass);
    dissector_class->dissect = packet_dissector_tls_dissect;
}

static void
packet_dissector_tls_init(PacketDissectorTls *self)
{
    // TLS Dissector base information
    packet_dissector_add_subdissector(PACKET_DISSECTOR(self), PACKET_PROTO_WS);
    packet_dissector_add_subdissector(PACKET_DISSECTOR(self), PACKET_PROTO_SIP);
}

PacketDissector *
packet_dissector_tls_new()
{
    return g_object_new(
        PACKET_DISSECTOR_TYPE_TLS,
        "id", PACKET_PROTO_TLS,
        "name", "TLS",
        NULL
    );
}
