#ifndef _TGZ_FSFOOLS_
#define _TGZ_FSFOOLS_

// Figure out the fs::FS library to load for the **destination** filesystem


#if defined DEST_FS_USES_SPIFFS || defined DEST_FS_USES_LITTLEFS || defined DEST_FS_USES_FFAT
  #define WARN_LIMITED_FS
#endif


#include "ESP32-targz-lib.h"

#endif
