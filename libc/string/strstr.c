#include <string.h>

char* strstr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;
    
    // Empty needle matches at the beginning
    if (*needle == '\0') return (char*)haystack;
    
    while (*haystack) {
        const char* h = haystack;
        const char* n = needle;
        
        // Compare characters
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        
        // If we reached the end of needle, we found a match
        if (*n == '\0') {
            return (char*)haystack;
        }
        
        haystack++;
    }
    
    return NULL;
}