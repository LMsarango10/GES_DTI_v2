/*
Metis MAC Hashing Algorithm Library

Copyright (C) 2022 Purple Blob S.L. - All Rights Reserved

Please refer to the file "LICENSE" for the full license governing this code.
*/

#ifndef METIS_ALGOLIB_H
#define METIS_ALGOLIB_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef enum metis_failure_reason {
    metis_failure_reason_none,
    metis_failure_reason_non_utf8_string_input,
    metis_failure_reason_input_format_error,
    metis_failure_reason_hex_range_error,
} metis_failure_reason;

extern const uint8_t METIS_OUTPUT_HASH_LENGTH;

metis_failure_reason metis_digest_mac_from_str(const char *mac_str, char *out_buf);

metis_failure_reason metis_digest_mac_from_str_salt(const char *mac_str,
                                                    const char *salt_str,
                                                    char *out_buf);

void metis_enable_printing(bool enabled);

metis_failure_reason metis_is_device(const char *mac_str, bool *out);

#endif /* METIS_ALGOLIB_H */