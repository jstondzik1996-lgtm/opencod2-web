#ifndef WEB_WEBGL2_NATIVE_H
#define WEB_WEBGL2_NATIVE_H

/*
 * Native-format draw path for the Direct3D 9 -> WebGL2 translation layer.
 *
 * The legacy path in webgl2_compat.c converts every vertex of every draw
 * into a float array (48 bytes/vertex) on the CPU and re-uploads it with
 * glBufferData. This header describes a draw directly in terms of the
 * Direct3D vertex/index buffer memory, so the GPU reads the original layout
 * (float positions, packed 8-bit colours, float texcoords) from mirrored GL
 * buffers. Static buffers are uploaded once; buffers written through
 * Lock() are re-uploaded only for the dirty byte range.
 *
 * Only plain C types are used so the header can be shared by the device
 * reconstruction, the compat layer and the host-side unit tests.
 */

#ifdef __cplusplus
extern "C" {
#endif

enum {
    WEBGL2_COLOR_RGBA = 0, /* bytes R,G,B,A */
    WEBGL2_COLOR_BGRA = 1, /* bytes B,G,R,A (D3DCOLOR in little-endian memory) */
    WEBGL2_COLOR_ARGB = 2  /* bytes A,R,G,B */
};

typedef struct WebGL2NativeDraw {
    const void *vb_base;     /* start of the whole D3D vertex buffer allocation */
    unsigned int vb_size;    /* size of that allocation in bytes */
    const void *ib_base;     /* start of the whole D3D index buffer allocation */
    unsigned int ib_size;
    unsigned int base_offset;   /* stream offset + BaseVertexIndex * stride, bytes from vb_base */
    int stride;
    int pos_offset;             /* bytes from vertex start */
    int pos_components;         /* 3 or 4 floats */
    int color_offset;           /* -1: no colour stream */
    int color_order;            /* WEBGL2_COLOR_* */
    int tex0_offset;            /* -1: no texcoord stream for stage 0 */
    int tex1_offset;            /* -1: no lightmap texcoord stream */
    unsigned int min_vertex;    /* MinVertexIndex (relative to base_offset) */
    unsigned int num_vertices;  /* NumVertices */
    unsigned int index_offset;  /* byte offset of the first 16-bit index from ib_base */
    int index_count;
} WebGL2NativeDraw;

typedef struct WebGL2Stats {
    unsigned int frames;
    unsigned int native_draws;
    unsigned int legacy_draws;
    unsigned int native_rejects;
    unsigned int mirror_buffers;
    unsigned int mirror_full_uploads;
    unsigned int mirror_partial_uploads;
    double vertex_bytes_uploaded;  /* native path, vertex mirrors */
    double index_bytes_uploaded;   /* native path, index mirrors */
    double legacy_bytes_uploaded;  /* legacy converted vertices + indices */
    unsigned int state_calls_issued;
    unsigned int state_calls_skipped;
    unsigned int uniform_calls_issued;
    unsigned int uniform_calls_skipped;
    unsigned int texture_binds_skipped;
    unsigned int texparam_calls_skipped;
} WebGL2Stats;

/* Returns 1 if the draw was issued, 0 if the caller must use the legacy path. */
int webgl2_draw_native(const WebGL2NativeDraw *draw);
/* 1 unless COD2_GL_LEGACY is set (A/B testing against the old path). */
int webgl2_native_enabled(void);

/* Called from Lock() of D3D vertex/index buffers (size 0 = until the end). */
void webgl2_buffer_written(const void *base, unsigned int offset, unsigned int size);
/* Called before a D3D vertex/index buffer frees its memory. */
void webgl2_buffer_released(const void *base);

/* Fixed-function lights only apply to 3D (not pre-transformed / 2D) draws. */
void webgl2_set_lighting_allowed(int allowed);

/* Once per presented frame: statistics and GL state-cache resynchronisation. */
void webgl2_frame_end(void);
/* Forget every cached GL state value (call after foreign GL code ran). */
void webgl2_state_invalidate(void);

void webgl2_get_stats(WebGL2Stats *out);
void webgl2_reset_stats(void);

/* Pure helpers, exported for unit tests. */
int webgl2_rebase_indices16(const unsigned short *src, int count,
                            unsigned short *dst, unsigned int *out_min, unsigned int *out_max);

#ifdef __cplusplus
}
#endif

#endif
