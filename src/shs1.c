#include "shs1.h"

#include <string.h>
#include <sodium.h>

// TODO remove this
void print_hex(void *mem, int size) {
  int i;
  unsigned char *p = (unsigned char *)mem;
  for (i=0;i<size;i++) {
    printf("0x%02x ", p[i]);
  }
  printf("\n");
}

#define HELLO_BYTES crypto_sign_BYTES + crypto_sign_PUBLICKEYBYTES

// TODO check whether some fields become unused at some point and can be overwritten with other data
// The order of fields is relevant from `app` to `shared_hash`.
struct SHS1_Client {
  // begin K | a_s * b_p | a_s * B_p | A_s * b_p
  unsigned char app[crypto_auth_KEYBYTES]; // K
  unsigned char shared_secret[crypto_scalarmult_BYTES]; // (a_s * b_p)
  unsigned char server_lterm_shared[crypto_scalarmult_BYTES]; // (a_s * B_p)
  unsigned char client_lterm_shared[crypto_scalarmult_BYTES]; // (A_s * b_p) only here to save copying later
  // end K | a_s * b_p | a_s * B_p | A_s * b_p
  // begin K | B_p | hash(a_s * b_p)
  unsigned char app_copy[crypto_auth_KEYBYTES]; // same as app, put here to save some copying later TODO suggest to change concatenation order in shs2 so this is not needed
  unsigned char server_pub[crypto_sign_PUBLICKEYBYTES]; // B_p
  unsigned char shared_hash[crypto_hash_sha256_BYTES]; // hash(a_s * b_p)
  // end K | B_p | hash(a_s * b_p)
  unsigned char pub[crypto_sign_PUBLICKEYBYTES]; // A_p
  unsigned char sec[crypto_sign_SECRETKEYBYTES]; // A_s
  unsigned char eph_pub[crypto_box_PUBLICKEYBYTES]; // a_p
  unsigned char eph_sec[crypto_box_SECRETKEYBYTES]; // a_s
  unsigned char app_hmac[crypto_auth_BYTES]; // hmac_{K}(a_p)
  unsigned char server_eph_pub[crypto_box_PUBLICKEYBYTES]; //b_p
  unsigned char hello[HELLO_BYTES]; // H = sign_{A_s}(K | B_p | hash(a_s * b_p)) | A_p
  unsigned char box_sec[crypto_hash_sha256_BYTES]; // hash(K | a_s * b_p | a_s * B_p | A_s * b_p)
  unsigned char encryption_nonce[crypto_box_NONCEBYTES]; // first 24 bytes of hmac_{K}(b_p)
};

SHS1_Client *shs1_init_client(
  const unsigned char *pub,
  const unsigned char *sec,
  const unsigned char *server_pub,
  const unsigned char *app,
  const unsigned char *eph_pub,
  const unsigned char *eph_sec
)
{
  SHS1_Client *client = malloc(sizeof(SHS1_Client));

  memcpy(client->pub, pub, crypto_sign_PUBLICKEYBYTES);
  memcpy(client->sec, sec, crypto_sign_SECRETKEYBYTES);
  memcpy(client->server_pub, server_pub, crypto_sign_PUBLICKEYBYTES);
  memcpy(client->app, app, crypto_auth_KEYBYTES);
  memcpy(client->app_copy, app, crypto_auth_KEYBYTES);
  memcpy(client->eph_pub, eph_pub, crypto_box_PUBLICKEYBYTES);
  memcpy(client->eph_sec, eph_sec, crypto_box_SECRETKEYBYTES);

  // hmac_{K}(a_p)
  crypto_auth(client->app_hmac, client->eph_pub, crypto_box_PUBLICKEYBYTES, client->app);

  return client;
}

// challenge <- hmac_{K}(a_p) | a_p
void shs1_create_client_challenge(
  unsigned char *challenge,
  SHS1_Client *client
)
{
  memcpy(challenge, client->app_hmac, crypto_auth_BYTES);
  memcpy(challenge + crypto_auth_BYTES, client->eph_pub, crypto_box_PUBLICKEYBYTES);
}

bool shs1_verify_server_challenge(
  const unsigned char *challenge,
  SHS1_Client *client
)
{
  if (crypto_auth_verify(
        challenge, challenge + crypto_auth_BYTES,
        crypto_box_PUBLICKEYBYTES, client->app
      ) != 0) {
    return false;
  } else {
    // hmac_{K}(b_p)
    memcpy(client->server_app_hmac, challenge, crypto_box_NONCEBYTES);
    // b_p
    memcpy(client->server_eph_pub, challenge + crypto_auth_BYTES, crypto_box_PUBLICKEYBYTES);
    // (a_s * b_p)
    if (crypto_scalarmult(client->shared_secret, client->eph_sec, client->server_eph_pub) != 0) {
      return false;
    };
    // hash(a_s * b_p)
    crypto_hash_sha256(client->shared_hash, client->shared_secret, crypto_scalarmult_BYTES);

    return true;
  }
}

// auth <- secretbox_{hash(K | a_s * b_p | a_s * B_p)}(H)
int shs1_create_client_auth(
  unsigned char *auth,
  SHS1_Client *client
)
{
  unsigned char curve_server_pub[crypto_scalarmult_curve25519_BYTES];
  if (crypto_sign_ed25519_pk_to_curve25519(curve_server_pub, client->server_pub) != 0) {
    return -1;
  };

  // (a_s * B_p)
  if (crypto_scalarmult(client->server_lterm_shared, client->eph_sec, curve_server_pub) != 0) {
    return -2;
  };

  // hash(K | a_s * b_p | a_s * B_p)
  unsigned char box_sec[crypto_secretbox_KEYBYTES]; // same as crypto_hash_sha256_BYTES
  crypto_hash_sha256(box_sec, client->app, crypto_auth_KEYBYTES + 2 * crypto_scalarmult_BYTES);

  // sign_{A_s}(K | B_p | hash(a_s * b_p))
  unsigned char sig[crypto_sign_BYTES];
  crypto_sign_detached(
    sig, NULL, client->app_copy,
    crypto_auth_KEYBYTES + crypto_sign_PUBLICKEYBYTES + crypto_hash_sha256_BYTES,
    client->sec
  );

  // H = sign_{A_s}(K | B_p | hash(a_s * b_p)) | A_p
  memcpy(client->hello, sig, sizeof(sig));
  memcpy(client->hello + crypto_sign_BYTES, client->pub, crypto_sign_PUBLICKEYBYTES);

  unsigned char nonce[crypto_secretbox_NONCEBYTES] = {0};

  crypto_secretbox_easy(
    auth, client->hello, HELLO_BYTES,
    nonce, box_sec
  );

  return 0;
}

bool shs1_verify_server_auth(
  const unsigned char *auth,
  SHS1_Client *client
)
{
  unsigned char curve_sec[crypto_scalarmult_curve25519_BYTES];
  if (crypto_sign_ed25519_sk_to_curve25519(curve_sec, client->sec) != 0) {
    return -1;
  };

  // (A_s * b_p)
  if (crypto_scalarmult(client->client_lterm_shared, curve_sec, client->server_eph_pub) != 0) {
    return -2;
  };

  // hash(K | a_s * b_p | a_s * B_p | A_s * b_p)
  crypto_hash_sha256(client->box_sec, client->app, crypto_auth_KEYBYTES + 3 * crypto_scalarmult_BYTES);

  unsigned char nonce[crypto_secretbox_NONCEBYTES] = {0};

  unsigned char sig[crypto_sign_BYTES];
  if (crypto_secretbox_open_easy(sig, auth, SHS1_SERVER_AUTH_BYTES, nonce, client->box_sec) != 0) {
    return false;
  } else {
    // K | H | hash(a_s * b_p)
    unsigned char expected[crypto_auth_KEYBYTES + HELLO_BYTES + crypto_hash_sha256_BYTES];
    memcpy(expected, client->app, crypto_auth_KEYBYTES);
    memcpy(expected + crypto_auth_KEYBYTES, client->hello, HELLO_BYTES);
    memcpy(expected + crypto_auth_KEYBYTES + HELLO_BYTES, client->shared_hash, crypto_hash_sha256_BYTES);

    return crypto_sign_verify_detached(sig, expected, sizeof(expected), client->server_pub) == 0;
  }

  return false;
}

void shs1_client_outcome(
  SHS1_Outcome *outcome,
  const SHS1_Client *client
)
{
  // hash(hash(hash(K | a_s * b_p | a_s * B_p | A_s * b_p)) | B_p)
  unsigned char tmp[crypto_hash_sha256_BYTES + crypto_sign_PUBLICKEYBYTES];
  crypto_hash_sha256(tmp, client->box_sec, crypto_hash_sha256_BYTES);
  memcpy(tmp + crypto_hash_sha256_BYTES, client->server_pub, crypto_sign_PUBLICKEYBYTES);
  crypto_hash_sha256(outcome->encryption_key, tmp, crypto_hash_sha256_BYTES + crypto_sign_PUBLICKEYBYTES);

  memcpy(outcome->encryption_nonce, client->server_app_hmac, crypto_box_NONCEBYTES);

  // hash(hash(hash(K | a_s * b_p | a_s * B_p | A_s * b_p)) | A_p)
  memcpy(tmp + crypto_hash_sha256_BYTES, client->pub, crypto_sign_PUBLICKEYBYTES);
  crypto_hash_sha256(outcome->decryption_key, tmp, crypto_hash_sha256_BYTES + crypto_sign_PUBLICKEYBYTES);

  memcpy(outcome->decryption_nonce, client->app_hmac, crypto_box_NONCEBYTES);
}
