// Translation unit that compiles the stb_image_write implementation.
//
// Only the PNG encoder is used (screenshots). The stb repository is pinned by
// commit in CMakeLists.txt.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
