#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png/png.h>
#include <math.h>

/*
 Vertex DMA buffer. Oversized just because.
 */
#define VERTBUF_SIZE (1536 * 1024)

uint8_t __attribute__((aligned(32))) list_vert_buf[VERTBUF_SIZE];

/*
 PVR init params. This sample uses TR polygons only.
*/

pvr_init_params_t pvr_params = {
    {PVR_BINSIZE_16, 0, 0, 0, 0},
    (VERTBUF_SIZE) / 2,
    1, // dma enabled
    0, // fsaa
    1, // 1 is autosort disabled
    2, // extra OPBs
    0, // Vertex buffer double-buffering enabled
};

/*
The following structs, macros and functions are adaptations of actual code from Doom 64 for Dreamcast.
`dmaListVert_t` used to contain several more members for additional vertex lighting.
That functionality has been stripped.

Note that this code can process both triangles and quads, although in this example program it is only
being used with triangles.
*/

typedef struct
{
    pvr_vertex_t *v; // 0
    float w;         // 4
} dmaListVert_t;

typedef struct
{
    unsigned n_verts;
    pvr_poly_hdr_t *hdr;
    dmaListVert_t __attribute__((aligned(32))) dVerts[5];
} dmaPoly_t;

static int debug_color = 0;
static int drawn = 0;
static int material_change = 0;

#define approx_recip(x) (1.0f / sqrtf((x) * (x)))

#define lerp(a, b, t) ((a) + (((b) - (a)) * (t)))

#define transform_dmaListVert(dmav) mat_trans_single3_nodivw((dmav)->v->x, (dmav)->v->y, (dmav)->v->z, (dmav)->w)

/*
credit to Kazade / glDC code for my near-z clipping implementation
https://github.com/Kazade/GLdc/blob/572fa01b03b070e8911db43ca1fb55e3a4f8bdd5/GL/platforms/software.c#L140
*/

// check for vertices behind near-z plane
//
// q bit is set when input poly is a quad instead of triangle
//
// all bits 1 mean all vertices are visible
//
//  q v3 v2 v1 v0
//  -------------
//  0  0  0  0  0  triangle none visible
//  0  0  0  0  1  triangle vert 0 visible
//  ...
//  0  0  1  1  1  all verts of triangle visible
//  1  0  0  0  0  quad none visible
//  1  0  1  0  0  quad vert 2 visible
//  ...
//  1  1  1  1  1  all verts of a quad visible
static inline unsigned nearz_vismask(dmaPoly_t *poly)
{
    unsigned nvert = (unsigned)poly->n_verts;
    unsigned rvm = (nvert & 4) << 2; //(nvert == 4) ? 16 : 0;

    dmaListVert_t *vi = poly->dVerts;
    for (unsigned i = 0; i < nvert; i++)
    {
        rvm |= ((vi->v->z >= -vi->w) << i);
        vi++;
    }

    return rvm;
}

// lerp two 32-bit colors
static uint32_t color_lerp(float ft, uint32_t c1, uint32_t c2)
{
    uint8_t t = (ft * 255);
    uint32_t maskRB = 0x00FF00FF; // Mask for Red & Blue channels
    uint32_t maskG = 0x0000FF00;  // Mask for Green channel
    uint32_t maskA = 0xFF000000;  // Mask for Alpha channel

    // Interpolate Red & Blue
    uint32_t rb = ((((c2 & maskRB) - (c1 & maskRB)) * t) >> 8) + (c1 & maskRB);

    // Interpolate Green
    uint32_t g = ((((c2 & maskG) - (c1 & maskG)) * t) >> 8) + (c1 & maskG);

    // Interpolate Alpha
    uint32_t a = ((((c2 & maskA) >> 24) - ((c1 & maskA) >> 24)) * t) >> 8;
    a = (a + (c1 >> 24)) << 24; // Shift back into position

    return (a & maskA) | (rb & maskRB) | (g & maskG);
}

// lerp two dmaListVert_t
// called if any input verts are determined to be behind the near-z plane
static void nearz_clip(const dmaListVert_t *restrict v1,
                       const dmaListVert_t *restrict v2,
                       dmaListVert_t *out)
{
    const float d0 = v1->w + v1->v->z;
    const float d1 = v2->w + v2->v->z;
    const float diff_d1d0 = (d1 - d0);

    float t = fabsf(d0) * approx_recip(diff_d1d0);

    out->w = lerp(v1->w, v2->w, t);

    out->v->x = lerp(v1->v->x, v2->v->x, t);
    out->v->y = lerp(v1->v->y, v2->v->y, t);
    out->v->z = lerp(v1->v->z, v2->v->z, t);

    out->v->u = lerp(v1->v->u, v2->v->u, t);
    out->v->v = lerp(v1->v->v, v2->v->v, t);

    out->v->argb = color_lerp(t, v1->v->argb, v2->v->argb);
    out->v->oargb = color_lerp(t, v1->v->oargb, v2->v->oargb);
}

// initialize a dmaPoly_t * for rendering the next polygon
// n_verts	3 for triangle
//			4 for quad
// diffuse_hdr is pointer to header to submit if a new pvr header submission is required
static void init_poly(int list, dmaPoly_t *poly, pvr_poly_hdr_t *diffuse_hdr, unsigned n_verts)
{
    void *list_tail = (void *)pvr_vertbuf_tail(list);

    poly->n_verts = n_verts;

    // header always points to next usable position in vertbuf/DMA list
    poly->hdr = (pvr_poly_hdr_t *)list_tail;

    if (material_change)
    {
        // copy the contents of the header into poly struct
        memcpy(poly->hdr, diffuse_hdr, sizeof(pvr_poly_hdr_t));

        // advance the vertbuf/DMA list position
        list_tail += sizeof(pvr_poly_hdr_t);
    }

    // set up 5 dmaListVert_t entries
    // each entry maintains a pointer into the vertbuf/DMA list for a vertex
    // near-z clipping is done in-place in the vertbuf/DMA list
    // some quad clipping cases require an extra vert added to triangle strip
    // this necessitates having contiguous space for 5 pvr_vertex_t available
    dmaListVert_t *dv = poly->dVerts;
    for (unsigned i = 0; i < 5; i++)
    {
        // each dmaListVert_t gets a pointer to the corresponding pvr_vertex_t
        (dv++)->v = (pvr_vertex_t *)list_tail;
        list_tail += sizeof(pvr_vertex_t);
        // advance the vertbuf/DMA list position for next vert
    }
}

static unsigned __attribute__((noinline)) clip_poly(dmaPoly_t *p, unsigned p_vismask);

// this is the main event
// given an unclipped, world-space polygon
// representing a triangle or quad from a model
//
// transform the polygon vertices from world space into view space
//
// clip vertices in-place in the list-to-DMA against near-z plane
//
// perspective-divide the resultant vertices after clipping
//  so they are screen space and ready to give to PVR/TA
//
// advance list-to-DMA position by sizeof(pvr_poly_hdr_t)
//  plus number of post-clip vertices * sizeof(pvr_vertex_t)
//
// return to rendering code for next polygon
static void submit_poly(int list, dmaPoly_t *p)
{
    unsigned i;
    unsigned p_vismask;
    unsigned verts_to_process = p->n_verts;

    // apply loaded transform matrix to each vertex
    // transform is a single `mat_trans_single3_nodivw` per vertex
    dmaListVert_t *dv = p->dVerts;
    for (i = 0; i < verts_to_process; i++)
    {
        transform_dmaListVert(dv);
        dv++;
    }

    // test for poly behind near-z plane
    p_vismask = nearz_vismask(p);

    // 0 or 16 means nothing visible, this happens
    if (!(p_vismask & ~16))
        return;

    // set vert flags to defaults for poly type
    p->dVerts[0].v->flags = p->dVerts[1].v->flags = PVR_CMD_VERTEX;
    p->dVerts[3].v->flags = p->dVerts[4].v->flags = PVR_CMD_VERTEX_EOL;

    if (verts_to_process == 4)
        p->dVerts[2].v->flags = PVR_CMD_VERTEX;
    else
        p->dVerts[2].v->flags = PVR_CMD_VERTEX_EOL;

    verts_to_process = clip_poly(p, p_vismask);
    if (!verts_to_process)
        return;

    drawn++;

    dv = p->dVerts;
    // perspective divide every vert to submit
    for (i = 0; i < verts_to_process; i++)
    {
        pvr_vertex_t *pv = dv->v;
        float invw = approx_recip(dv->w);
        pv->x *= invw;
        pv->y *= invw;
        pv->z = invw;

        dv++;
    }

    uint32_t amount = (material_change * sizeof(pvr_poly_hdr_t)) + (verts_to_process * sizeof(pvr_vertex_t));
    material_change = 0;
    // update diffuse/DMA list pointer
    pvr_vertbuf_written(list, amount);
}

static unsigned __attribute__((noinline)) clip_poly(dmaPoly_t *p, unsigned p_vismask)
{
    unsigned verts_to_process = p->n_verts;

    switch (p_vismask)
    {
    // tri only 0 visible
    case 1:
        nearz_clip(&p->dVerts[0], &p->dVerts[1], &p->dVerts[1]);
        nearz_clip(&p->dVerts[0], &p->dVerts[2], &p->dVerts[2]);

        break;

    // tri only 1 visible
    case 2:
        nearz_clip(&p->dVerts[0], &p->dVerts[1], &p->dVerts[0]);
        nearz_clip(&p->dVerts[1], &p->dVerts[2], &p->dVerts[2]);

        break;

    // tri 0 + 1 visible
    case 3:
        verts_to_process = 4;

        nearz_clip(&p->dVerts[1], &p->dVerts[2], &p->dVerts[3]);
        nearz_clip(&p->dVerts[0], &p->dVerts[2], &p->dVerts[2]);

        p->dVerts[2].v->flags = PVR_CMD_VERTEX;

        break;

    // tri only 2 visible
    case 4:
        nearz_clip(&p->dVerts[0], &p->dVerts[2], &p->dVerts[0]);
        nearz_clip(&p->dVerts[1], &p->dVerts[2], &p->dVerts[1]);

        break;

    // tri 0 + 2 visible
    case 5:
        verts_to_process = 4;

        nearz_clip(&p->dVerts[1], &p->dVerts[2], &p->dVerts[3]);
        nearz_clip(&p->dVerts[0], &p->dVerts[1], &p->dVerts[1]);

        p->dVerts[2].v->flags = PVR_CMD_VERTEX;

        break;

    // tri 1 + 2 visible
    case 6:
        verts_to_process = 4;

        memcpy(p->dVerts[3].v, p->dVerts[2].v, sizeof(pvr_vertex_t));
        p->dVerts[3].w = p->dVerts[2].w;

        nearz_clip(&p->dVerts[0], &p->dVerts[2], &p->dVerts[2]);
        nearz_clip(&p->dVerts[0], &p->dVerts[1], &p->dVerts[0]);

        p->dVerts[2].v->flags = PVR_CMD_VERTEX;
        break;

    // tri all visible
    case 7:

        break;

    // quad only 0 visible
    case 17:
        verts_to_process = 3;

        nearz_clip(&p->dVerts[0], &p->dVerts[1], &p->dVerts[1]);
        nearz_clip(&p->dVerts[0], &p->dVerts[2], &p->dVerts[2]);

        p->dVerts[2].v->flags = PVR_CMD_VERTEX_EOL;

        break;

    // quad only 1 visible
    case 18:
        verts_to_process = 3;

        nearz_clip(&p->dVerts[0], &p->dVerts[1], &p->dVerts[0]);
        nearz_clip(&p->dVerts[1], &p->dVerts[3], &p->dVerts[2]);

        p->dVerts[2].v->flags = PVR_CMD_VERTEX_EOL;

        break;

    // quad 0 + 1 visible
    case 19:
        nearz_clip(&p->dVerts[0], &p->dVerts[2], &p->dVerts[2]);
        nearz_clip(&p->dVerts[1], &p->dVerts[3], &p->dVerts[3]);

        break;

    // quad only 2 visible
    case 20:
        verts_to_process = 3;

        nearz_clip(&p->dVerts[0], &p->dVerts[2], &p->dVerts[0]);
        nearz_clip(&p->dVerts[2], &p->dVerts[3], &p->dVerts[1]);

        p->dVerts[2].v->flags = PVR_CMD_VERTEX_EOL;

        break;

    // quad 0 + 2 visible
    case 21:
        nearz_clip(&p->dVerts[0], &p->dVerts[1], &p->dVerts[1]);
        nearz_clip(&p->dVerts[2], &p->dVerts[3], &p->dVerts[3]);

        break;

    // quad 1 + 2 visible is not possible
    // it is a middle diagonal
    case 22:
        verts_to_process = 0;

        break;

    // quad 0 + 1 + 2 visible
    case 23:
        verts_to_process = 5;

        nearz_clip(&p->dVerts[2], &p->dVerts[3], &p->dVerts[4]);
        nearz_clip(&p->dVerts[1], &p->dVerts[3], &p->dVerts[3]);

        p->dVerts[3].v->flags = PVR_CMD_VERTEX;

        break;

    // quad only 3 visible
    case 24:
        verts_to_process = 3;

        nearz_clip(&p->dVerts[1], &p->dVerts[3], &p->dVerts[0]);
        nearz_clip(&p->dVerts[2], &p->dVerts[3], &p->dVerts[2]);

        memcpy(p->dVerts[1].v, p->dVerts[3].v, sizeof(pvr_vertex_t));
        p->dVerts[1].w = p->dVerts[3].w;

        p->dVerts[1].v->flags = PVR_CMD_VERTEX;
        p->dVerts[2].v->flags = PVR_CMD_VERTEX_EOL;
        break;

    // quad 0 + 3 visible is not possible
    // it is the other middle diagonal
    case 25:
        verts_to_process = 0;

        break;

    // quad 1 + 3 visible
    case 26:
        nearz_clip(&p->dVerts[0], &p->dVerts[1], &p->dVerts[0]);
        nearz_clip(&p->dVerts[2], &p->dVerts[3], &p->dVerts[2]);

        break;

    // quad 0 + 1 + 3 visible
    case 27:
        verts_to_process = 5;

        nearz_clip(&p->dVerts[2], &p->dVerts[3], &p->dVerts[4]);
        nearz_clip(&p->dVerts[0], &p->dVerts[2], &p->dVerts[2]);

        p->dVerts[3].v->flags = PVR_CMD_VERTEX;

        break;

    // quad 2 + 3 visible
    case 28:
        nearz_clip(&p->dVerts[0], &p->dVerts[2], &p->dVerts[0]);
        nearz_clip(&p->dVerts[1], &p->dVerts[3], &p->dVerts[1]);

        break;

    // quad 0 + 2 + 3 visible
    case 29:
        verts_to_process = 5;

        memcpy(p->dVerts[4].v, p->dVerts[3].v, sizeof(pvr_vertex_t));
        p->dVerts[4].w = p->dVerts[3].w;

        nearz_clip(&p->dVerts[1], &p->dVerts[3], &p->dVerts[3]);
        nearz_clip(&p->dVerts[0], &p->dVerts[1], &p->dVerts[1]);

        p->dVerts[3].v->flags = PVR_CMD_VERTEX;
        p->dVerts[4].v->flags = PVR_CMD_VERTEX_EOL;

        break;

    // quad 1 + 2 + 3 visible
    case 30:
        verts_to_process = 5;

        memcpy(p->dVerts[4].v, p->dVerts[2].v, sizeof(pvr_vertex_t));
        p->dVerts[4].w = p->dVerts[2].w;

        nearz_clip(&p->dVerts[0], &p->dVerts[2], &p->dVerts[2]);
        nearz_clip(&p->dVerts[0], &p->dVerts[1], &p->dVerts[0]);

        p->dVerts[3].v->flags = PVR_CMD_VERTEX;
        p->dVerts[4].v->flags = PVR_CMD_VERTEX_EOL;
        break;

    // quad all visible
    case 31:

        break;
    }
    return verts_to_process;
}

/*
This is code originally provided by @_bruceleet. It is an OBJ file loader and viewer.

It used the KOS PVR direct render API.

I adapted it to use the KOS PVR DMA rendering API.
*/

typedef struct
{
    float x, y, z;
    float u, v;
    uint32_t color;
    uint32_t pad[2];
} vertex_t;

typedef struct
{
    int v1, v2, v3;
    int t1, t2, t3;
    int material_id;
    uint32_t pad;
} face_t;

typedef struct
{
    float u, v;
} texcoord_t;

typedef struct
{
    pvr_ptr_t ptr;
    int w, h;
    uint32_t fmt;
} texture_t;

typedef struct
{
    uint32_t hash;
    texture_t *texture;
    pvr_poly_hdr_t *hdr;
    uint32_t kd;
} material_t;

#define MAX_VERTICES 10000
#define MAX_FACES 10000
#define MAX_TEXCOORDS 10000
#define MAX_MATERIALS 128

static vertex_t __attribute__((aligned(32))) vertices[MAX_VERTICES];
static face_t __attribute__((aligned(32))) faces[MAX_FACES];
static texcoord_t __attribute__((aligned(32))) texcoords[MAX_TEXCOORDS];
static material_t __attribute__((aligned(32))) materials[MAX_MATERIALS];

static int num_vertices = 0;
static int num_faces = 0;
static int num_texcoords = 0;
static int num_materials = 0;
static int current_material_id = -1;

static const float SCREEN_CENTER_X = 320.0f;
static const float SCREEN_CENTER_Y = 240.0f;
static const float FOV_COTANGENT = 1.732050808f;

static float cam_x = 0.0f;
static float cam_y = -0.6f;
static float cam_z = 10.0f;
static float cam_yaw = 0.0f;
static float cam_pitch = 0.0f;

static void update_camera(cont_state_t *state)
{
    if (!state)
        return;

    float joy_x = (float)state->joyx / 128.0f;
    float speed_forward = 0.0f;
    float joy_y = (float)state->joyy / 128.0f;

    if (fabs(joy_x) > 0.1f)
    {
        cam_yaw += joy_x * 0.09f;
    }

    speed_forward = -joy_y * 0.51f;

    if (cam_pitch > 1.55f)
        cam_pitch = 1.55f;
    if (cam_pitch < -1.55f)
        cam_pitch = -1.55f;

    float speed_vertical = 0.0f;
    float speed_strafe = 0.0f;

    if (state->rtrig > 0)
        speed_forward = (float)state->rtrig * 0.002f;
    if (state->ltrig > 0)
        speed_forward = -(float)state->ltrig * 0.002f;

    if (state->buttons & CONT_DPAD_UP)
        speed_vertical = 0.15f;
    if (state->buttons & CONT_DPAD_DOWN)
        speed_vertical = -0.15f;
    if (state->buttons & CONT_DPAD_LEFT)
        speed_strafe = -0.15f;
    if (state->buttons & CONT_DPAD_RIGHT)
        speed_strafe = 0.15f;

    float cx = fsin(cam_yaw) * fcos(cam_pitch);
    float cy = fsin(cam_pitch);
    float cz = -fcos(cam_yaw) * fcos(cam_pitch);

    cam_x += cx * speed_forward;
    cam_y += cy * speed_forward;
    cam_z += cz * speed_forward;

    cam_y += speed_vertical;

    float strafe_angle = cam_yaw - (F_PI * 0.5f);
    cam_x += fsin(strafe_angle) * speed_strafe;
    cam_z += -fcos(strafe_angle) * speed_strafe;

    if (state->buttons & CONT_Y)
    {
        cam_x = 0.0f;
        cam_y = -0.6f;
        cam_z = 10.0f;
        cam_yaw = 0.0f;
        cam_pitch = 0.0f;
    }

    if (state->buttons & CONT_A)
    {
        debug_color ^= 1;
    }
}

static texture_t *load_texture(const char *filename)
{
    texture_t *tex = malloc(sizeof(texture_t));
    kos_img_t img;

    // printf("Loading texture: %s\n", filename);

    if (png_to_img(filename, PNG_NO_ALPHA, &img) < 0)
    {
        // printf("Failed to load texture: %s\n", filename);
        free(tex);
        return NULL;
    }

    tex->ptr = pvr_mem_malloc(img.byte_count);
    tex->w = img.w;
    tex->h = img.h;
    tex->fmt = PVR_TXRFMT_RGB565;

    pvr_txr_load_kimg(&img, tex->ptr, 0);
    kos_img_free(&img, 0);

    // printf("Texture loaded successfully: %s (%dx%d)\n", filename, tex->w, tex->h);
    return tex;
}

static unsigned long int djb2_hash(char *s)
{
    unsigned long int result = 5381;
    unsigned long int i;

    result = ((result << 5) ^ result) ^ (s[0] & 0x7f);

    for (i = 1; i < 64 && s[i] != '\0'; ++i)
        result = ((result << 5) ^ result) ^ s[i];

    return result;
}

static int find_material(const char *name)
{
    uint32_t new_hash = djb2_hash(name);

    for (int i = 0; i < num_materials; i++)
    {
        if (materials[i].hash == new_hash)
        {
            return i;
        }
    }
    return -1;
}

static void load_mtl(const char *filename, pvr_list_t list)
{
    FILE *file = fopen(filename, "r");
    if (!file)
    {
        // printf("Failed to load MTL: %s\n", filename);
        return;
    }

    char line[256];
    int current_mat = -1;

    while (fgets(line, sizeof(line), file))
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;

        if (strncmp(line, "newmtl ", 7) == 0)
        {
            if (num_materials >= MAX_MATERIALS)
            {
                // printf("Too many materials!\n");
                break;
            }

            char mat_name[64];
            sscanf(line, "newmtl %63s", mat_name);

            current_mat = num_materials;
            // snprintf(materials[current_mat].name, sizeof(materials[current_mat].name), "%s", mat_name);
            materials[current_mat].hash = djb2_hash(mat_name);
            materials[current_mat].texture = NULL;
            num_materials++;

            // printf("Found material: %s\n", mat_name);

            char tex_path[256];
            snprintf(tex_path, sizeof(tex_path), "/rd/textures/%s_baseColor.png", mat_name);
            materials[current_mat].texture = load_texture(tex_path);
            materials[current_mat].hdr = (pvr_poly_hdr_t *)malloc(sizeof(pvr_poly_hdr_t));
            {
                pvr_poly_cxt_t cxt;

                if (materials[current_mat].texture)
                {
                    pvr_poly_cxt_txr(&cxt, list,
                                     materials[current_mat].texture->fmt,
                                     materials[current_mat].texture->w, materials[current_mat].texture->h,
                                     materials[current_mat].texture->ptr, PVR_FILTER_BILINEAR);
                }
                else
                {
                    pvr_poly_cxt_col(&cxt, list);
                }

                cxt.gen.culling = PVR_CULLING_CCW;
                cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
                cxt.depth.write = PVR_DEPTHWRITE_ENABLE;
                //		        cxt.gen.fog_type = PVR_FOG_TABLE;
                //		        cxt.gen.fog_type2 = PVR_FOG_TABLE;

                pvr_poly_compile(materials[current_mat].hdr, &cxt);
            }
            if (materials[current_mat].texture)
            {
                // printf("Loaded texture for material %s\n", mat_name);
            }
            else
            {
                // printf("No texture found for material %s\n", mat_name);
            }
        }
        else if (strncmp(line, "Kd ", 3) == 0 && current_mat >= 0)
        {
            float nr, ng, nb;
            sscanf(line, "Kd %f %f %f", &nr, &ng, &nb);
            materials[current_mat].kd = (0xff000000) | (((uint8_t)(nr * 255)) << 16) | (((uint8_t)(ng * 255)) << 8) | ((uint8_t)(nb * 255));
        }

        else if (strncmp(line, "map_Kd ", 7) == 0 && current_mat >= 0)
        {
            if (!materials[current_mat].texture)
            {
                char tex_name[128];
                sscanf(line, "map_Kd %s", tex_name);

                char tex_path[256];
                snprintf(tex_path, sizeof(tex_path), "/rd/textures/%s", tex_name);

                materials[current_mat].texture = load_texture(tex_path);

                if (!materials[current_mat].texture)
                {
                    char *dot = strrchr(tex_path, '.');
                    if (dot)
                    {
                        strcpy(dot, ".png");
                        materials[current_mat].texture = load_texture(tex_path);
                    }
                }

                if (materials[current_mat].texture)
                {
                    // printf("Material %08x using texture: %s\n",
                    // materials[current_mat].hash, tex_name);
                }
            }
        }
    }

    fclose(file);
    // printf("Loaded %d materials from MTL\n", num_materials);
}

static void load_obj(const char *filename, pvr_list_t list)
{
    FILE *file = fopen(filename, "r");
    if (!file)
    {
        // printf("Failed to load OBJ: %s\n", filename);
        return;
    }

    num_vertices = 0;
    num_faces = 0;
    num_texcoords = 0;
    current_material_id = -1;

    char line[256];
    float x, y, z, u, v;
    int v1, v2, v3, t1, t2, t3;

    while (fgets(line, sizeof(line), file))
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;

        if (strncmp(line, "mtllib ", 7) == 0)
        {
            char mtl_name[128];
            sscanf(line, "mtllib %s", mtl_name);

            char mtl_path[256];
            char *last_slash = strrchr(filename, '/');
            if (last_slash)
            {
                int dir_len = last_slash - filename + 1;
                strncpy(mtl_path, filename, dir_len);
                mtl_path[dir_len] = '\0';
                strcat(mtl_path, mtl_name);
            }
            else
            {
                strcpy(mtl_path, mtl_name);
            }

            // printf("Loading material library: %s\n", mtl_path);
            load_mtl(mtl_path, list);
        }

        else if (strncmp(line, "usemtl ", 7) == 0)
        {
            char mat_name[64];
            sscanf(line, "usemtl %s", mat_name);
            current_material_id = find_material(mat_name);
            // printf("Using material: %s (id=%d)\n", mat_name, current_material_id);
        }

        else if (line[0] == 'v' && line[1] == ' ')
        {
            if (num_vertices >= MAX_VERTICES)
            {
                // printf("Error: Too many vertices\n");
                break;
            }
            if (sscanf(line, "v %f %f %f", &x, &y, &z) == 3)
            {
                vertices[num_vertices].x = x;
                vertices[num_vertices].y = y;
                vertices[num_vertices].z = z;
                vertices[num_vertices].color = 0xFFFFFFFF;
                num_vertices++;
            }
        }

        else if (line[0] == 'v' && line[1] == 't')
        {
            if (num_texcoords >= MAX_TEXCOORDS)
            {
                // printf("Error: Too many texture coordinates\n");
                break;
            }
            if (sscanf(line, "vt %f %f", &u, &v) == 2)
            {
                texcoords[num_texcoords].u = u;
                texcoords[num_texcoords].v = 1.0f - v;
                num_texcoords++;
            }
        }

        else if (line[0] == 'f')
        {
            if (num_faces >= MAX_FACES)
            {
                // printf("Error: Too many faces\n");
                break;
            }

            int n1, n2, n3;
            int matched = sscanf(line, "f %d/%d/%d %d/%d/%d %d/%d/%d",
                                 &v1, &t1, &n1, &v2, &t2, &n2, &v3, &t3, &n3);

            if (matched != 9)
            {
                matched = sscanf(line, "f %d/%d %d/%d %d/%d",
                                 &v1, &t1, &v2, &t2, &v3, &t3);

                if (matched != 6)
                {
                    matched = sscanf(line, "f %d %d %d", &v1, &v2, &v3);
                    if (matched == 3)
                    {
                        t1 = t2 = t3 = 1;
                    }
                    else
                    {
                        continue;
                    }
                }
            }

            if (v1 <= 0 || v2 <= 0 || v3 <= 0 ||
                v1 > num_vertices || v2 > num_vertices || v3 > num_vertices)
            {
                continue;
            }

            faces[num_faces].v1 = v1 - 1;
            faces[num_faces].v2 = v2 - 1;
            faces[num_faces].v3 = v3 - 1;
            faces[num_faces].t1 = (t1 > 0 && t1 <= num_texcoords) ? t1 - 1 : 0;
            faces[num_faces].t2 = (t2 > 0 && t2 <= num_texcoords) ? t2 - 1 : 0;
            faces[num_faces].t3 = (t3 > 0 && t3 <= num_texcoords) ? t3 - 1 : 0;
            faces[num_faces].material_id = current_material_id;
            num_faces++;
        }
    }

    fclose(file);
    // printf("Loaded OBJ: %d vertices, %d texcoords, %d faces\n",
    // num_vertices, num_texcoords, num_faces);
}

static void setup_matrix(void)
{
    mat_identity();

    mat_perspective(SCREEN_CENTER_X,
                    SCREEN_CENTER_Y,
                    FOV_COTANGENT,
                    0.0f,
                    1000.0f);

    mat_rotate_y(-cam_yaw);
    mat_rotate_x(-cam_pitch);

    mat_translate(-cam_x, -cam_y, -cam_z);

    matrix_t tmp;
    mat_store(&tmp);
    tmp[1][1] = -tmp[1][1];
    mat_load(&tmp);
}

static int render_model(pvr_list_t list)
{
    int last_material = -100;
    dmaPoly_t next_poly;
    dmaListVert_t *dV[3];
    __builtin_prefetch(&faces[0]);

    dV[0] = &next_poly.dVerts[2];
    dV[1] = &next_poly.dVerts[1];
    dV[2] = &next_poly.dVerts[0];

    setup_matrix();
    int last_drawn = 0;
    uint32_t col_to_use = 0xffffffff;
    int use_tex = 0;

    for (int i = 0; i < num_faces; i++)
    {
        __builtin_prefetch(&faces[i + 1]);
        use_tex = 0;
        vertex_t *v1 = &vertices[faces[i].v1];

        float test_x = v1->x - cam_x;
        float test_y = v1->y - cam_y;
        float test_z = v1->z - cam_z;
        float test_len;
        vec3f_length(test_x, test_y, test_z, test_len);

        if (test_len > 35.0f)
            continue;

        last_drawn++;

        vertex_t *v2 = &vertices[faces[i].v2];
        vertex_t *v3 = &vertices[faces[i].v3];

        if (last_material != faces[i].material_id)
        {
            material_change = 1;
            last_material = faces[i].material_id;
            col_to_use = materials[faces[i].material_id].kd;
        }

//        __builtin_prefetch(materials[faces[i].material_id].hdr);

        init_poly(list, &next_poly, materials[faces[i].material_id].hdr, 3);

        dV[0]->v->x = v1->x;
        dV[0]->v->y = v1->y;
        dV[0]->v->z = v1->z;

        if (num_texcoords > 0 && faces[i].t1 >= 0 && faces[i].t1 < num_texcoords)
        {
            dV[0]->v->u = texcoords[faces[i].t1].u;
            dV[0]->v->v = texcoords[faces[i].t1].v;
            use_tex = 1;
        }
        else
        {
            dV[0]->v->u = 0.0f;
            dV[0]->v->v = 0.0f;
        }
        dV[0]->v->argb = debug_color ? 0xffff0000 : col_to_use;
        
        dV[1]->v->x = v2->x;
        dV[1]->v->y = v2->y;
        dV[1]->v->z = v2->z;
        if (use_tex) //num_texcoords > 0 && faces[i].t2 >= 0 && faces[i].t2 < num_texcoords)
        {
            dV[1]->v->u = texcoords[faces[i].t2].u;
            dV[1]->v->v = texcoords[faces[i].t2].v;
        }
        else
        {
            dV[1]->v->u = 0.0f;
            dV[1]->v->v = 0.0f;
        }
        dV[1]->v->argb = debug_color ? 0xff00ff00 : col_to_use;
        dV[2]->v->x = v3->x;
        dV[2]->v->y = v3->y;
        dV[2]->v->z = v3->z;

        if (use_tex) //num_texcoords > 0 && faces[i].t3 >= 0 && faces[i].t3 < num_texcoords)
        {
            dV[2]->v->u = texcoords[faces[i].t3].u;
            dV[2]->v->v = texcoords[faces[i].t3].v;
        }
        else
        {
            dV[2]->v->u = 0.0f;
            dV[2]->v->v = 0.0f;
        }
        dV[2]->v->argb = debug_color ? 0xff0000ff : col_to_use;
        submit_poly(list, &next_poly);
    }

    return last_drawn;
}

int main(int argc, char **argv)
{
    pvr_list_t poly_type = PVR_LIST_OP_POLY;
    pvr_init(&pvr_params);
    pvr_set_vertbuf(poly_type, list_vert_buf, VERTBUF_SIZE);

    load_obj("/rd/test/untitled.obj", poly_type);

    uint32 frames = 0;
    uint32 last_time = timer_ms_gettime64();
    float fps = 0.0f;

    while (1)
    {
        maple_device_t *cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        if (cont)
        {
            cont_state_t *state = (cont_state_t *)maple_dev_status(cont);
            if (state)
            {
                update_camera(state);
                if (state->buttons & CONT_START)
                    break;
            }
        }

        drawn = 0;

        pvr_wait_ready();
        pvr_scene_begin();
        pvr_set_bg_color(0.102, 0.219, 0.165);

        int submitted = render_model(poly_type);

        pvr_scene_finish();

        frames++;
        uint32 current_time = timer_ms_gettime64();
        if (current_time - last_time >= 2000)
        {
            fps = (frames * 1000.0f) / (current_time - last_time);
            printf("FPS: %.2f | Faces: %d (submitted %d, drawn %d) | Materials: %d | Cam: (%.1f, %.1f, %.1f)\n",
                   fps, num_faces, submitted, drawn, num_materials, cam_x, cam_y, cam_z);
            frames = 0;
            last_time = current_time;
        }
    }

    for (int i = 0; i < num_materials; i++)
    {
        if (materials[i].texture)
        {
            pvr_mem_free(materials[i].texture->ptr);
            free(materials[i].texture);
            free(materials[i].hdr);
        }
    }

    exit(0);

    return 0;
}