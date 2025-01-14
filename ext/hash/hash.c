/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) The PHP Group                                          |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Sara Golemon <pollita@php.net>                               |
  |         Scott MacVicar <scottmac@php.net>                            |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include "php_hash.h"
#include "ext/standard/info.h"
#include "ext/standard/file.h"

#include "zend_interfaces.h"
#include "zend_exceptions.h"

HashTable php_hash_hashtable;
zend_class_entry *php_hashcontext_ce;
static zend_object_handlers php_hashcontext_handlers;

#ifdef PHP_MHASH_BC
struct mhash_bc_entry {
	char *mhash_name;
	char *hash_name;
	int value;
};

#define MHASH_NUM_ALGOS 35

static struct mhash_bc_entry mhash_to_hash[MHASH_NUM_ALGOS] = {
	{"CRC32", "crc32", 0}, /* used by bzip */
	{"MD5", "md5", 1},
	{"SHA1", "sha1", 2},
	{"HAVAL256", "haval256,3", 3},
	{NULL, NULL, 4},
	{"RIPEMD160", "ripemd160", 5},
	{NULL, NULL, 6},
	{"TIGER", "tiger192,3", 7},
	{"GOST", "gost", 8},
	{"CRC32B", "crc32b", 9}, /* used by ethernet (IEEE 802.3), gzip, zip, png, etc */
	{"HAVAL224", "haval224,3", 10},
	{"HAVAL192", "haval192,3", 11},
	{"HAVAL160", "haval160,3", 12},
	{"HAVAL128", "haval128,3", 13},
	{"TIGER128", "tiger128,3", 14},
	{"TIGER160", "tiger160,3", 15},
	{"MD4", "md4", 16},
	{"SHA256", "sha256", 17},
	{"ADLER32", "adler32", 18},
	{"SHA224", "sha224", 19},
	{"SHA512", "sha512", 20},
	{"SHA384", "sha384", 21},
	{"WHIRLPOOL", "whirlpool", 22},
	{"RIPEMD128", "ripemd128", 23},
	{"RIPEMD256", "ripemd256", 24},
	{"RIPEMD320", "ripemd320", 25},
	{NULL, NULL, 26}, /* support needs to be added for snefru 128 */
	{"SNEFRU256", "snefru256", 27},
	{"MD2", "md2", 28},
	{"FNV132", "fnv132", 29},
	{"FNV1A32", "fnv1a32", 30},
	{"FNV164", "fnv164", 31},
	{"FNV1A64", "fnv1a64", 32},
	{"JOAAT", "joaat", 33},
	{"CRC32C", "crc32c", 34}, /* Castagnoli's CRC, used by iSCSI, SCTP, Btrfs, ext4, etc */
};
#endif

/* Hash Registry Access */

PHP_HASH_API const php_hash_ops *php_hash_fetch_ops(const char *algo, size_t algo_len) /* {{{ */
{
	char *lower = zend_str_tolower_dup(algo, algo_len);
	php_hash_ops *ops = zend_hash_str_find_ptr(&php_hash_hashtable, lower, algo_len);
	efree(lower);

	return ops;
}
/* }}} */

PHP_HASH_API void php_hash_register_algo(const char *algo, const php_hash_ops *ops) /* {{{ */
{
	size_t algo_len = strlen(algo);
	char *lower = zend_str_tolower_dup(algo, algo_len);
	zend_hash_add_ptr(&php_hash_hashtable, zend_string_init_interned(lower, algo_len, 1), (void *) ops);
	efree(lower);
}
/* }}} */

PHP_HASH_API int php_hash_copy(const void *ops, void *orig_context, void *dest_context) /* {{{ */
{
	php_hash_ops *hash_ops = (php_hash_ops *)ops;

	memcpy(dest_context, orig_context, hash_ops->context_size);
	return SUCCESS;
}
/* }}} */

/* Userspace */

static void php_hash_do_hash(INTERNAL_FUNCTION_PARAMETERS, int isfilename, zend_bool raw_output_default) /* {{{ */
{
	zend_string *digest;
	char *algo, *data;
	size_t algo_len, data_len;
	zend_bool raw_output = raw_output_default;
	const php_hash_ops *ops;
	void *context;
	php_stream *stream = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss|b", &algo, &algo_len, &data, &data_len, &raw_output) == FAILURE) {
		return;
	}

	ops = php_hash_fetch_ops(algo, algo_len);
	if (!ops) {
		php_error_docref(NULL, E_WARNING, "Unknown hashing algorithm: %s", algo);
		RETURN_FALSE;
	}
	if (isfilename) {
		if (CHECK_NULL_PATH(data, data_len)) {
			php_error_docref(NULL, E_WARNING, "Invalid path");
			RETURN_FALSE;
		}
		stream = php_stream_open_wrapper_ex(data, "rb", REPORT_ERRORS, NULL, FG(default_context));
		if (!stream) {
			/* Stream will report errors opening file */
			RETURN_FALSE;
		}
	}

	context = emalloc(ops->context_size);
	ops->hash_init(context);

	if (isfilename) {
		char buf[1024];
		ssize_t n;

		while ((n = php_stream_read(stream, buf, sizeof(buf))) > 0) {
			ops->hash_update(context, (unsigned char *) buf, n);
		}
		php_stream_close(stream);
		if (n < 0) {
			efree(context);
			RETURN_FALSE;
		}
	} else {
		ops->hash_update(context, (unsigned char *) data, data_len);
	}

	digest = zend_string_alloc(ops->digest_size, 0);
	ops->hash_final((unsigned char *) ZSTR_VAL(digest), context);
	efree(context);

	if (raw_output) {
		ZSTR_VAL(digest)[ops->digest_size] = 0;
		RETURN_NEW_STR(digest);
	} else {
		zend_string *hex_digest = zend_string_safe_alloc(ops->digest_size, 2, 0, 0);

		php_hash_bin2hex(ZSTR_VAL(hex_digest), (unsigned char *) ZSTR_VAL(digest), ops->digest_size);
		ZSTR_VAL(hex_digest)[2 * ops->digest_size] = 0;
		zend_string_release_ex(digest, 0);
		RETURN_NEW_STR(hex_digest);
	}
}
/* }}} */

/* {{{ proto string hash(string algo, string data[, bool raw_output = false])
Generate a hash of a given input string
Returns lowercase hexits by default */
PHP_FUNCTION(hash)
{
	php_hash_do_hash(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0, 0);
}
/* }}} */

/* {{{ proto string hash_file(string algo, string filename[, bool raw_output = false])
Generate a hash of a given file
Returns lowercase hexits by default */
PHP_FUNCTION(hash_file)
{
	php_hash_do_hash(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1, 0);
}
/* }}} */

static inline void php_hash_string_xor_char(unsigned char *out, const unsigned char *in, const unsigned char xor_with, const size_t length) {
	size_t i;
	for (i=0; i < length; i++) {
		out[i] = in[i] ^ xor_with;
	}
}

static inline void php_hash_string_xor(unsigned char *out, const unsigned char *in, const unsigned char *xor_with, const size_t length) {
	size_t i;
	for (i=0; i < length; i++) {
		out[i] = in[i] ^ xor_with[i];
	}
}

static inline void php_hash_hmac_prep_key(unsigned char *K, const php_hash_ops *ops, void *context, const unsigned char *key, const size_t key_len) {
	memset(K, 0, ops->block_size);
	if (key_len > ops->block_size) {
		/* Reduce the key first */
		ops->hash_init(context);
		ops->hash_update(context, key, key_len);
		ops->hash_final(K, context);
	} else {
		memcpy(K, key, key_len);
	}
	/* XOR the key with 0x36 to get the ipad) */
	php_hash_string_xor_char(K, K, 0x36, ops->block_size);
}

static inline void php_hash_hmac_round(unsigned char *final, const php_hash_ops *ops, void *context, const unsigned char *key, const unsigned char *data, const zend_long data_size) {
	ops->hash_init(context);
	ops->hash_update(context, key, ops->block_size);
	ops->hash_update(context, data, data_size);
	ops->hash_final(final, context);
}

static void php_hash_do_hash_hmac(INTERNAL_FUNCTION_PARAMETERS, int isfilename, zend_bool raw_output_default) /* {{{ */
{
	zend_string *digest;
	char *algo, *data, *key;
	unsigned char *K;
	size_t algo_len, data_len, key_len;
	zend_bool raw_output = raw_output_default;
	const php_hash_ops *ops;
	void *context;
	php_stream *stream = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "sss|b", &algo, &algo_len, &data, &data_len,
																  &key, &key_len, &raw_output) == FAILURE) {
		return;
	}

	ops = php_hash_fetch_ops(algo, algo_len);
	if (!ops) {
		zend_throw_error(NULL, "Unknown hashing algorithm: %s", algo);
		return;
	}
	else if (!ops->is_crypto) {
		zend_throw_error(NULL, "Non-cryptographic hashing algorithm: %s", algo);
		return;
	}

	if (isfilename) {
		if (CHECK_NULL_PATH(data, data_len)) {
			zend_throw_error(NULL, "Invalid path");
			return;
		}
		stream = php_stream_open_wrapper_ex(data, "rb", REPORT_ERRORS, NULL, FG(default_context));
		if (!stream) {
			/* Stream will report errors opening file */
			RETURN_FALSE;
		}
	}

	context = emalloc(ops->context_size);

	K = emalloc(ops->block_size);
	digest = zend_string_alloc(ops->digest_size, 0);

	php_hash_hmac_prep_key(K, ops, context, (unsigned char *) key, key_len);

	if (isfilename) {
		char buf[1024];
		ssize_t n;
		ops->hash_init(context);
		ops->hash_update(context, K, ops->block_size);
		while ((n = php_stream_read(stream, buf, sizeof(buf))) > 0) {
			ops->hash_update(context, (unsigned char *) buf, n);
		}
		php_stream_close(stream);
		if (n < 0) {
			efree(context);
			efree(K);
			zend_string_release(digest);
			RETURN_FALSE;
		}

		ops->hash_final((unsigned char *) ZSTR_VAL(digest), context);
	} else {
		php_hash_hmac_round((unsigned char *) ZSTR_VAL(digest), ops, context, K, (unsigned char *) data, data_len);
	}

	php_hash_string_xor_char(K, K, 0x6A, ops->block_size);

	php_hash_hmac_round((unsigned char *) ZSTR_VAL(digest), ops, context, K, (unsigned char *) ZSTR_VAL(digest), ops->digest_size);

	/* Zero the key */
	ZEND_SECURE_ZERO(K, ops->block_size);
	efree(K);
	efree(context);

	if (raw_output) {
		ZSTR_VAL(digest)[ops->digest_size] = 0;
		RETURN_NEW_STR(digest);
	} else {
		zend_string *hex_digest = zend_string_safe_alloc(ops->digest_size, 2, 0, 0);

		php_hash_bin2hex(ZSTR_VAL(hex_digest), (unsigned char *) ZSTR_VAL(digest), ops->digest_size);
		ZSTR_VAL(hex_digest)[2 * ops->digest_size] = 0;
		zend_string_release_ex(digest, 0);
		RETURN_NEW_STR(hex_digest);
	}
}
/* }}} */

/* {{{ proto string hash_hmac(string algo, string data, string key[, bool raw_output = false])
Generate a hash of a given input string with a key using HMAC
Returns lowercase hexits by default */
PHP_FUNCTION(hash_hmac)
{
	php_hash_do_hash_hmac(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0, 0);
}
/* }}} */

/* {{{ proto string hash_hmac_file(string algo, string filename, string key[, bool raw_output = false])
Generate a hash of a given file with a key using HMAC
Returns lowercase hexits by default */
PHP_FUNCTION(hash_hmac_file)
{
	php_hash_do_hash_hmac(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1, 0);
}
/* }}} */

/* {{{ proto HashContext hash_init(string algo[, int options, string key])
Initialize a hashing context */
PHP_FUNCTION(hash_init)
{
	zend_string *algo, *key = NULL;
	zend_long options = 0;
	int argc = ZEND_NUM_ARGS();
	void *context;
	const php_hash_ops *ops;
	php_hashcontext_object *hash;

	if (zend_parse_parameters(argc, "S|lS", &algo, &options, &key) == FAILURE) {
		RETURN_NULL();
	}

	ops = php_hash_fetch_ops(ZSTR_VAL(algo), ZSTR_LEN(algo));
	if (!ops) {
		zend_throw_error(NULL, "Unknown hashing algorithm: %s", ZSTR_VAL(algo));
		return;
	}

	if (options & PHP_HASH_HMAC) {
		if (!ops->is_crypto) {
			zend_throw_error(NULL, "HMAC requested with a non-cryptographic hashing algorithm: %s", ZSTR_VAL(algo));
			return;
		}
		if (!key || (ZSTR_LEN(key) == 0)) {
			/* Note: a zero length key is no key at all */
			zend_throw_error(NULL, "HMAC requested without a key");
			return;
		}
	}

	object_init_ex(return_value, php_hashcontext_ce);
	hash = php_hashcontext_from_object(Z_OBJ_P(return_value));

	context = emalloc(ops->context_size);
	ops->hash_init(context);

	hash->ops = ops;
	hash->context = context;
	hash->options = options;
	hash->key = NULL;

	if (options & PHP_HASH_HMAC) {
		char *K = emalloc(ops->block_size);
		size_t i, block_size;

		memset(K, 0, ops->block_size);

		if (ZSTR_LEN(key) > ops->block_size) {
			/* Reduce the key first */
			ops->hash_update(context, (unsigned char *) ZSTR_VAL(key), ZSTR_LEN(key));
			ops->hash_final((unsigned char *) K, context);
			/* Make the context ready to start over */
			ops->hash_init(context);
		} else {
			memcpy(K, ZSTR_VAL(key), ZSTR_LEN(key));
		}

		/* XOR ipad */
		block_size = ops->block_size;
		for(i = 0; i < block_size; i++) {
			K[i] ^= 0x36;
		}
		ops->hash_update(context, (unsigned char *) K, ops->block_size);
		hash->key = (unsigned char *) K;
	}
}
/* }}} */

#define PHP_HASHCONTEXT_VERIFY(func, hash) { \
	if (!hash->context) { \
		zend_throw_error(NULL, "%s(): supplied resource is not a valid Hash Context resource", func); \
		return; \
	} \
}

/* {{{ proto bool hash_update(HashContext context, string data)
Pump data into the hashing algorithm */
PHP_FUNCTION(hash_update)
{
	zval *zhash;
	php_hashcontext_object *hash;
	zend_string *data;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "OS", &zhash, php_hashcontext_ce, &data) == FAILURE) {
		return;
	}

	hash = php_hashcontext_from_object(Z_OBJ_P(zhash));
	PHP_HASHCONTEXT_VERIFY("hash_update", hash);
	hash->ops->hash_update(hash->context, (unsigned char *) ZSTR_VAL(data), ZSTR_LEN(data));

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto int hash_update_stream(HashContext context, resource handle[, int length])
Pump data into the hashing algorithm from an open stream */
PHP_FUNCTION(hash_update_stream)
{
	zval *zhash, *zstream;
	php_hashcontext_object *hash;
	php_stream *stream = NULL;
	zend_long length = -1, didread = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "Or|l", &zhash, php_hashcontext_ce, &zstream, &length) == FAILURE) {
		return;
	}

	hash = php_hashcontext_from_object(Z_OBJ_P(zhash));
	PHP_HASHCONTEXT_VERIFY("hash_update_stream", hash);
	php_stream_from_zval(stream, zstream);

	while (length) {
		char buf[1024];
		zend_long toread = 1024;
		ssize_t n;

		if (length > 0 && toread > length) {
			toread = length;
		}

		if ((n = php_stream_read(stream, buf, toread)) <= 0) {
			RETURN_LONG(didread);
		}
		hash->ops->hash_update(hash->context, (unsigned char *) buf, n);
		length -= n;
		didread += n;
	}

	RETURN_LONG(didread);
}
/* }}} */

/* {{{ proto bool hash_update_file(HashContext context, string filename[, resource context])
Pump data into the hashing algorithm from a file */
PHP_FUNCTION(hash_update_file)
{
	zval *zhash, *zcontext = NULL;
	php_hashcontext_object *hash;
	php_stream_context *context;
	php_stream *stream;
	zend_string *filename;
	char buf[1024];
	ssize_t n;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "OP|r", &zhash, php_hashcontext_ce, &filename, &zcontext) == FAILURE) {
		return;
	}

	hash = php_hashcontext_from_object(Z_OBJ_P(zhash));
	PHP_HASHCONTEXT_VERIFY("hash_update_file", hash);
	context = php_stream_context_from_zval(zcontext, 0);

	stream = php_stream_open_wrapper_ex(ZSTR_VAL(filename), "rb", REPORT_ERRORS, NULL, context);
	if (!stream) {
		/* Stream will report errors opening file */
		RETURN_FALSE;
	}

	while ((n = php_stream_read(stream, buf, sizeof(buf))) > 0) {
		hash->ops->hash_update(hash->context, (unsigned char *) buf, n);
	}
	php_stream_close(stream);

	RETURN_BOOL(n >= 0);
}
/* }}} */

/* {{{ proto string hash_final(HashContext context[, bool raw_output=false])
Output resulting digest */
PHP_FUNCTION(hash_final)
{
	zval *zhash;
	php_hashcontext_object *hash;
	zend_bool raw_output = 0;
	zend_string *digest;
	size_t digest_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "O|b", &zhash, php_hashcontext_ce, &raw_output) == FAILURE) {
		return;
	}

	hash = php_hashcontext_from_object(Z_OBJ_P(zhash));
	PHP_HASHCONTEXT_VERIFY("hash_final", hash);

	digest_len = hash->ops->digest_size;
	digest = zend_string_alloc(digest_len, 0);
	hash->ops->hash_final((unsigned char *) ZSTR_VAL(digest), hash->context);
	if (hash->options & PHP_HASH_HMAC) {
		size_t i, block_size;

		/* Convert K to opad -- 0x6A = 0x36 ^ 0x5C */
		block_size = hash->ops->block_size;
		for(i = 0; i < block_size; i++) {
			hash->key[i] ^= 0x6A;
		}

		/* Feed this result into the outter hash */
		hash->ops->hash_init(hash->context);
		hash->ops->hash_update(hash->context, hash->key, hash->ops->block_size);
		hash->ops->hash_update(hash->context, (unsigned char *) ZSTR_VAL(digest), hash->ops->digest_size);
		hash->ops->hash_final((unsigned char *) ZSTR_VAL(digest), hash->context);

		/* Zero the key */
		ZEND_SECURE_ZERO(hash->key, hash->ops->block_size);
		efree(hash->key);
		hash->key = NULL;
	}
	ZSTR_VAL(digest)[digest_len] = 0;

	/* Invalidate the object from further use */
	efree(hash->context);
	hash->context = NULL;

	if (raw_output) {
		RETURN_NEW_STR(digest);
	} else {
		zend_string *hex_digest = zend_string_safe_alloc(digest_len, 2, 0, 0);

		php_hash_bin2hex(ZSTR_VAL(hex_digest), (unsigned char *) ZSTR_VAL(digest), digest_len);
		ZSTR_VAL(hex_digest)[2 * digest_len] = 0;
		zend_string_release_ex(digest, 0);
		RETURN_NEW_STR(hex_digest);
	}
}
/* }}} */

/* {{{ proto HashContext hash_copy(HashContext context)
Copy hash object */
PHP_FUNCTION(hash_copy)
{
	zval *zhash;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "O", &zhash, php_hashcontext_ce) == FAILURE) {
		return;
	}

	RETVAL_OBJ(Z_OBJ_HANDLER_P(zhash, clone_obj)(Z_OBJ_P(zhash)));

	if (php_hashcontext_from_object(Z_OBJ_P(return_value))->context == NULL) {
		zval_ptr_dtor(return_value);

		zend_throw_error(NULL, "Cannot copy hash");
		return;
	}
}
/* }}} */

/* {{{ proto array hash_algos(void)
Return a list of registered hashing algorithms */
PHP_FUNCTION(hash_algos)
{
	zend_string *str;

	array_init(return_value);
	ZEND_HASH_FOREACH_STR_KEY(&php_hash_hashtable, str) {
		add_next_index_str(return_value, zend_string_copy(str));
	} ZEND_HASH_FOREACH_END();
}
/* }}} */

/* {{{ proto array hash_hmac_algos(void)
Return a list of registered hashing algorithms suitable for hash_hmac() */
PHP_FUNCTION(hash_hmac_algos)
{
	zend_string *str;
	const php_hash_ops *ops;

	array_init(return_value);
	ZEND_HASH_FOREACH_STR_KEY_PTR(&php_hash_hashtable, str, ops) {
		if (ops->is_crypto) {
			add_next_index_str(return_value, zend_string_copy(str));
		}
	} ZEND_HASH_FOREACH_END();
}
/* }}} */

/* {{{ proto string hash_hkdf(string algo, string ikm [, int length = 0, string info = '', string salt = ''])
RFC5869 HMAC-based key derivation function */
PHP_FUNCTION(hash_hkdf)
{
	zend_string *returnval, *ikm, *algo, *info = NULL, *salt = NULL;
	zend_long length = 0;
	unsigned char *prk, *digest, *K;
	size_t i;
	size_t rounds;
	const php_hash_ops *ops;
	void *context;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "SS|lSS", &algo, &ikm, &length, &info, &salt) == FAILURE) {
		return;
	}

	ops = php_hash_fetch_ops(ZSTR_VAL(algo), ZSTR_LEN(algo));
	if (!ops) {
		zend_throw_error(NULL, "Unknown hashing algorithm: %s", ZSTR_VAL(algo));
		return;
	}

	if (!ops->is_crypto) {
		zend_throw_error(NULL, "Non-cryptographic hashing algorithm: %s", ZSTR_VAL(algo));
		return;
	}

	if (ZSTR_LEN(ikm) == 0) {
		zend_throw_error(NULL, "Input keying material cannot be empty");
		return;
	}

	if (length < 0) {
		zend_throw_error(NULL, "Length must be greater than or equal to 0: " ZEND_LONG_FMT, length);
		return;
	} else if (length == 0) {
		length = ops->digest_size;
	} else if (length > (zend_long) (ops->digest_size * 255)) {
		zend_throw_error(NULL, "Length must be less than or equal to %zd: " ZEND_LONG_FMT, ops->digest_size * 255, length);
		return;
	}

	context = emalloc(ops->context_size);

	// Extract
	ops->hash_init(context);
	K = emalloc(ops->block_size);
	php_hash_hmac_prep_key(K, ops, context,
		(unsigned char *) (salt ? ZSTR_VAL(salt) : ""), salt ? ZSTR_LEN(salt) : 0);

	prk = emalloc(ops->digest_size);
	php_hash_hmac_round(prk, ops, context, K, (unsigned char *) ZSTR_VAL(ikm), ZSTR_LEN(ikm));
	php_hash_string_xor_char(K, K, 0x6A, ops->block_size);
	php_hash_hmac_round(prk, ops, context, K, prk, ops->digest_size);
	ZEND_SECURE_ZERO(K, ops->block_size);

	// Expand
	returnval = zend_string_alloc(length, 0);
	digest = emalloc(ops->digest_size);
	for (i = 1, rounds = (length - 1) / ops->digest_size + 1; i <= rounds; i++) {
		// chr(i)
		unsigned char c[1];
		c[0] = (i & 0xFF);

		php_hash_hmac_prep_key(K, ops, context, prk, ops->digest_size);
		ops->hash_init(context);
		ops->hash_update(context, K, ops->block_size);

		if (i > 1) {
			ops->hash_update(context, digest, ops->digest_size);
		}

		if (info != NULL && ZSTR_LEN(info) > 0) {
			ops->hash_update(context, (unsigned char *) ZSTR_VAL(info), ZSTR_LEN(info));
		}

		ops->hash_update(context, c, 1);
		ops->hash_final(digest, context);
		php_hash_string_xor_char(K, K, 0x6A, ops->block_size);
		php_hash_hmac_round(digest, ops, context, K, digest, ops->digest_size);
		memcpy(
			ZSTR_VAL(returnval) + ((i - 1) * ops->digest_size),
			digest,
			(i == rounds ? length - ((i - 1) * ops->digest_size) : ops->digest_size)
		);
	}

	ZEND_SECURE_ZERO(K, ops->block_size);
	ZEND_SECURE_ZERO(digest, ops->digest_size);
	ZEND_SECURE_ZERO(prk, ops->digest_size);
	efree(K);
	efree(context);
	efree(prk);
	efree(digest);
	ZSTR_VAL(returnval)[length] = 0;
	RETURN_STR(returnval);
}

/* {{{ proto string hash_pbkdf2(string algo, string password, string salt, int iterations [, int length = 0, bool raw_output = false])
Generate a PBKDF2 hash of the given password and salt
Returns lowercase hexits by default */
PHP_FUNCTION(hash_pbkdf2)
{
	zend_string *returnval;
	char *algo, *salt, *pass = NULL;
	unsigned char *computed_salt, *digest, *temp, *result, *K1, *K2 = NULL;
	zend_long loops, i, j, iterations, digest_length = 0, length = 0;
	size_t algo_len, pass_len, salt_len = 0;
	zend_bool raw_output = 0;
	const php_hash_ops *ops;
	void *context;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "sssl|lb", &algo, &algo_len, &pass, &pass_len, &salt, &salt_len, &iterations, &length, &raw_output) == FAILURE) {
		return;
	}

	ops = php_hash_fetch_ops(algo, algo_len);
	if (!ops) {
		zend_throw_error(NULL, "Unknown hashing algorithm: %s", algo);
		return;
	}
	else if (!ops->is_crypto) {
		zend_throw_error(NULL, "Non-cryptographic hashing algorithm: %s", algo);
		return;
	}

	if (iterations <= 0) {
		zend_throw_error(NULL, "Iterations must be a positive integer: " ZEND_LONG_FMT, iterations);
		return;
	}

	if (length < 0) {
		zend_throw_error(NULL, "Length must be greater than or equal to 0: " ZEND_LONG_FMT, length);
		return;
	}

	if (salt_len > INT_MAX - 4) {
		zend_throw_error(NULL, "Supplied salt is too long, max of INT_MAX - 4 bytes: %zd supplied", salt_len);
		return;
	}

	context = emalloc(ops->context_size);
	ops->hash_init(context);

	K1 = emalloc(ops->block_size);
	K2 = emalloc(ops->block_size);
	digest = emalloc(ops->digest_size);
	temp = emalloc(ops->digest_size);

	/* Setup Keys that will be used for all hmac rounds */
	php_hash_hmac_prep_key(K1, ops, context, (unsigned char *) pass, pass_len);
	/* Convert K1 to opad -- 0x6A = 0x36 ^ 0x5C */
	php_hash_string_xor_char(K2, K1, 0x6A, ops->block_size);

	/* Setup Main Loop to build a long enough result */
	if (length == 0) {
		length = ops->digest_size;
		if (!raw_output) {
			length = length * 2;
		}
	}
	digest_length = length;
	if (!raw_output) {
		digest_length = (zend_long) ceil((float) length / 2.0);
	}

	loops = (zend_long) ceil((float) digest_length / (float) ops->digest_size);

	result = safe_emalloc(loops, ops->digest_size, 0);

	computed_salt = safe_emalloc(salt_len, 1, 4);
	memcpy(computed_salt, (unsigned char *) salt, salt_len);

	for (i = 1; i <= loops; i++) {
		/* digest = hash_hmac(salt + pack('N', i), password) { */

		/* pack("N", i) */
		computed_salt[salt_len] = (unsigned char) (i >> 24);
		computed_salt[salt_len + 1] = (unsigned char) ((i & 0xFF0000) >> 16);
		computed_salt[salt_len + 2] = (unsigned char) ((i & 0xFF00) >> 8);
		computed_salt[salt_len + 3] = (unsigned char) (i & 0xFF);

		php_hash_hmac_round(digest, ops, context, K1, computed_salt, (zend_long) salt_len + 4);
		php_hash_hmac_round(digest, ops, context, K2, digest, ops->digest_size);
		/* } */

		/* temp = digest */
		memcpy(temp, digest, ops->digest_size);

		/*
		 * Note that the loop starting at 1 is intentional, since we've already done
		 * the first round of the algorithm.
		 */
		for (j = 1; j < iterations; j++) {
			/* digest = hash_hmac(digest, password) { */
			php_hash_hmac_round(digest, ops, context, K1, digest, ops->digest_size);
			php_hash_hmac_round(digest, ops, context, K2, digest, ops->digest_size);
			/* } */
			/* temp ^= digest */
			php_hash_string_xor(temp, temp, digest, ops->digest_size);
		}
		/* result += temp */
		memcpy(result + ((i - 1) * ops->digest_size), temp, ops->digest_size);
	}
	/* Zero potentially sensitive variables */
	ZEND_SECURE_ZERO(K1, ops->block_size);
	ZEND_SECURE_ZERO(K2, ops->block_size);
	ZEND_SECURE_ZERO(computed_salt, salt_len + 4);
	efree(K1);
	efree(K2);
	efree(computed_salt);
	efree(context);
	efree(digest);
	efree(temp);

	returnval = zend_string_alloc(length, 0);
	if (raw_output) {
		memcpy(ZSTR_VAL(returnval), result, length);
	} else {
		php_hash_bin2hex(ZSTR_VAL(returnval), result, digest_length);
	}
	ZSTR_VAL(returnval)[length] = 0;
	efree(result);
	RETURN_NEW_STR(returnval);
}
/* }}} */

/* {{{ proto bool hash_equals(string known_string, string user_string)
   Compares two strings using the same time whether they're equal or not.
   A difference in length will leak */
PHP_FUNCTION(hash_equals)
{
	zval *known_zval, *user_zval;
	char *known_str, *user_str;
	int result = 0;
	size_t j;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "zz", &known_zval, &user_zval) == FAILURE) {
		return;
	}

	/* We only allow comparing string to prevent unexpected results. */
	if (Z_TYPE_P(known_zval) != IS_STRING) {
		zend_type_error("Expected known_string to be a string, %s given", zend_zval_type_name(known_zval));
		return;
	}

	if (Z_TYPE_P(user_zval) != IS_STRING) {
		zend_type_error("Expected user_string to be a string, %s given", zend_zval_type_name(user_zval));
		return;
	}

	if (Z_STRLEN_P(known_zval) != Z_STRLEN_P(user_zval)) {
		RETURN_FALSE;
	}

	known_str = Z_STRVAL_P(known_zval);
	user_str = Z_STRVAL_P(user_zval);

	/* This is security sensitive code. Do not optimize this for speed. */
	for (j = 0; j < Z_STRLEN_P(known_zval); j++) {
		result |= known_str[j] ^ user_str[j];
	}

	RETURN_BOOL(0 == result);
}
/* }}} */

/* {{{ proto HashContext::__construct() */
static PHP_METHOD(HashContext, __construct) {
	/* Normally unreachable as private/final */
	zend_throw_exception(zend_ce_error, "Illegal call to private/final constructor", 0);
}
/* }}} */

static const zend_function_entry php_hashcontext_methods[] = {
	PHP_ME(HashContext, __construct, NULL, ZEND_ACC_PRIVATE)
	PHP_FE_END
};

/* Module Housekeeping */

#define PHP_HASH_HAVAL_REGISTER(p,b)	php_hash_register_algo("haval" #b "," #p , &php_hash_##p##haval##b##_ops);

#ifdef PHP_MHASH_BC

#if 0
/* See #69823, we should not insert module into module_registry while doing startup */

PHP_MINFO_FUNCTION(mhash)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "MHASH support", "Enabled");
	php_info_print_table_row(2, "MHASH API Version", "Emulated Support");
	php_info_print_table_end();
}

zend_module_entry mhash_module_entry = {
	STANDARD_MODULE_HEADER,
	"mhash",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	PHP_MINFO(mhash),
	PHP_MHASH_VERSION,
	STANDARD_MODULE_PROPERTIES,
};
#endif

static void mhash_init(INIT_FUNC_ARGS)
{
	char buf[128];
	int len;
	int algo_number = 0;

	for (algo_number = 0; algo_number < MHASH_NUM_ALGOS; algo_number++) {
		struct mhash_bc_entry algorithm = mhash_to_hash[algo_number];
		if (algorithm.mhash_name == NULL) {
			continue;
		}

		len = slprintf(buf, 127, "MHASH_%s", algorithm.mhash_name);
		zend_register_long_constant(buf, len, algorithm.value, CONST_CS | CONST_PERSISTENT, module_number);
	}

	/* TODO: this cause #69823 zend_register_internal_module(&mhash_module_entry); */
}

/* {{{ proto string mhash(int hash, string data [, string key])
   Hash data with hash */
PHP_FUNCTION(mhash)
{
	zval *z_algorithm;
	zend_long algorithm;

	if (zend_parse_parameters(1, "z", &z_algorithm) == FAILURE) {
		return;
	}

	algorithm = zval_get_long(z_algorithm);

	/* need to convert the first parameter from int constant to string algorithm name */
	if (algorithm >= 0 && algorithm < MHASH_NUM_ALGOS) {
		struct mhash_bc_entry algorithm_lookup = mhash_to_hash[algorithm];
		if (algorithm_lookup.hash_name) {
			ZVAL_STRING(z_algorithm, algorithm_lookup.hash_name);
		}
	}

	if (ZEND_NUM_ARGS() == 3) {
		php_hash_do_hash_hmac(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0, 1);
	} else if (ZEND_NUM_ARGS() == 2) {
		php_hash_do_hash(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0, 1);
	} else {
		WRONG_PARAM_COUNT;
	}
}
/* }}} */

/* {{{ proto string mhash_get_hash_name(int hash)
   Gets the name of hash */
PHP_FUNCTION(mhash_get_hash_name)
{
	zend_long algorithm;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "l", &algorithm) == FAILURE) {
		return;
	}

	if (algorithm >= 0 && algorithm  < MHASH_NUM_ALGOS) {
		struct mhash_bc_entry algorithm_lookup = mhash_to_hash[algorithm];
		if (algorithm_lookup.mhash_name) {
			RETURN_STRING(algorithm_lookup.mhash_name);
		}
	}
	RETURN_FALSE;
}
/* }}} */

/* {{{ proto int mhash_count(void)
   Gets the number of available hashes */
PHP_FUNCTION(mhash_count)
{
	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}
	RETURN_LONG(MHASH_NUM_ALGOS - 1);
}
/* }}} */

/* {{{ proto int mhash_get_block_size(int hash)
   Gets the block size of hash */
PHP_FUNCTION(mhash_get_block_size)
{
	zend_long algorithm;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "l", &algorithm) == FAILURE) {
		return;
	}
	RETVAL_FALSE;

	if (algorithm >= 0 && algorithm  < MHASH_NUM_ALGOS) {
		struct mhash_bc_entry algorithm_lookup = mhash_to_hash[algorithm];
		if (algorithm_lookup.mhash_name) {
			const php_hash_ops *ops = php_hash_fetch_ops(algorithm_lookup.hash_name, strlen(algorithm_lookup.hash_name));
			if (ops) {
				RETVAL_LONG(ops->digest_size);
			}
		}
	}
}
/* }}} */

#define SALT_SIZE 8

/* {{{ proto string mhash_keygen_s2k(int hash, string input_password, string salt, int bytes)
   Generates a key using hash functions */
PHP_FUNCTION(mhash_keygen_s2k)
{
	zend_long algorithm, l_bytes;
	int bytes;
	char *password, *salt;
	size_t password_len, salt_len;
	char padded_salt[SALT_SIZE];

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "lssl", &algorithm, &password, &password_len, &salt, &salt_len, &l_bytes) == FAILURE) {
		return;
	}

	bytes = (int)l_bytes;
	if (bytes <= 0){
		php_error_docref(NULL, E_WARNING, "the byte parameter must be greater than 0");
		RETURN_FALSE;
	}

	salt_len = MIN(salt_len, SALT_SIZE);

	memcpy(padded_salt, salt, salt_len);
	if (salt_len < SALT_SIZE) {
		memset(padded_salt + salt_len, 0, SALT_SIZE - salt_len);
	}
	salt_len = SALT_SIZE;

	RETVAL_FALSE;
	if (algorithm >= 0 && algorithm < MHASH_NUM_ALGOS) {
		struct mhash_bc_entry algorithm_lookup = mhash_to_hash[algorithm];
		if (algorithm_lookup.mhash_name) {
			const php_hash_ops *ops = php_hash_fetch_ops(algorithm_lookup.hash_name, strlen(algorithm_lookup.hash_name));
			if (ops) {
				unsigned char null = '\0';
				void *context;
				char *key, *digest;
				int i = 0, j = 0;
				size_t block_size = ops->digest_size;
				size_t times = bytes / block_size;

				if ((bytes % block_size) != 0) {
					times++;
				}

				context = emalloc(ops->context_size);
				ops->hash_init(context);

				key = ecalloc(1, times * block_size);
				digest = emalloc(ops->digest_size + 1);

				for (i = 0; i < times; i++) {
					ops->hash_init(context);

					for (j=0;j<i;j++) {
						ops->hash_update(context, &null, 1);
					}
					ops->hash_update(context, (unsigned char *)padded_salt, salt_len);
					ops->hash_update(context, (unsigned char *)password, password_len);
					ops->hash_final((unsigned char *)digest, context);
					memcpy( &key[i*block_size], digest, block_size);
				}

				RETVAL_STRINGL(key, bytes);
				ZEND_SECURE_ZERO(key, bytes);
				efree(digest);
				efree(context);
				efree(key);
			}
		}
	}
}
/* }}} */

#endif

/* ----------------------------------------------------------------------- */

/* {{{ php_hashcontext_create */
static zend_object* php_hashcontext_create(zend_class_entry *ce) {
	php_hashcontext_object *objval = zend_object_alloc(sizeof(php_hashcontext_object), ce);
	zend_object *zobj = &objval->std;

	zend_object_std_init(zobj, ce);
	object_properties_init(zobj, ce);
	zobj->handlers = &php_hashcontext_handlers;

	return zobj;
}
/* }}} */

/* {{{ php_hashcontext_dtor */
static void php_hashcontext_dtor(zend_object *obj) {
	php_hashcontext_object *hash = php_hashcontext_from_object(obj);

	/* Just in case the algo has internally allocated resources */
	if (hash->context) {
		unsigned char *dummy = emalloc(hash->ops->digest_size);
		hash->ops->hash_final(dummy, hash->context);
		efree(dummy);
		efree(hash->context);
		hash->context = NULL;
	}

	if (hash->key) {
		ZEND_SECURE_ZERO(hash->key, hash->ops->block_size);
		efree(hash->key);
		hash->key = NULL;
	}
}
/* }}} */

/* {{{ php_hashcontext_clone */
static zend_object *php_hashcontext_clone(zend_object *zobj) {
	php_hashcontext_object *oldobj = php_hashcontext_from_object(zobj);
	zend_object *znew = php_hashcontext_create(zobj->ce);
	php_hashcontext_object *newobj = php_hashcontext_from_object(znew);

	zend_objects_clone_members(znew, zobj);

	newobj->ops = oldobj->ops;
	newobj->options = oldobj->options;
	newobj->context = emalloc(newobj->ops->context_size);
	newobj->ops->hash_init(newobj->context);

	if (SUCCESS != newobj->ops->hash_copy(newobj->ops, oldobj->context, newobj->context)) {
		efree(newobj->context);
		newobj->context = NULL;
		return znew;
	}

	newobj->key = ecalloc(1, newobj->ops->block_size);
	if (oldobj->key) {
		memcpy(newobj->key, oldobj->key, newobj->ops->block_size);
	}

	return znew;
}
/* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(hash)
{
	zend_class_entry ce;

	zend_hash_init(&php_hash_hashtable, 35, NULL, NULL, 1);

	php_hash_register_algo("md2",			&php_hash_md2_ops);
	php_hash_register_algo("md4",			&php_hash_md4_ops);
	php_hash_register_algo("md5",			&php_hash_md5_ops);
	php_hash_register_algo("sha1",			&php_hash_sha1_ops);
	php_hash_register_algo("sha224",		&php_hash_sha224_ops);
	php_hash_register_algo("sha256",		&php_hash_sha256_ops);
	php_hash_register_algo("sha384",		&php_hash_sha384_ops);
	php_hash_register_algo("sha512/224",            &php_hash_sha512_224_ops);
	php_hash_register_algo("sha512/256",            &php_hash_sha512_256_ops);
	php_hash_register_algo("sha512",		&php_hash_sha512_ops);
	php_hash_register_algo("sha3-224",		&php_hash_sha3_224_ops);
	php_hash_register_algo("sha3-256",		&php_hash_sha3_256_ops);
	php_hash_register_algo("sha3-384",		&php_hash_sha3_384_ops);
	php_hash_register_algo("sha3-512",		&php_hash_sha3_512_ops);
	php_hash_register_algo("ripemd128",		&php_hash_ripemd128_ops);
	php_hash_register_algo("ripemd160",		&php_hash_ripemd160_ops);
	php_hash_register_algo("ripemd256",		&php_hash_ripemd256_ops);
	php_hash_register_algo("ripemd320",		&php_hash_ripemd320_ops);
	php_hash_register_algo("whirlpool",		&php_hash_whirlpool_ops);
	php_hash_register_algo("tiger128,3",	&php_hash_3tiger128_ops);
	php_hash_register_algo("tiger160,3",	&php_hash_3tiger160_ops);
	php_hash_register_algo("tiger192,3",	&php_hash_3tiger192_ops);
	php_hash_register_algo("tiger128,4",	&php_hash_4tiger128_ops);
	php_hash_register_algo("tiger160,4",	&php_hash_4tiger160_ops);
	php_hash_register_algo("tiger192,4",	&php_hash_4tiger192_ops);
	php_hash_register_algo("snefru",		&php_hash_snefru_ops);
	php_hash_register_algo("snefru256",		&php_hash_snefru_ops);
	php_hash_register_algo("gost",			&php_hash_gost_ops);
	php_hash_register_algo("gost-crypto",		&php_hash_gost_crypto_ops);
	php_hash_register_algo("adler32",		&php_hash_adler32_ops);
	php_hash_register_algo("crc32",			&php_hash_crc32_ops);
	php_hash_register_algo("crc32b",		&php_hash_crc32b_ops);
	php_hash_register_algo("crc32c",		&php_hash_crc32c_ops);
	php_hash_register_algo("fnv132",		&php_hash_fnv132_ops);
	php_hash_register_algo("fnv1a32",		&php_hash_fnv1a32_ops);
	php_hash_register_algo("fnv164",		&php_hash_fnv164_ops);
	php_hash_register_algo("fnv1a64",		&php_hash_fnv1a64_ops);
	php_hash_register_algo("joaat",			&php_hash_joaat_ops);

	PHP_HASH_HAVAL_REGISTER(3,128);
	PHP_HASH_HAVAL_REGISTER(3,160);
	PHP_HASH_HAVAL_REGISTER(3,192);
	PHP_HASH_HAVAL_REGISTER(3,224);
	PHP_HASH_HAVAL_REGISTER(3,256);

	PHP_HASH_HAVAL_REGISTER(4,128);
	PHP_HASH_HAVAL_REGISTER(4,160);
	PHP_HASH_HAVAL_REGISTER(4,192);
	PHP_HASH_HAVAL_REGISTER(4,224);
	PHP_HASH_HAVAL_REGISTER(4,256);

	PHP_HASH_HAVAL_REGISTER(5,128);
	PHP_HASH_HAVAL_REGISTER(5,160);
	PHP_HASH_HAVAL_REGISTER(5,192);
	PHP_HASH_HAVAL_REGISTER(5,224);
	PHP_HASH_HAVAL_REGISTER(5,256);

	REGISTER_LONG_CONSTANT("HASH_HMAC",		PHP_HASH_HMAC,	CONST_CS | CONST_PERSISTENT);

	INIT_CLASS_ENTRY(ce, "HashContext", php_hashcontext_methods);
	php_hashcontext_ce = zend_register_internal_class(&ce);
	php_hashcontext_ce->ce_flags |= ZEND_ACC_FINAL;
	php_hashcontext_ce->create_object = php_hashcontext_create;
	php_hashcontext_ce->serialize = zend_class_serialize_deny;
	php_hashcontext_ce->unserialize = zend_class_unserialize_deny;

	memcpy(&php_hashcontext_handlers, &std_object_handlers,
	       sizeof(zend_object_handlers));
	php_hashcontext_handlers.offset = XtOffsetOf(php_hashcontext_object, std);
	php_hashcontext_handlers.dtor_obj = php_hashcontext_dtor;
	php_hashcontext_handlers.clone_obj = php_hashcontext_clone;

#ifdef PHP_MHASH_BC
	mhash_init(INIT_FUNC_ARGS_PASSTHRU);
#endif

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(hash)
{
	zend_hash_destroy(&php_hash_hashtable);

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(hash)
{
	char buffer[2048];
	zend_string *str;
	char *s = buffer, *e = s + sizeof(buffer);

	ZEND_HASH_FOREACH_STR_KEY(&php_hash_hashtable, str) {
		s += slprintf(s, e - s, "%s ", ZSTR_VAL(str));
	} ZEND_HASH_FOREACH_END();
	*s = 0;

	php_info_print_table_start();
	php_info_print_table_row(2, "hash support", "enabled");
	php_info_print_table_row(2, "Hashing Engines", buffer);
	php_info_print_table_end();

#ifdef PHP_MHASH_BC
	php_info_print_table_start();
	php_info_print_table_row(2, "MHASH support", "Enabled");
	php_info_print_table_row(2, "MHASH API Version", "Emulated Support");
	php_info_print_table_end();
#endif

}
/* }}} */

/* {{{ arginfo */
#ifdef PHP_HASH_MD5_NOT_IN_CORE
ZEND_BEGIN_ARG_INFO_EX(arginfo_hash_md5, 0, 0, 1)
	ZEND_ARG_INFO(0, str)
	ZEND_ARG_INFO(0, raw_output)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hash_md5_file, 0, 0, 1)
	ZEND_ARG_INFO(0, filename)
	ZEND_ARG_INFO(0, raw_output)
ZEND_END_ARG_INFO()
#endif

#ifdef PHP_HASH_SHA1_NOT_IN_CORE
ZEND_BEGIN_ARG_INFO_EX(arginfo_hash_sha1, 0, 0, 1)
	ZEND_ARG_INFO(0, str)
	ZEND_ARG_INFO(0, raw_output)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hash_sha1_file, 0, 0, 1)
	ZEND_ARG_INFO(0, filename)
	ZEND_ARG_INFO(0, raw_output)
ZEND_END_ARG_INFO()
#endif

ZEND_BEGIN_ARG_INFO_EX(arginfo_hash, 0, 0, 2)
	ZEND_ARG_INFO(0, algo)
	ZEND_ARG_INFO(0, data)
	ZEND_ARG_INFO(0, raw_output)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hash_file, 0, 0, 2)
	ZEND_ARG_INFO(0, algo)
	ZEND_ARG_INFO(0, filename)
	ZEND_ARG_INFO(0, raw_output)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hash_hmac, 0, 0, 3)
	ZEND_ARG_INFO(0, algo)
	ZEND_ARG_INFO(0, data)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, raw_output)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hash_hmac_file, 0, 0, 3)
	ZEND_ARG_INFO(0, algo)
	ZEND_ARG_INFO(0, filename)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, raw_output)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hash_init, 0, 0, 1)
	ZEND_ARG_INFO(0, algo)
	ZEND_ARG_INFO(0, options)
	ZEND_ARG_INFO(0, key)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_hash_update, 0)
	ZEND_ARG_INFO(0, context)
	ZEND_ARG_INFO(0, data)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hash_update_stream, 0, 0, 2)
	ZEND_ARG_INFO(0, context)
	ZEND_ARG_INFO(0, handle)
	ZEND_ARG_INFO(0, length)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hash_update_file, 0, 0, 2)
	ZEND_ARG_INFO(0, context)
	ZEND_ARG_INFO(0, filename)
	ZEND_ARG_INFO(0, stream_context)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hash_final, 0, 0, 1)
	ZEND_ARG_INFO(0, context)
	ZEND_ARG_INFO(0, raw_output)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_hash_copy, 0)
	ZEND_ARG_INFO(0, context)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_hash_algos, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hash_pbkdf2, 0, 0, 4)
	ZEND_ARG_INFO(0, algo)
	ZEND_ARG_INFO(0, password)
	ZEND_ARG_INFO(0, salt)
	ZEND_ARG_INFO(0, iterations)
	ZEND_ARG_INFO(0, length)
	ZEND_ARG_INFO(0, raw_output)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_hash_equals, 0)
	ZEND_ARG_INFO(0, known_string)
	ZEND_ARG_INFO(0, user_string)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hash_hkdf, 0, 0, 2)
	ZEND_ARG_INFO(0, ikm)
	ZEND_ARG_INFO(0, algo)
	ZEND_ARG_INFO(0, length)
	ZEND_ARG_INFO(0, string)
	ZEND_ARG_INFO(0, salt)
ZEND_END_ARG_INFO()

/* BC Land */
#ifdef PHP_MHASH_BC
ZEND_BEGIN_ARG_INFO(arginfo_mhash_get_block_size, 0)
	ZEND_ARG_INFO(0, hash)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_mhash_get_hash_name, 0)
	ZEND_ARG_INFO(0, hash)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_mhash_keygen_s2k, 0)
	ZEND_ARG_INFO(0, hash)
	ZEND_ARG_INFO(0, input_password)
	ZEND_ARG_INFO(0, salt)
	ZEND_ARG_INFO(0, bytes)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_mhash_count, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mhash, 0, 0, 2)
	ZEND_ARG_INFO(0, hash)
	ZEND_ARG_INFO(0, data)
	ZEND_ARG_INFO(0, key)
ZEND_END_ARG_INFO()
#endif

/* }}} */

/* {{{ hash_functions[]
 */
static const zend_function_entry hash_functions[] = {
	PHP_FE(hash,									arginfo_hash)
	PHP_FE(hash_file,								arginfo_hash_file)

	PHP_FE(hash_hmac,								arginfo_hash_hmac)
	PHP_FE(hash_hmac_file,							arginfo_hash_hmac_file)

	PHP_FE(hash_init,								arginfo_hash_init)
	PHP_FE(hash_update,								arginfo_hash_update)
	PHP_FE(hash_update_stream,						arginfo_hash_update_stream)
	PHP_FE(hash_update_file,						arginfo_hash_update_file)
	PHP_FE(hash_final,								arginfo_hash_final)
	PHP_FE(hash_copy,								arginfo_hash_copy)

	PHP_FE(hash_algos,								arginfo_hash_algos)
	PHP_FE(hash_hmac_algos,							arginfo_hash_algos)
	PHP_FE(hash_pbkdf2,								arginfo_hash_pbkdf2)
	PHP_FE(hash_equals,								arginfo_hash_equals)
	PHP_FE(hash_hkdf,								arginfo_hash_hkdf)

	/* BC Land */
#ifdef PHP_HASH_MD5_NOT_IN_CORE
	PHP_NAMED_FE(md5, php_if_md5,					arginfo_hash_md5)
	PHP_NAMED_FE(md5_file, php_if_md5_file,			arginfo_hash_md5_file)
#endif /* PHP_HASH_MD5_NOT_IN_CORE */

#ifdef PHP_HASH_SHA1_NOT_IN_CORE
	PHP_NAMED_FE(sha1, php_if_sha1,					arginfo_hash_sha1)
	PHP_NAMED_FE(sha1_file, php_if_sha1_file,		arginfo_hash_sha1_file)
#endif /* PHP_HASH_SHA1_NOT_IN_CORE */

#ifdef PHP_MHASH_BC
	PHP_FE(mhash_keygen_s2k, arginfo_mhash_keygen_s2k)
	PHP_FE(mhash_get_block_size, arginfo_mhash_get_block_size)
	PHP_FE(mhash_get_hash_name, arginfo_mhash_get_hash_name)
	PHP_FE(mhash_count, arginfo_mhash_count)
	PHP_FE(mhash, arginfo_mhash)
#endif

	PHP_FE_END
};
/* }}} */

/* {{{ hash_module_entry
 */
zend_module_entry hash_module_entry = {
	STANDARD_MODULE_HEADER,
	PHP_HASH_EXTNAME,
	hash_functions,
	PHP_MINIT(hash),
	PHP_MSHUTDOWN(hash),
	NULL, /* RINIT */
	NULL, /* RSHUTDOWN */
	PHP_MINFO(hash),
	PHP_HASH_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */
