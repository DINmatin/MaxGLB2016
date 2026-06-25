#include <stdlib.h>

// Visual Studio 2012 besitzt kein C99-atoll().
// Die entsprechende Microsoft-Funktion heißt _atoi64().
#if defined(_MSC_VER) && _MSC_VER < 1800
    #define atoll _atoi64
#endif

#define CGLTF_IMPLEMENTATION
#define CGLTF_WRITE_IMPLEMENTATION
#include "../../third_party/cgltf/cgltf.h"

#if defined(_MSC_VER) && _MSC_VER < 1800
    #undef atoll
#endif