static unsigned char relocation_target;

// Keep a base relocation in images whose other data contains only relative addresses.
static unsigned char *const relocation_reference __attribute__((used)) = &relocation_target;
