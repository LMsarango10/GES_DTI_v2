#include <Arduino.h>
#include "./metislib_v1.1/metis_algolib.h"

void setup()
{
  Serial.begin(115200);
  Serial.println("Start");
}

void loop()
{
  const char *exampleMac = "76:4a:7f:9b:da:57";
  const char *exampleMac2 = "84:89:ad:29:f8:5f";
  const char *exampleMac3 = "8Z:89:ad:29:f8:5Z";
  const char *exampleMac4 = "8Z:89:ad:2ŋ:f8:5";
  char out_mac[METIS_OUTPUT_HASH_LENGTH];
  bool isDevice = false;
  metis_enable_printing(true);
  printf("TESTING MAC: %s\n", exampleMac);
  if (metis_digest_mac_from_str(exampleMac, (char *)&out_mac) ==
      metis_failure_reason_none)
  {
    printf("OK\n");
    printf("Resulted MAC: %s\n", out_mac);
  }
  else
  {
    printf("FAILED\n");
  }
  printf("TESTING MAC: %s\n", exampleMac2);
  if (metis_is_device(exampleMac2, &isDevice) == metis_failure_reason_none)
  {
    printf("OK\n");
    printf("Is Device?: %s\n", isDevice ? "YES" : "NO");
  }
  else
  {
    printf("FAILED\n");
  }
  printf("TESTING MAC: %s\n", exampleMac3);
  if (metis_is_device(exampleMac3, &isDevice) == metis_failure_reason_none)
  {
    printf("OK\n");
    printf("Is Device?: %s\n", isDevice ? "YES" : "NO");
  }
  else
  {
    printf("FAILED\n");
  }
  printf("TESTING MAC: %s\n", exampleMac4);
  if (metis_digest_mac_from_str(exampleMac4, (char *)&out_mac) ==
      metis_failure_reason_none)
  {
    printf("OK\n");
    printf("Resulted MAC: %s\n", out_mac);
  }
  else
  {
    printf("FAILED\n");
  }
}