#pragma once

/**
 * @file test_ca_cert.h
 * @brief FujiNet Test CA Certificate for HTTPS/TLS testing
 *
 * This certificate is used to verify self-signed server certificates
 * generated for local testing (e.g., nginx HTTPS proxy).
 *
 * To use this CA, add ?testca=1 to the HTTPS URL.
 *
 * The CA certificate was generated with:
 *   integration-tests/certs/generate_certs.sh
 *
 * Valid for: 2026-2036 (10 years)
 * Supports: localhost, 127.0.0.1, 192.168.1.x, 10.0.0.x, 172.17.0.x
 */

namespace fujinet::platform::esp32 {

// PEM format CA certificate (null-terminated string)
// Each line ends with \n for proper PEM formatting
static const char test_ca_cert_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDXTCCAkWgAwIBAgIUL9/5SX/3jE2wxB8fSfDGVq4b8dEwDQYJKoZIhvcNAQEL\n"
    "BQAwPjEYMBYGA1UEAwwPRnVqaU5ldCBUZXN0IENBMRUwEwYDVQQKDAxGdWppTmV0\n"
    "IFRlc3QxCzAJBgNVBAYTAlVTMB4XDTI2MDIxNjIwMTYxNFoXDTM2MDIxNDIwMTYx\n"
    "NFowPjEYMBYGA1UEAwwPRnVqaU5ldCBUZXN0IENBMRUwEwYDVQQKDAxGdWppTmV0\n"
    "IFRlc3QxCzAJBgNVBAYTAlVTMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKC\n"
    "AQEAmgD4BGQGHOqvHanoCjO0mT+AePnwAXw5f6kT3rKcyH/Mt0JZgXOnN20VFZPG\n"
    "e+HmIMd7yg5xiLZANcUOHdE/jAtsqXYI6JROUmZVZLgiI573mIzyRaf6r3Vkdrvz\n"
    "+nyWK7N+R5toTmENpPT2eLSRp6wX1ud7i08khwpTSN8KDQhADFiokiE9omb/whCH\n"
    "/q6o+Ink00E7Emk/nbVr4uT0X/2fOM3XeZKVVJluqlKIibtAA6dPwY2rtYaXT+lT\n"
    "MPZ5byDDhXmosR6UXnbx9IwJRJwETNWEFJPIyYqA1PFj2RG5Q1rrNAyqBm5Knbd6\n"
    "0T6cr3GpLKcWZbsU2xnO3LkGmQIDAQABo1MwUTAdBgNVHQ4EFgQUKibh0bfSX16/\n"
    "vasASdxNTHhPFzgwHwYDVR0jBBgwFoAUKibh0bfSX16/vasASdxNTHhPFzgwDwYD\n"
    "VR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAOxw67lQkFJN6o17GOZmQ\n"
    "JqE4TTvD52klgwFMnb48Ttb97c53+r+z35Er583iiZHMUdlP4mWI4u5/TyLWZrrU\n"
    "oisVPyuum3DBXPWsJoCwis+QBxl0vQf5cUjqSTNveRWrOi5nwXHelXu/+zvG5rzv\n"
    "mg2VBt3ioVzoSjNE4mR/7gMKAzlzz2jK7ZXpsAQMYUE2iCO3JNNQd6fsNQpQpoCZ\n"
    "fBLihGf5W6O41byRBRXGQvp8hi/4JDoS/G+UcTQOIjy2thSrYL4xCzClba+cPeVo\n"
    "BUEbuQBXGIafcTqKAKstBSsNMOibc9NR1Jf7zFOrNx8iWtiCuxus59wiAzr7e2yc\n"
    "oA==\n"
    "-----END CERTIFICATE-----\n";

} // namespace fujinet::platform::esp32