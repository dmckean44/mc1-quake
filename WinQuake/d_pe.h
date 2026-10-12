/*
 * Driver for the MC1 pixel engine: a command FIFO at 0xc0000200 that draws
 * world spans, surface cache blocks, opaque alias triangles and particles.
 * The engine is specified in mc1-quake2's doc/mc1_pixel_engine.md.
 */

#ifndef D_PE_H
#define D_PE_H

#define PE_ID_MAGIC	0x5045

enum {
	PE_ID = 0x00,
	PE_CMD = 0x04,
	PE_SYNC = 0x08,
	PE_WAIT = 0x0c,
	PE_DONE = 0x10,
	PE_BUSY_LO = 0x18,
	PE_STALL_LO = 0x20
};

enum {
	PE_SPANS = 1,
	PE_SURFACE = 2,
	PE_TRIANGLE = 3,
	PE_PARTICLE = 4,
	PE_TARGET = 5,
	PE_SKIN = 6,
	PE_COVER = 7,
	PE_POLY = 8,
	PE_FILL = 9,
	PE_DEFER = 11
};

enum {
	PE_SPAN_TEX = 1,
	PE_SPAN_Z = 2,
	PE_SPAN_TURB = 4,
	PE_SPAN_FLAT = 8,
	PE_SPAN_SKY = 16
};

enum {
	PE_PARTICLE_OPAQUE = 0,
	PE_PARTICLE_66 = 2
};

#if defined(MC1)
#define PE_REG(off) (*(volatile unsigned *)(0xc0000200 + (off)))
#else
extern volatile unsigned pe_noreg;
#define PE_REG(off) pe_noreg
#endif

extern qboolean pe_present;
extern qboolean pe_active;
extern qboolean pe_alias;
extern unsigned pe_submitted;
extern unsigned pe_synced;
extern unsigned pe_spans_fence;
extern qboolean pe_world;
extern qboolean pe_defer_builds;
extern int pe_version;

typedef struct
{
	edge_t		*edge;
	int		left;
} pe_surfedge_t;

extern pe_surfedge_t *pe_surfedges;
extern int pe_numsurfedges;
extern int pe_maxsurfedges;

static inline void PE_AddEdge (edge_t *edge, int left)
{
	if (pe_numsurfedges < pe_maxsurfedges)
	{
		pe_surfedges[pe_numsurfedges].edge = edge;
		pe_surfedges[pe_numsurfedges].left = left;
		pe_numsurfedges++;
	}
}

static inline unsigned PE_Begin (unsigned op, unsigned words)
{
	PE_REG(PE_CMD) = (op << 24) | words;
	return ++pe_submitted;
}

static inline void PE_Word (unsigned x)
{
	PE_REG(PE_CMD) = x;
}

static inline void PE_Float (float x)
{
	union { float f; unsigned u; } c;

	c.f = x;
	PE_Word (c.u);
}

static inline void PE_Ptr (const void *p)
{
	PE_Word ((unsigned)(size_t)p);
}

/* commands finish in order, so waiting for one waits for all before it */
static inline void PE_WaitFor (unsigned id)
{
	if (id > pe_synced)
	{
		PE_REG(PE_WAIT) = id;
		pe_synced = id;
	}
}

static inline void PE_Finish (void)
{
	PE_WaitFor (pe_submitted);
}

void PE_Init (void);
void PE_BeginFrame (void);
void PE_SetSkin (const byte *skin, int skinwidth);
void PE_DrawSpans (surf_t *s, int flags, const int *turb,
		float zorigin, float zstepu, float zstepv);
void PE_DrawWorld (surf_t *world_end);
int PE_BeginSurf (surf_t *s, int flags);
void PE_EndSurf (surf_t *s, int n);
void PE_DrawSky (surf_t *s);
void PE_DrawSurfaceBlocks (void);
void PE_Triangle (const int *p0, const int *p1, const int *p2);

#endif
