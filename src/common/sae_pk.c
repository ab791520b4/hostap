/*
 * SAE-PK
 * Copyright (c) 2020, The Linux Foundation
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "utils/includes.h"
#include <stdint.h>

#include "utils/common.h"
#include "utils/base64.h"
#include "common/ieee802_11_defs.h"
#include "common/ieee802_11_common.h"
#include "crypto/crypto.h"
#include "crypto/aes.h"
#include "crypto/aes_siv.h"
#include "sae.h"


/* RFC 4648 base 32 alphabet with lowercase characters */
static const char *sae_pk_base32_table = "abcdefghijklmnopqrstuvwxyz234567";


bool sae_pk_valid_password(const char *pw)
{
	int pos;

	if (os_strlen(pw) < 9) {
		 /* Not long enough to meet the minimum required resistance to
		  * preimage attacks, so do not consider this valid for SAE-PK.
		  */
		return false;
	}

	for (pos = 0; pw[pos]; pos++) {
		if (pos && pos % 5 == 4) {
			if (pw[pos] != '-')
				return false;
			continue;
		}
		if (!os_strchr(sae_pk_base32_table, pw[pos]))
			return false;
	}
	if (pos == 0)
		return false;
	return pw[pos - 1] != '-';
}


static char * add_char(const char *start, char *pos, u8 idx, size_t *bits)
{
	if (*bits == 0)
		return pos;
	if (*bits > 5)
		*bits -= 5;
	else
		*bits = 0;

	if ((pos - start) % 5 == 4)
		*pos++ = '-';
	*pos++ = sae_pk_base32_table[idx];
	return pos;
}


char * sae_pk_base32_encode(const u8 *src, size_t len_bits)
{
	char *out, *pos;
	size_t olen, extra_pad, i;
	u64 block = 0;
	u8 val;
	size_t len = (len_bits + 7) / 8;
	size_t left = len_bits;
	int j;

	if (len == 0 || len >= SIZE_MAX / 8)
		return NULL;
	olen = len * 8 / 5 + 1;
	olen += olen / 4; /* hyphen separators */
	pos = out = os_zalloc(olen + 1);
	if (!out)
		return NULL;

	extra_pad = (5 - len % 5) % 5;
	for (i = 0; i < len + extra_pad; i++) {
		val = i < len ? src[i] : 0;
		block <<= 8;
		block |= val;
		if (i % 5 == 4) {
			for (j = 7; j >= 0; j--)
				pos = add_char(out, pos,
					       (block >> j * 5) & 0x1f, &left);
			block = 0;
		}
	}

	return out;
}


u8 * sae_pk_base32_decode(const char *src, size_t len, size_t *out_len)
{
	u8 dtable[256], *out, *pos, tmp;
	u64 block = 0;
	size_t i, count, olen;
	int pad = 0;
	size_t extra_pad;

	os_memset(dtable, 0x80, 256);
	for (i = 0; sae_pk_base32_table[i]; i++)
		dtable[(u8) sae_pk_base32_table[i]] = i;
	dtable['='] = 0;

	count = 0;
	for (i = 0; i < len; i++) {
		if (dtable[(u8) src[i]] != 0x80)
			count++;
	}

	if (count == 0)
		return NULL;
	extra_pad = (8 - count % 8) % 8;

	olen = (count + extra_pad) / 8 * 5;
	pos = out = os_malloc(olen);
	if (!out)
		return NULL;

	count = 0;
	for (i = 0; i < len + extra_pad; i++) {
		u8 val;

		if (i >= len)
			val = '=';
		else
			val = src[i];
		tmp = dtable[val];
		if (tmp == 0x80)
			continue;

		if (val == '=')
			pad++;
		block <<= 5;
		block |= tmp;
		count++;
		if (count == 8) {
			*pos++ = (block >> 32) & 0xff;
			*pos++ = (block >> 24) & 0xff;
			*pos++ = (block >> 16) & 0xff;
			*pos++ = (block >> 8) & 0xff;
			*pos++ = block & 0xff;
			count = 0;
			block = 0;
			if (pad) {
				/* Leave in all the available bits with zero
				 * padding to full octets from right. */
				pos -= pad * 5 / 8;
				break;
			}
		}
	}

	*out_len = pos - out;
	return out;
}


int sae_pk_set_password(struct sae_data *sae, const char *password)
{
	struct sae_temporary_data *tmp = sae->tmp;
	size_t len;

	len = os_strlen(password);
	if (!tmp || len < 1)
		return -1;

	bin_clear_free(tmp->pw, tmp->pw_len);
	tmp->pw = sae_pk_base32_decode(password, len, &tmp->pw_len);
	tmp->lambda = len - len / 5;
	return tmp->pw ? 0 : -1;
}


static size_t sae_group_2_hash_len(int group)
{
	switch (group) {
	case 19:
		return 32;
	case 20:
		return 48;
	case 21:
		return 64;
	}

	return 0;
}


void sae_deinit_pk(struct sae_pk *pk)
{
	if (pk) {
		wpabuf_free(pk->m);
		crypto_ec_key_deinit(pk->key);
		wpabuf_free(pk->pubkey);
		os_free(pk);
	}
}


struct sae_pk * sae_parse_pk(const char *val)
{
	struct sae_pk *pk;
	const char *pos;
	size_t len;
	unsigned char *der;
	size_t der_len;

	/* <m-as-hexdump>:<base64-encoded-DER-encoded-key> */

	pos = os_strchr(val, ':');
	if (!pos || (pos - val) & 0x01)
		return NULL;
	len = (pos - val) / 2;
	if (len != SAE_PK_M_LEN) {
		wpa_printf(MSG_INFO, "SAE: Unexpected Modifier M length %zu",
			   len);
		return NULL;
	}

	pk = os_zalloc(sizeof(*pk));
	if (!pk)
		return NULL;
	pk->m = wpabuf_alloc(len);
	if (!pk->m || hexstr2bin(val, wpabuf_put(pk->m, len), len)) {
		wpa_printf(MSG_INFO, "SAE: Failed to parse m");
		goto fail;
	}

	pos++;
	der = base64_decode(pos, os_strlen(pos), &der_len);
	if (!der) {
		wpa_printf(MSG_INFO, "SAE: Failed to base64 decode PK key");
		goto fail;
	}

	pk->key = crypto_ec_key_parse_priv(der, der_len);
	bin_clear_free(der, der_len);
	if (!pk->key)
		goto fail;
	pk->group = crypto_ec_key_group(pk->key);
	pk->pubkey = crypto_ec_key_get_subject_public_key(pk->key);
	if (!pk->pubkey)
		goto fail;

	return pk;
fail:
	sae_deinit_pk(pk);
	return NULL;
}


int sae_hash(size_t hash_len, const u8 *data, size_t len, u8 *hash)
{
	if (hash_len == 32)
		return sha256_vector(1, &data, &len, hash);
#ifdef CONFIG_SHA384
	if (hash_len == 48)
		return sha384_vector(1, &data, &len, hash);
#endif /* CONFIG_SHA384 */
#ifdef CONFIG_SHA512
	if (hash_len == 64)
		return sha512_vector(1, &data, &len, hash);
#endif /* CONFIG_SHA512 */
	return -1;
}


static int sae_pk_hash_sig_data(struct sae_data *sae, size_t hash_len,
				bool ap, const u8 *m, size_t m_len,
				const u8 *pubkey, size_t pubkey_len, u8 *hash)
{
	struct sae_temporary_data *tmp = sae->tmp;
	struct wpabuf *sig_data;
	u8 *pos;
	int ret = -1;

	/* Signed data for KeyAuth: eleAP || eleSTA || scaAP || scaSTA ||
	 * M || K_AP || AP-BSSID || STA-MAC */
	sig_data = wpabuf_alloc(tmp->prime_len * 6 + m_len + pubkey_len +
				2 * ETH_ALEN);
	if (!sig_data)
		goto fail;
	pos = wpabuf_put(sig_data, 2 * tmp->prime_len);
	if (crypto_ec_point_to_bin(tmp->ec, ap ? tmp->own_commit_element_ecc :
				   tmp->peer_commit_element_ecc,
				   pos, pos + tmp->prime_len) < 0)
		goto fail;
	pos = wpabuf_put(sig_data, 2 * tmp->prime_len);
	if (crypto_ec_point_to_bin(tmp->ec, ap ? tmp->peer_commit_element_ecc :
				   tmp->own_commit_element_ecc,
				   pos, pos + tmp->prime_len) < 0)
		goto fail;
	if (crypto_bignum_to_bin(ap ? tmp->own_commit_scalar :
				 sae->peer_commit_scalar,
				 wpabuf_put(sig_data, tmp->prime_len),
				 tmp->prime_len, tmp->prime_len) < 0 ||
	    crypto_bignum_to_bin(ap ? sae->peer_commit_scalar :
				 tmp->own_commit_scalar,
				 wpabuf_put(sig_data, tmp->prime_len),
				 tmp->prime_len, tmp->prime_len) < 0)
		goto fail;
	wpabuf_put_data(sig_data, m, m_len);
	wpabuf_put_data(sig_data, pubkey, pubkey_len);
	wpabuf_put_data(sig_data, ap ? tmp->own_addr : tmp->peer_addr,
			ETH_ALEN);
	wpabuf_put_data(sig_data, ap ? tmp->peer_addr : tmp->own_addr,
			ETH_ALEN);
	wpa_hexdump_buf_key(MSG_DEBUG, "SAE-PK: Data to be signed for KeyAuth",
			    sig_data);
	if (sae_hash(hash_len, wpabuf_head(sig_data), wpabuf_len(sig_data),
		     hash) < 0)
		goto fail;
	wpa_hexdump(MSG_DEBUG, "SAE-PK: hash(data to be signed)",
		    hash, hash_len);
	ret = 0;
fail:
	wpabuf_free(sig_data);
	return ret;
}


int sae_write_confirm_pk(struct sae_data *sae, struct wpabuf *buf)
{
	struct sae_temporary_data *tmp = sae->tmp;
	struct wpabuf *elem = NULL, *sig = NULL;
	size_t extra;
	int ret = -1;
	u8 *encr_mod;
	size_t encr_mod_len;
	const struct sae_pk *pk;
	u8 hash[SAE_MAX_HASH_LEN];
	size_t hash_len;

	if (!tmp)
		return -1;

	pk = tmp->ap_pk;
	if (!pk)
		return 0;

	if (tmp->kek_len != 32 && tmp->kek_len != 48 && tmp->kek_len != 64) {
		wpa_printf(MSG_INFO, "SAE-PK: No KEK available for confirm");
		return -1;
	}

	if (!tmp->ec) {
		/* Only ECC groups are supported for SAE-PK in the current
		 * implementation. */
		wpa_printf(MSG_INFO,
			   "SAE-PK: SAE commit did not use an ECC group");
		return -1;
	}

	hash_len = sae_group_2_hash_len(pk->group);
	if (sae_pk_hash_sig_data(sae, hash_len, true, wpabuf_head(pk->m),
				 wpabuf_len(pk->m), wpabuf_head(pk->pubkey),
				 wpabuf_len(pk->pubkey), hash) < 0)
		goto fail;
	sig = crypto_ec_key_sign(pk->key, hash, hash_len);
	if (!sig)
		goto fail;
	wpa_hexdump_buf(MSG_DEBUG, "SAE-PK: KeyAuth = Sig_AP()", sig);

	elem = wpabuf_alloc(1500 + wpabuf_len(sig));
	if (!elem)
		goto fail;

	/* EncryptedModifier = AES-SIV-Q(M); no AAD */
	encr_mod_len = wpabuf_len(pk->m) + AES_BLOCK_SIZE;
	wpabuf_put_u8(elem, encr_mod_len);
	encr_mod = wpabuf_put(elem, encr_mod_len);
	if (aes_siv_encrypt(tmp->kek, tmp->kek_len,
			    wpabuf_head(pk->m), wpabuf_len(pk->m),
			    0, NULL, NULL, encr_mod) < 0)
		goto fail;
	wpa_hexdump(MSG_DEBUG, "SAE-PK: EncryptedModifier",
		    encr_mod, encr_mod_len);

	/* FILS Public Key element */
	wpabuf_put_u8(elem, WLAN_EID_EXTENSION);
	wpabuf_put_u8(elem, 2 + wpabuf_len(pk->pubkey));
	wpabuf_put_u8(elem, WLAN_EID_EXT_FILS_PUBLIC_KEY);
	wpabuf_put_u8(elem, 3); /* Key Type: ECDSA public key */
	wpabuf_put_buf(elem, pk->pubkey);

	/* FILS Key Confirmation element (KeyAuth) */
	wpabuf_put_u8(elem, WLAN_EID_EXTENSION);
	wpabuf_put_u8(elem, 1 + wpabuf_len(sig));
	wpabuf_put_u8(elem, WLAN_EID_EXT_FILS_KEY_CONFIRM);
	/* KeyAuth = Sig_AP(eleAP || eleSTA || scaAP || scaSTA || M || K_AP ||
	 *                  AP-BSSID || STA-MAC) */
	wpabuf_put_buf(elem, sig);

	/* TODO: fragmentation */
	extra = 6; /* Vendor specific element header */

	if (wpabuf_tailroom(elem) < extra + wpabuf_len(buf)) {
		wpa_printf(MSG_INFO,
			   "SAE-PK: No room in message buffer for SAE-PK element (%zu < %zu)",
			   wpabuf_tailroom(buf), extra + wpabuf_len(buf));
		goto fail;
	}

	/* SAE-PK element */
	wpabuf_put_u8(buf, WLAN_EID_VENDOR_SPECIFIC);
	wpabuf_put_u8(buf, 4 + wpabuf_len(elem));
	wpabuf_put_be32(buf, SAE_PK_IE_VENDOR_TYPE);
	wpabuf_put_buf(buf, elem);

	ret = 0;
fail:
	wpabuf_free(elem);
	wpabuf_free(sig);
	return ret;

}


static bool sae_pk_valid_fingerprint(struct sae_data *sae,
				     const u8 *m, size_t m_len,
				     const u8 *k_ap, size_t k_ap_len, int group)
{
	struct sae_temporary_data *tmp = sae->tmp;
	size_t sec, i;
	u8 *fingerprint_exp, *hash_data, *pos;
	size_t hash_len, hash_data_len, fingerprint_bits, fingerprint_bytes;
	u8 hash[SAE_MAX_HASH_LEN];
	int res;

	if (!tmp->pw || tmp->pw_len < 1) {
		wpa_printf(MSG_DEBUG,
			   "SAE-PK: No PW available for K_AP fingerprint check");
		return false;
	}

	/* Fingerprint = L(Hash(SSID || M || K_AP), 0, 8*Sec + 5*Lambda - 2) */

	hash_len = sae_group_2_hash_len(group);
	hash_data_len = tmp->ssid_len + m_len + k_ap_len;
	hash_data = os_malloc(hash_data_len);
	if (!hash_data)
		return false;
	pos = hash_data;
	os_memcpy(pos, tmp->ssid, tmp->ssid_len);
	pos += tmp->ssid_len;
	os_memcpy(pos, m, m_len);
	pos += m_len;
	os_memcpy(pos, k_ap, k_ap_len);

	wpa_hexdump_key(MSG_DEBUG, "SAE-PK: SSID || M || K_AP",
			hash_data, hash_data_len);
	res = sae_hash(hash_len, hash_data, hash_data_len, hash);
	bin_clear_free(hash_data, hash_data_len);
	if (res < 0)
		return false;
	wpa_hexdump(MSG_DEBUG, "SAE-PK: Hash(SSID || M || K_AP)",
		    hash, hash_len);

	wpa_hexdump_key(MSG_DEBUG, "SAE-PK: PW", tmp->pw, tmp->pw_len);
	sec = (tmp->pw[0] >> 6) + 2;
	fingerprint_bits = 8 * sec + 5 * tmp->lambda - 2;
	wpa_printf(MSG_DEBUG, "SAE-PK: Sec=%zu Lambda=%zu fingerprint_bits=%zu",
		   sec, tmp->lambda, fingerprint_bits);
	if (fingerprint_bits > hash_len * 8) {
		wpa_printf(MSG_INFO,
			   "SAE-PK: Not enough hash output bits for the fingerprint");
		return false;
	}
	fingerprint_bytes = (fingerprint_bits + 7) / 8;
	if (fingerprint_bits % 8) {
		size_t extra;

		/* Zero out the extra bits in the last octet */
		extra = 8 - fingerprint_bits % 8;
		pos = &hash[fingerprint_bits / 8];
		*pos = (*pos >> extra) << extra;
	}
	wpa_hexdump(MSG_DEBUG, "SAE-PK: Fingerprint", hash, fingerprint_bytes);

	fingerprint_exp = os_zalloc(sec + tmp->pw_len);
	if (!fingerprint_exp)
		return false;
	pos = fingerprint_exp + sec;
	for (i = 0; i < tmp->pw_len; i++) {
		u8 next = i + 1 < tmp->pw_len ? tmp->pw[i + 1] : 0;

		*pos++ = tmp->pw[i] << 2 | next >> 6;
	}

	wpa_hexdump(MSG_DEBUG, "SAE-PK: Fingerprint_Expected",
		    fingerprint_exp, fingerprint_bytes);
	res = os_memcmp_const(hash, fingerprint_exp, fingerprint_bytes);
	bin_clear_free(fingerprint_exp, tmp->pw_len);

	if (res) {
		wpa_printf(MSG_DEBUG, "SAE-PK: K_AP fingerprint mismatch");
		return false;
	}

	wpa_printf(MSG_DEBUG, "SAE-PK: Valid K_AP fingerprint");
	return true;
}


int sae_check_confirm_pk(struct sae_data *sae, const u8 *ies, size_t ies_len)
{
	struct sae_temporary_data *tmp = sae->tmp;
	const u8 *sae_pk, *pos, *end, *encr_mod, *k_ap, *key_auth;
	u8 m[SAE_PK_M_LEN];
	size_t k_ap_len, key_auth_len;
	struct crypto_ec_key *key;
	int res;
	u8 hash[SAE_MAX_HASH_LEN];
	size_t hash_len;
	int group;

	if (!tmp)
		return -1;
	if (!sae->pk || tmp->ap_pk)
		return 0;

	if (tmp->kek_len != 32 && tmp->kek_len != 48 && tmp->kek_len != 64) {
		wpa_printf(MSG_INFO, "SAE-PK: No KEK available for confirm");
		return -1;
	}

	if (!tmp->ec) {
		/* Only ECC groups are supported for SAE-PK in the current
		 * implementation. */
		wpa_printf(MSG_INFO,
			   "SAE-PK: SAE commit did not use an ECC group");
		return -1;
	}

	wpa_hexdump(MSG_DEBUG, "SAE-PK: Received confirm IEs", ies, ies_len);
	sae_pk = get_vendor_ie(ies, ies_len, SAE_PK_IE_VENDOR_TYPE);
	if (!sae_pk) {
		wpa_printf(MSG_INFO, "SAE-PK: No SAE-PK element included");
		return -1;
	}
	/* TODO: Fragment reassembly */
	pos = sae_pk + 2;
	end = pos + sae_pk[1];

	if (end - pos < 4 + 1 + SAE_PK_M_LEN + AES_BLOCK_SIZE) {
		wpa_printf(MSG_INFO,
			   "SAE-PK: No room for EncryptedModifier in SAE-PK element");
		return -1;
	}
	pos += 4;
	if (*pos != SAE_PK_M_LEN + AES_BLOCK_SIZE) {
		wpa_printf(MSG_INFO,
			   "SAE-PK: Unexpected EncryptedModifier length %u",
			   *pos);
		return -1;
	}
	pos++;
	encr_mod = pos;
	pos += SAE_PK_M_LEN + AES_BLOCK_SIZE;

	if (end - pos < 4 || pos[0] != WLAN_EID_EXTENSION || pos[1] < 2 ||
	    pos[1] > end - pos - 2 ||
	    pos[2] != WLAN_EID_EXT_FILS_PUBLIC_KEY) {
		wpa_printf(MSG_INFO,
			   "SAE-PK: No FILS Public Key element in SAE-PK element");
		return -1;
	}
	if (pos[3] != 3) {
		wpa_printf(MSG_INFO, "SAE-PK: Unsupported public key type %u",
			   pos[3]);
		return -1;
	}
	k_ap_len = pos[1] - 2;
	pos += 4;
	k_ap = pos;
	pos += k_ap_len;

	if (end - pos < 4 || pos[0] != WLAN_EID_EXTENSION || pos[1] < 1 ||
	    pos[1] > end - pos - 2 ||
	    pos[2] != WLAN_EID_EXT_FILS_KEY_CONFIRM) {
		wpa_printf(MSG_INFO,
			   "SAE-PK: No FILS Key Confirm element in SAE-PK element");
		return -1;
	}
	key_auth_len = pos[1] - 1;
	pos += 3;
	key_auth = pos;
	pos += key_auth_len;

	if (pos < end) {
		wpa_hexdump(MSG_DEBUG,
			    "SAE-PK: Extra data at the end of SAE-PK element",
			    pos, end - pos);
	}

	wpa_hexdump(MSG_DEBUG, "SAE-PK: EncryptedModifier",
		    encr_mod, SAE_PK_M_LEN + AES_BLOCK_SIZE);

	if (aes_siv_decrypt(tmp->kek, tmp->kek_len,
			    encr_mod, SAE_PK_M_LEN + AES_BLOCK_SIZE,
			    0, NULL, NULL, m) < 0) {
		wpa_printf(MSG_INFO,
			   "SAE-PK: Failed to decrypt EncryptedModifier");
		return -1;
	}
	wpa_hexdump_key(MSG_DEBUG, "SAE-PK: Modifier M", m, SAE_PK_M_LEN);

	wpa_hexdump(MSG_DEBUG, "SAE-PK: Received K_AP", k_ap, k_ap_len);
	/* TODO: Check against the public key, if one is stored in the network
	 * profile */

	key = crypto_ec_key_parse_pub(k_ap, k_ap_len);
	if (!key) {
		wpa_printf(MSG_INFO, "SAE-PK: Failed to parse K_AP");
		return -1;
	}

	group = crypto_ec_key_group(key);
	if (!sae_pk_valid_fingerprint(sae, m, SAE_PK_M_LEN, k_ap, k_ap_len,
				      group)) {
		crypto_ec_key_deinit(key);
		return -1;
	}

	/* TODO: Could support alternative groups as long as the combination
	 * meets the requirements. */
	if (group != sae->group) {
		wpa_printf(MSG_INFO,
			   "SAE-PK: K_AP group %d does not match SAE group %d",
			   group, sae->group);
		crypto_ec_key_deinit(key);
		return -1;
	}

	wpa_hexdump(MSG_DEBUG, "SAE-PK: Received KeyAuth",
		    key_auth, key_auth_len);

	hash_len = sae_group_2_hash_len(group);
	if (sae_pk_hash_sig_data(sae, hash_len, false, m, SAE_PK_M_LEN,
				 k_ap, k_ap_len, hash) < 0) {
		crypto_ec_key_deinit(key);
		return -1;
	}

	res = crypto_ec_key_verify_signature(key, hash, hash_len,
					     key_auth, key_auth_len);
	crypto_ec_key_deinit(key);

	if (res != 1) {
		wpa_printf(MSG_INFO,
			   "SAE-PK: Invalid or incorrect signature in KeyAuth");
		return -1;
	}

	wpa_printf(MSG_DEBUG, "SAE-PK: Valid KeyAuth signature received");

	/* TODO: Store validated public key into network profile */

	return 0;
}
