/* d_pe.c -- MC1 pixel engine driver (see d_pe.h) */

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"
#include "d_pe.h"

qboolean	pe_present;
qboolean	pe_active;
qboolean	pe_alias;
unsigned	pe_submitted;
unsigned	pe_synced;
unsigned	pe_spans_fence;
qboolean	pe_world;
int		pe_version;
pe_surfedge_t	*pe_surfedges;
int		pe_numsurfedges;
int		pe_maxsurfedges;

#if !defined(MC1)
volatile unsigned	pe_noreg;
#endif

/* 1: spans, 2: world faces as polygons (stage B) */
static cvar_t	r_pe = {"r_pe", "2"};
static const byte	*pe_skin;
static int	pe_skinwidth;

/* model vertices the engine reads after the model is done (MESH) */
#define PE_VERTPOOL	16384
#define PE_MAXTRIS	4096
static finalvert_t	pe_vertpool[PE_VERTPOOL];
static int	pe_vertpos;
static unsigned	pe_mesh_fence;
static unsigned short	pe_order[PE_MAXTRIS];

void PE_Init (void)
{
	Cvar_RegisterVariable (&r_pe);
#if defined(MC1)
	pe_present = (PE_REG(PE_ID) >> 16) == PE_ID_MAGIC;
	pe_version = PE_REG(PE_ID) & 0xffff;
#endif
	if (pe_present)
		Con_Printf ("Pixel engine version %d found.\n", pe_version);
}

void PE_BeginFrame (void)
{
	PE_Finish ();
	pe_active = pe_present && r_pe.value;
	pe_alias = false;
	pe_numsurfedges = 0;
	/* the sky needs version 8; draworder and drawflat stay with the edge sorter */
	pe_world = pe_active && r_pe.value >= 2 && pe_version >= 8 &&
			!r_draworder.value && !r_drawflat.value;
	if (!pe_active)
		return;

	if (pe_world && pe_maxsurfedges < 2 * r_numallocatededges + 64)
	{
		free (pe_surfedges);
		pe_maxsurfedges = 2 * r_numallocatededges + 64;
		pe_surfedges = (pe_surfedge_t *) malloc (pe_maxsurfedges * sizeof(*pe_surfedges));
	}

	PE_Begin (PE_TARGET, 5);
	PE_Ptr (d_viewbuffer);
	PE_Word (screenwidth);
	PE_Ptr (d_pzbuffer);
	PE_Ptr (vid.colormap);
	PE_Ptr (NULL);
	pe_skin = NULL;

	if (pe_world)
	{
		PE_Begin (PE_COVER, 5);
		PE_Word (r_refdef.vrect.x);
		PE_Word (r_refdef.vrect.y);
		PE_Word (r_refdef.vrectright);
		PE_Word (r_refdef.vrectbottom);
		PE_Word (20);
	}
}

void PE_SetSkin (const byte *skin, int skinwidth)
{
	if (skin == pe_skin && skinwidth == pe_skinwidth)
		return;
	pe_skin = skin;
	pe_skinwidth = skinwidth;
	PE_Begin (PE_SKIN, 2);
	PE_Ptr (skin);
	PE_Word (skinwidth);
}

/* A surface command's first two words: its spans, or in stage B its screen
   edges as a polygon (sent by PE_EndSurf after the other 19 words). */
int PE_BeginSurf (surf_t *s, int flags)
{
	if (!pe_world)
	{
		pe_spans_fence = PE_Begin (PE_SPANS, 21);
		PE_Word (flags);
		PE_Ptr (s->spans);
		return 0;
	}
	if (s == &surfaces[1])
	{
		PE_Begin (PE_FILL, 21);
		PE_Word (flags);
		PE_Word (0);
		return 0;
	}
	PE_Begin (PE_POLY, 21 + 3 * s->pe_count);
	PE_Word (flags);
	PE_Word (s->pe_count);
	return s->pe_count;
}

void PE_EndSurf (surf_t *s, int n)
{
	int	i;

	for (i = 0; i < n; i++)
	{
		const pe_surfedge_t *e = &pe_surfedges[s->pe_first + i];

		PE_Word ((e->edge->v_first & 0x7fff) | (e->left << 15) | (e->edge->v_last << 16));
		PE_Word (e->edge->u);
		PE_Word (e->edge->u_step);
	}
}

/* one surface with the gradients D_CalcGradients left behind */
void PE_DrawSpans (surf_t *s, int flags, const int *turb,
		float zorigin, float zstepu, float zstepv)
{
	int	n = PE_BeginSurf (s, flags);

	PE_Ptr (cacheblock);
	PE_Word (cachewidth);
	PE_Word (sadjust);
	PE_Word (tadjust);
	PE_Word (bbextents);
	PE_Word (bbextentt);
	PE_Float (d_sdivzorigin);
	PE_Float (d_sdivzstepu);
	PE_Float (d_sdivzstepv);
	PE_Float (d_tdivzorigin);
	PE_Float (d_tdivzstepu);
	PE_Float (d_tdivzstepv);
	PE_Float (d_ziorigin);
	PE_Float (d_zistepu);
	PE_Float (d_zistepv);
	PE_Float (zorigin);
	PE_Float (zstepu);
	PE_Float (zstepv);
	PE_Ptr (turb);
	PE_EndSurf (s, n);
}

finalvert_t *PE_AllocVerts (int n)
{
	finalvert_t	*v;

	if (pe_vertpos + n > PE_VERTPOOL)
	{
		PE_WaitFor (pe_mesh_fence);
		pe_vertpos = 0;
	}
	v = &pe_vertpool[pe_vertpos];
	pe_vertpos += n;
	return v;
}

/* all the triangles of an unclipped model (version 8) */
qboolean PE_DrawMesh (finalvert_t *verts, mtriangle_t *tris, int numtris, int seamfixup)
{
	int	i;

	if (pe_version < 8 || numtris > PE_MAXTRIS)
		return false;
	if (!pe_order[1])
	{
		for (i = 0; i < PE_MAXTRIS; i++)
			pe_order[i] = i;
	}
	pe_mesh_fence = PE_Begin (PE_MESH, 9);
	PE_Ptr (verts);
	PE_Word (sizeof(finalvert_t));
	PE_Ptr (tris);
	PE_Ptr (pe_order);
	PE_Word (numtris);
	PE_Ptr (NULL);
	PE_Word (PE_MESH_QUAKE);
	PE_Word (0);		/* unclipped, the flags only mark the seam */
	PE_Word (seamfixup);
	return true;
}

void PE_Triangle (const int *p0, const int *p1, const int *p2)
{
	int	i;

	PE_Begin (PE_TRIANGLE, 18);
	for (i = 0; i < 6; i++)
		PE_Word (p0[i]);
	for (i = 0; i < 6; i++)
		PE_Word (p1[i]);
	for (i = 0; i < 6; i++)
		PE_Word (p2[i]);
}
