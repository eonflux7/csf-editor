// Translation unit that compiles the stb_image implementation.
//
// Only the PNG decoder is required by the preview; keeping the format list
// narrow keeps the binary and attack surface small. The dimension cap matches
// the limit enforced by the texture decoders.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 16384
#include <stb_image.h>
