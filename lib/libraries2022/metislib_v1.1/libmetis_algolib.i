 %module metisalgo
 %include "typemaps.i"
 %include "stdint.i"
 %{
 /* Includes the header in the wrapper code */
#include "metis_algolib.h"
 %}
 %apply bool *OUTPUT { bool* out };
 %include "metis_algolib.h"