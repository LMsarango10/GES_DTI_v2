/*
Metis MAC Hashing Algorithm Library

Copyright (C) 2019 Purple Blob S.L. - All Rights Reserved

Please refer to the file "LICENSE" for the full license governing this code.
*/

#ifndef METIS_ALGOLIB_H
#define METIS_ALGOLIB_H

#include <mbedtls/sha1.h>

#include <stdio.h>
#include <WString.h>
#include <array>
#include <iterator>
#include <algorithm>

extern const uint8_t METIS_OUTPUT_HASH_LENGTH;

void metis_digest_mac(uint8_t *mac_input, char *out_buf);

void metis_digest_mac_salt(uint8_t *mac_input, uint32_t salt, char *out_buf);

bool metis_is_device(uint8_t *mac_input);

#endif /* METIS_ALGOLIB_H */
