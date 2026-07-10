#ifndef TOTP_H
#define TOTP_H

/* 
 * Verifies a 6-digit TOTP code against a base32 encoded secret.
 * Returns 1 if valid and not reused, 0 otherwise.
 */
int totp_verify(const char *secret, const char *code);

#endif
