#include "error_codes.h"

const char* umm_error_string(int code)
{
    switch (code) {
    case UMM_OK:                return "OK";
    case UMM_E_INVALID_ARG:     return "Invalid argument";
    case UMM_E_NOT_FOUND:       return "Not found";
    case UMM_E_NO_MEMORY:       return "Out of memory";
    case UMM_E_ALREADY_EXISTS:  return "Already exists";
    case UMM_E_RPC_ERROR:       return "RPC error";
    case UMM_E_TIMEOUT:         return "Operation timed out";
    case UMM_E_TRANSPORT_ERROR: return "Transport error";
    case UMM_E_NOT_INITIALIZED: return "Not initialized";
    case UMM_E_UNSUPPORTED:     return "Unsupported operation";
    case UMM_E_UNKNOWN:         return "Unknown error";
    default:                    return "Unknown error";
    }
}
