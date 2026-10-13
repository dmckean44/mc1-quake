/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// d_edge.c

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"
#include "d_pe.h"

static int	miplevel;

float		scale_for_mip;
int			screenwidth;
int			ubasestep, errorterm, erroradjustup, erroradjustdown;
int			vstartscan;

// FIXME: should go away
extern void			R_RotateBmodel (void);
extern void			R_TransformFrustum (void);

vec3_t		transformed_modelorg;

/*
==============
D_DrawPoly

==============
*/
void D_DrawPoly (void)
{
// this driver takes spans, not polygons
}


/*
=============
D_MipLevelForScale
=============
*/
int D_MipLevelForScale (float scale)
{
	int		lmiplevel;

	if (scale >= d_scalemip[0] )
		lmiplevel = 0;
	else if (scale >= d_scalemip[1] )
		lmiplevel = 1;
	else if (scale >= d_scalemip[2] )
		lmiplevel = 2;
	else
		lmiplevel = 3;

	if (lmiplevel < d_minmip)
		lmiplevel = d_minmip;

	return lmiplevel;
}


/*
==============
D_DrawSolidSurface
==============
*/

// FIXME: clean this up

void D_DrawSolidSurface (surf_t *surf, int color)
{
	espan_t	*span;
	byte	*pdest;
	int		u, u2, pix;
	
	pix = (color<<24) | (color<<16) | (color<<8) | color;
	for (span=surf->spans ; span ; span=span->pnext)
	{
		pdest = (byte *)d_viewbuffer + screenwidth*span->v;
		u = span->u;
		u2 = span->u + span->count - 1;
		((byte *)pdest)[u] = pix;

		if (u2 - u < 8)
		{
			for (u++ ; u <= u2 ; u++)
				((byte *)pdest)[u] = pix;
		}
		else
		{
			for (u++ ; u & 3 ; u++)
				((byte *)pdest)[u] = pix;

			u2 -= 4;
			for ( ; u <= u2 ; u+=4)
				*(int *)((byte *)pdest + u) = pix;
			u2 += 4;
			for ( ; u <= u2 ; u++)
				((byte *)pdest)[u] = pix;
		}
	}
}


/*
==============
D_CalcGradients
==============
*/
void D_CalcGradients (msurface_t *pface)
{
	mplane_t	*pplane;
	float		mipscale;
	vec3_t		p_temp1;
	vec3_t		p_saxis, p_taxis;
	float		t;

	pplane = pface->plane;

	mipscale = 1.0F / (float)(1 << miplevel);

	TransformVector (pface->texinfo->vecs[0], p_saxis);
	TransformVector (pface->texinfo->vecs[1], p_taxis);

	t = xscaleinv * mipscale;
	d_sdivzstepu = p_saxis[0] * t;
	d_tdivzstepu = p_taxis[0] * t;

	t = yscaleinv * mipscale;
	d_sdivzstepv = -p_saxis[1] * t;
	d_tdivzstepv = -p_taxis[1] * t;

	d_sdivzorigin = p_saxis[2] * mipscale - xcenter * d_sdivzstepu -
			ycenter * d_sdivzstepv;
	d_tdivzorigin = p_taxis[2] * mipscale - xcenter * d_tdivzstepu -
			ycenter * d_tdivzstepv;

	VectorScale (transformed_modelorg, mipscale, p_temp1);

	t = 0x10000*mipscale;
	sadjust = ((fixed16_t)(DotProduct (p_temp1, p_saxis) * 0x10000 + 0.5f)) -
			((pface->texturemins[0] << 16) >> miplevel)
			+ pface->texinfo->vecs[0][3]*t;
	tadjust = ((fixed16_t)(DotProduct (p_temp1, p_taxis) * 0x10000 + 0.5f)) -
			((pface->texturemins[1] << 16) >> miplevel)
			+ pface->texinfo->vecs[1][3]*t;

//
// -1 (-epsilon) so we never wander off the edge of the texture
//
	bbextents = ((pface->extents[0] << 16) >> miplevel) - 1;
	bbextentt = ((pface->extents[1] << 16) >> miplevel) - 1;
}


static vec3_t	world_transformed_modelorg;

static void D_EnterBmodel (surf_t *s)
{
	vec3_t	local_modelorg;

// FIXME: we don't want to do all this for every polygon!
// TODO: store once at start of frame
	currententity = s->entity;	//FIXME: make this passed in to
								// R_RotateBmodel ()
	VectorSubtract (r_origin, currententity->origin, local_modelorg);
	TransformVector (local_modelorg, transformed_modelorg);

	R_RotateBmodel ();	// FIXME: don't mess with the frustum,
						// make entity passed in
}

static void D_LeaveBmodel (void)
{
//
// restore the old drawing state
// FIXME: we don't want to do this every time!
// TODO: speed up
//
	currententity = &cl_entities[0];
	VectorCopy (world_transformed_modelorg,
				transformed_modelorg);
	VectorCopy (base_vpn, vpn);
	VectorCopy (base_vup, vup);
	VectorCopy (base_vright, vright);
	VectorCopy (base_modelorg, modelorg);
	R_TransformFrustum ();
}

/*
==============
D_DrawSurface
==============
*/
static void D_DrawSurface (surf_t *s)
{
	msurface_t		*pface;
	surfcache_t		*pcurrentcache;

	r_drawnpolycount++;

	d_zistepu = s->d_zistepu;
	d_zistepv = s->d_zistepv;
	d_ziorigin = s->d_ziorigin;

	if (s->flags & SURF_DRAWSKY)
	{
		if (!r_skymade && !(pe_active && pe_version >= 12))
		{
			R_MakeSky ();
		}

		if (pe_active)
			PE_DrawSky (s);
		else
		{
			D_DrawSkyScans8 (s->spans);
			D_DrawZSpans (s->spans);
		}
	}
	else if (s->flags & SURF_DRAWBACKGROUND)
	{
	// set up a gradient for the background surface that places it
	// effectively at infinity distance from the viewpoint
		d_zistepu = 0;
		d_zistepv = 0;
		d_ziorigin = -0.9;

		if (pe_active)
			PE_DrawSpans (s, PE_SPAN_FLAT | PE_SPAN_Z | (((int)r_clearcolor.value & 0xFF) << 8),
					NULL, d_ziorigin, d_zistepu, d_zistepv);
		else
		{
			D_DrawSolidSurface (s, (int)r_clearcolor.value & 0xFF);
			D_DrawZSpans (s->spans);
		}
	}
	else if (s->flags & SURF_DRAWTURB)
	{
		pface = s->data;
		miplevel = 0;
		cacheblock = (pixel_t *)
				((byte *)pface->texinfo->texture +
				pface->texinfo->texture->offsets[0]);
		cachewidth = 64;

		if (s->insubmodel)
			D_EnterBmodel (s);

		D_CalcGradients (pface);
		if (pe_active)
			PE_DrawSpans (s, PE_SPAN_TEX | PE_SPAN_TURB | PE_SPAN_Z,
					sintable + ((int)(cl.time*SPEED)&(CYCLE-1)),
					d_ziorigin, d_zistepu, d_zistepv);
		else
		{
			Turbulent8 (s->spans);
			D_DrawZSpans (s->spans);
		}

		if (s->insubmodel)
			D_LeaveBmodel ();
	}
	else
	{
		if (s->insubmodel)
			D_EnterBmodel (s);

		pface = s->data;
		miplevel = D_MipLevelForScale (s->nearzi * scale_for_mip
		* pface->texinfo->mipadjust);

	// FIXME: make this passed in to D_CacheSurface
		pcurrentcache = D_CacheSurface (pface, miplevel);

		cacheblock = (pixel_t *)pcurrentcache->data;
		cachewidth = pcurrentcache->width;

		D_CalcGradients (pface);

		if (pe_active)
		{
			PE_DrawSpans (s, PE_SPAN_TEX | PE_SPAN_Z, NULL,
					d_ziorigin, d_zistepu, d_zistepv);
			pcurrentcache->pe_fence = pe_submitted;
		}
		else
		{
			(*d_drawspans) (s->spans);
			D_DrawZSpans (s->spans);
		}

		if (s->insubmodel)
			D_LeaveBmodel ();
	}
}

/*
==============
D_DrawSurfaces
==============
*/
void D_DrawSurfaces (void)
{
	surf_t			*s;

	currententity = &cl_entities[0];
	TransformVector (modelorg, transformed_modelorg);
	VectorCopy (transformed_modelorg, world_transformed_modelorg);

// TODO: could preset a lot of this at mode set time
	if (r_drawflat.value)
	{
		for (s = &surfaces[1] ; s<surface_p ; s++)
		{
			if (!s->spans)
				continue;

			d_zistepu = s->d_zistepu;
			d_zistepv = s->d_zistepv;
			d_ziorigin = s->d_ziorigin;

			if (pe_active)
			{
				PE_DrawSpans (s, PE_SPAN_FLAT | PE_SPAN_Z | (((int)s->data & 0xFF) << 8),
						NULL, d_ziorigin, d_zistepu, d_zistepv);
				continue;
			}
			D_DrawSolidSurface (s, (int)s->data & 0xFF);
			D_DrawZSpans (s->spans);
		}
	}
	else
	{
		for (s = &surfaces[1] ; s<surface_p ; s++)
		{
			if (s->spans)
				D_DrawSurface (s);
		}
	}
}

/*
==============
PE_DrawWorld

Stage B: the pixel engine fills each surface where no earlier one has drawn,
so the surfaces go out front to back. World surfaces are already in BSP key
order, brush model surfaces go in at the key of their leaf, and the
background fills what is left.
==============
*/
qboolean	pe_defer_builds;

void PE_DrawWorld (surf_t *world_end)
{
	static surf_t	**bmodels;
	static int	maxbmodels;
	int		i, j, numbmodels = surface_p - world_end;
	surf_t		*s;

	if (numbmodels > maxbmodels)
	{
		free (bmodels);
		maxbmodels = numbmodels * 2;
		bmodels = malloc (maxbmodels * sizeof(*bmodels));
	}
	for (i = 0; i < numbmodels; i++)
	{
		s = world_end + i;
		for (j = i; j > 0 && (bmodels[j - 1]->key > s->key ||
			(bmodels[j - 1]->key == s->key && bmodels[j - 1]->nearzi < s->nearzi)); j--)
		{
			bmodels[j] = bmodels[j - 1];
		}
		bmodels[j] = s;
	}

	currententity = &cl_entities[0];
	VectorCopy (base_modelorg, modelorg);
	TransformVector (modelorg, transformed_modelorg);
	VectorCopy (transformed_modelorg, world_transformed_modelorg);

	pe_defer_builds = pe_version >= 3;
	i = 0;
	for (s = &surfaces[2]; s < world_end; s++)
	{
		for ( ; i < numbmodels && bmodels[i]->key < s->key; i++)
			D_DrawSurface (bmodels[i]);
		D_DrawSurface (s);
	}
	for ( ; i < numbmodels; i++)
		D_DrawSurface (bmodels[i]);
	pe_defer_builds = false;

	D_DrawSurface (&surfaces[1]);
}

