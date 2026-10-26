/*
 * Host regression test for colour byte orders on the little-endian (WASM)
 * renderer path. Built freestanding for 32-bit x86, like tests/dobj_trace.
 *
 * 1. IWI bitmaps through the REAL Image_LoadFromData (r_image_load_obj.c):
 *    RGB24 (format 2) must reach Image_UploadData as D3DFMT_X8R8G8B8 in
 *    little-endian memory order B,G,R,X. CDirect3DSurface turns that into
 *    RGBA for WebGL. The old big-endian expansion produced X,R,G,B, i.e.
 *    0xFF in the blue channel after the BGRA->RGBA swizzle (purple tint).
 *    ARGB32 (format 1) is passed through unchanged.
 * 2. Vertex colours: the REAL R_ConvertColorToBytes (r_utils.c) produces the
 *    engine's "native" A,R,G,B bytes and the REAL StdConverterARGB_Convert
 *    (CColorConverter.c, what RB_SetVertexData and the static-model cache
 *    apply) turns them into B,G,R,A bytes. CDirect3DDevice must therefore
 *    decode converter output as BGRA (the static-model 0x40 stream was
 *    decoded as ARGB: alpha landed in blue).
 */
#include "common_types.h"

void Image_LoadFromData(GfxImage *image, GfxImageFileHeader *fileHeader, const byte *srcData);
void R_ConvertColorToBytes(const vec_t *colorFloat, byte *colorBytes);
void StdConverterARGB_Convert(const void *self, void *pDst, const void *pSrc);

/* ---- freestanding runtime ---------------------------------------------- */
static int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
static void out(const char *s)
{
    int n = 0;
    while (s[n])
        ++n;
    sys3(4, 1, (int)s, n);
}
static void out_int(int v)
{
    char buf[16];
    int i = 15;
    unsigned int u = v < 0 ? (unsigned int)-v : (unsigned int)v;
    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0)
        buf[--i] = '-';
    out(&buf[i]);
}
void *memset(void *d, int c, unsigned int n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned int n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

static int g_fail;
static int g_checks;
static void tc_check(int ok, const char *what)
{
    ++g_checks;
    if (!ok) {
        ++g_fail;
        out("FAIL: ");
        out(what);
        out("\n");
    }
}

/* ---- stubs for the image loader ----------------------------------------- */
static unsigned char g_arena[1 << 16];
static unsigned int g_arena_used;
void *Hunk_AllocateTempMemoryInternal(int size)
{
    void *p = &g_arena[g_arena_used];
    g_arena_used += ((unsigned int)size + 15u) & ~15u;
    return p;
}
void Hunk_FreeTempMemory(void *buf)
{
    (void)buf;
    g_arena_used = 0;
}
int Image_CubemapFace(int face) { return face; }

static int g_setup_format;
void Image_Setup(GfxImage *image, int width, int height, int depth, int semantic, int flags, int imageFormat)
{
    (void)semantic;
    (void)flags;
    image->width = (unsigned short)width;
    image->height = (unsigned short)height;
    image->depth = (unsigned short)depth;
    image->mapType = 3;
    image->picmip.platform[0] = 0;
    g_setup_format = imageFormat;
}

static unsigned char g_upload[256];
static int g_upload_format;
static int g_upload_calls;
void Image_UploadData(GfxImage *image, int imageFormat, int face, int mipLevel, const byte *pixels)
{
    unsigned int n = (unsigned int)image->width * image->height * 4u;
    unsigned int i;
    (void)face;
    (void)mipLevel;
    g_upload_format = imageFormat;
    g_upload_calls++;
    for (i = 0; i < n && i < sizeof(g_upload); i++)
        g_upload[i] = pixels[i];
}

/* ---- tests ---------------------------------------------------------------- */
static void test_iwi_rgb24(void)
{
    GfxImage image;
    GfxImageFileHeader hdr;
    /* 2x2 texels as stored in the IWI file: B,G,R per texel (D3D R8G8B8). */
    static const byte texels[12] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 1, 2, 3 };
    int t;
    int ok = 1;

    memset(&image, 0, sizeof(image));
    memset(&hdr, 0, sizeof(hdr));
    hdr.format = 2;    /* RGB24 */
    hdr.flags = 2;     /* no mipmaps */
    hdr.dimensions[0] = 2;
    hdr.dimensions[1] = 2;
    hdr.dimensions[2] = 1;
    g_upload_calls = 0;
    Image_LoadFromData(&image, &hdr, texels);
    tc_check(g_upload_calls == 1, "RGB24: exactly one upload");
    tc_check(g_setup_format == 0x16 && g_upload_format == 0x16, "RGB24 becomes D3DFMT_X8R8G8B8");
    for (t = 0; t < 4; t++) {
        const byte *d = &g_upload[t * 4];
        const byte *s = &texels[t * 3];
        if (d[0] != s[0] || d[1] != s[1] || d[2] != s[2] || d[3] != 0xFF)
            ok = 0;
    }
    tc_check(ok, "RGB24 texels must be uploaded as B,G,R,0xFF (little-endian X8R8G8B8)");
    /* What WebGL finally samples after CDirect3DSurface's BGRA->RGBA swap. */
    tc_check(g_upload[2] == 30 && g_upload[1] == 20 && g_upload[0] == 10,
          "first texel samples as R=30 G=20 B=10 (no forced blue)");
}

static void test_iwi_argb32(void)
{
    GfxImage image;
    GfxImageFileHeader hdr;
    static const byte texels[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    int i;
    int ok = 1;

    memset(&image, 0, sizeof(image));
    memset(&hdr, 0, sizeof(hdr));
    hdr.format = 1;    /* ARGB32: stored B,G,R,A */
    hdr.flags = 2;
    hdr.dimensions[0] = 2;
    hdr.dimensions[1] = 2;
    hdr.dimensions[2] = 1;
    Image_LoadFromData(&image, &hdr, texels);
    tc_check(g_upload_format == 0x15, "ARGB32 becomes D3DFMT_A8R8G8B8");
    for (i = 0; i < 16; i++)
        if (g_upload[i] != texels[i])
            ok = 0;
    tc_check(ok, "ARGB32 texels are uploaded unchanged (already B,G,R,A)");
}

static void tc_decode(const byte *s, int order, byte *rgba)
{
    if (order == 0) { /* RGBA */
        rgba[0] = s[0]; rgba[1] = s[1]; rgba[2] = s[2]; rgba[3] = s[3];
    } else if (order == 2) { /* ARGB */
        rgba[0] = s[1]; rgba[1] = s[2]; rgba[2] = s[3]; rgba[3] = s[0];
    } else { /* BGRA */
        rgba[0] = s[2]; rgba[1] = s[1]; rgba[2] = s[0]; rgba[3] = s[3];
    }
}

static void test_vertex_colour_chain(void)
{
    float colour[4] = { 0.8f, 0.4f, 0.2f, 1.0f }; /* warm brown, opaque */
    byte native[4];
    byte vb[4];
    byte rgba[4];

    R_ConvertColorToBytes(colour, native);
    tc_check(native[0] == 255 && native[1] == 204 && native[2] == 102 && native[3] == 51,
          "R_ConvertColorToBytes writes native A,R,G,B bytes");
    StdConverterARGB_Convert(0, vb, native);
    tc_check(vb[0] == 51 && vb[1] == 102 && vb[2] == 204 && vb[3] == 255,
          "StdConverterARGB turns native A,R,G,B into B,G,R,A bytes");
    tc_decode(vb, 1, rgba);
    tc_check(rgba[0] == 204 && rgba[1] == 102 && rgba[2] == 51 && rgba[3] == 255,
          "BGRA decode of converter output recovers the colour");
    tc_decode(vb, 2, rgba);
    tc_check(rgba[2] == 255 && rgba[3] == 51,
          "ARGB decode of converter output (old static-model path) puts alpha into blue");

    /*
     * Static-model cache (r_staticmodelcache.c, R_SkinStaticModelCachedCmd):
     * XSurface file colour R,G,B,A is first rotated to native A,R,G,B
     * (dst = src[3], src[0], src[1], src[2]) and then converted.
     */
    {
        byte file[4] = { 204, 102, 51, 255 };
        byte rotated[4];
        rotated[0] = file[3];
        rotated[1] = file[0];
        rotated[2] = file[1];
        rotated[3] = file[2];
        StdConverterARGB_Convert(0, vb, rotated);
        tc_decode(vb, 1, rgba);
        tc_check(rgba[0] == 204 && rgba[1] == 102 && rgba[2] == 51 && rgba[3] == 255,
              "static-model cache colours decode correctly as BGRA");
    }
}

static void tests(void)
{
    test_iwi_rgb24();
    test_iwi_argb32();
    test_vertex_colour_chain();
}

__attribute__((force_align_arg_pointer)) void _start(void)
{
    tests();
    if (g_fail) {
        out("texture_colors: ");
        out_int(g_fail);
        out(" of ");
        out_int(g_checks);
        out(" checks FAILED\n");
        sys3(1, 1, 0, 0);
    }
    out("texture_colors: OK (");
    out_int(g_checks);
    out(" checks)\n");
    sys3(1, 0, 0, 0);
}
