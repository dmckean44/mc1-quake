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
// d_part.c: software driver module for drawing particles

#include "quakedef.h"
#include "d_local.h"
#include "d_pe.h"


/* With engine version 13 the particles of a frame go out as one batch
   (PARTICLES): x, y, z and colour per particle, transformed by the engine.
   Two buffers, each reused once the engine is done with it. */
#define PE_MAXPART	2048
static float		pe_part[2][PE_MAXPART][4];
static unsigned		pe_part_fence[2];
static int		pe_part_buf, pe_numpart;
static qboolean		pe_part_batch;

/*
==============
D_EndParticles
==============
*/
void D_EndParticles (void)
{
	int	i;

	if (!pe_part_batch)
		return;
	pe_part_batch = false;
	if (!pe_numpart)
		return;
	pe_part_fence[pe_part_buf] = PE_Begin (PE_PARTICLES, 24);
	PE_Ptr (pe_part[pe_part_buf]);
	PE_Word (pe_numpart);
	for (i = 0; i < 3; i++)
		PE_Float (r_origin[i]);
	for (i = 0; i < 3; i++)
		PE_Float (r_pright[i]);
	for (i = 0; i < 3; i++)
		PE_Float (r_pup[i]);
	for (i = 0; i < 3; i++)
		PE_Float (r_ppn[i]);
	PE_Float (xcenter);
	PE_Float (ycenter);
	PE_Word (d_vrectx);
	PE_Word (d_vrecty);
	PE_Word (d_vrectright_particle);
	PE_Word (d_vrectbottom_particle);
	PE_Word (d_pix_shift);
	PE_Word (d_pix_min);
	PE_Word (d_pix_max);
	PE_Float (PARTICLE_Z_CLIP);
	pe_part_buf ^= 1;
}

/*
==============
D_StartParticles
==============
*/
void D_StartParticles (void)
{
	pe_part_batch = pe_active && pe_version >= 13 && d_y_aspect_shift == 0;
	if (pe_part_batch)
	{
		PE_WaitFor (pe_part_fence[pe_part_buf]);
		pe_numpart = 0;
	}
}

#if	!id386

/*
==============
D_DrawParticle
==============
*/
void D_DrawParticle (particle_t *pparticle)
{
	vec3_t	local, transformed;
	float	zi;
	byte	*pdest;
	short	*pz;
	int		i, izi, pix, count, u, v;

	int		_screenwidth = screenwidth;
	unsigned int	_d_zwidth = d_zwidth;
	int		_d_y_aspect_shift = d_y_aspect_shift;

	if (pe_part_batch)
	{
		if (pe_numpart < PE_MAXPART)
		{
			float	*rec = pe_part[pe_part_buf][pe_numpart++];

			rec[0] = pparticle->org[0];
			rec[1] = pparticle->org[1];
			rec[2] = pparticle->org[2];
			((int *)rec)[3] = (int)pparticle->color & 0xff;	/* opaque */
		}
		return;
	}

// transform point
	VectorSubtract (pparticle->org, r_origin, local);

	transformed[0] = DotProduct(local, r_pright);
	transformed[1] = DotProduct(local, r_pup);
	transformed[2] = DotProduct(local, r_ppn);		

	if (transformed[2] < PARTICLE_Z_CLIP)
		return;

// project the point
// FIXME: preadjust xcenter and ycenter
	zi = 1.0f / transformed[2];
	u = (int)(xcenter + zi * transformed[0] + 0.5f);
	v = (int)(ycenter - zi * transformed[1] + 0.5f);

	if ((v > d_vrectbottom_particle) || 
		(u > d_vrectright_particle) ||
		(v < d_vrecty) ||
		(u < d_vrectx))
	{
		return;
	}

	pz = d_pzbuffer + (_d_zwidth * v) + u;
	pdest = d_viewbuffer + d_scantable[v] + u;
	izi = (int)(zi * 0x8000);

	pix = izi >> d_pix_shift;

	if (pix < d_pix_min)
		pix = d_pix_min;
	else if (pix > d_pix_max)
		pix = d_pix_max;

	if (pe_active && _d_y_aspect_shift == 0)
	{
		PE_Begin (PE_PARTICLE, 6);
		PE_Word (u);
		PE_Word (v);
		PE_Word (izi);
		PE_Word (pix);
		PE_Word ((int)pparticle->color & 0xff);
		PE_Word (PE_PARTICLE_OPAQUE);
		return;
	}

	switch (pix)
	{
	case 1:
		count = 1 << _d_y_aspect_shift;

		for ( ; count ; count--, pz += _d_zwidth, pdest += _screenwidth)
		{
			if (pz[0] <= izi)
			{
				pz[0] = izi;
				pdest[0] = pparticle->color;
			}
		}
		break;

	case 2:
		count = 2 << _d_y_aspect_shift;

		for ( ; count ; count--, pz += _d_zwidth, pdest += _screenwidth)
		{
			if (pz[0] <= izi)
			{
				pz[0] = izi;
				pdest[0] = pparticle->color;
			}

			if (pz[1] <= izi)
			{
				pz[1] = izi;
				pdest[1] = pparticle->color;
			}
		}
		break;

	case 3:
		count = 3 << _d_y_aspect_shift;

		for ( ; count ; count--, pz += _d_zwidth, pdest += _screenwidth)
		{
			if (pz[0] <= izi)
			{
				pz[0] = izi;
				pdest[0] = pparticle->color;
			}

			if (pz[1] <= izi)
			{
				pz[1] = izi;
				pdest[1] = pparticle->color;
			}

			if (pz[2] <= izi)
			{
				pz[2] = izi;
				pdest[2] = pparticle->color;
			}
		}
		break;

	case 4:
		count = 4 << _d_y_aspect_shift;

		for ( ; count ; count--, pz += _d_zwidth, pdest += _screenwidth)
		{
			if (pz[0] <= izi)
			{
				pz[0] = izi;
				pdest[0] = pparticle->color;
			}

			if (pz[1] <= izi)
			{
				pz[1] = izi;
				pdest[1] = pparticle->color;
			}

			if (pz[2] <= izi)
			{
				pz[2] = izi;
				pdest[2] = pparticle->color;
			}

			if (pz[3] <= izi)
			{
				pz[3] = izi;
				pdest[3] = pparticle->color;
			}
		}
		break;

	default:
		count = pix << _d_y_aspect_shift;

		for ( ; count ; count--, pz += _d_zwidth, pdest += _screenwidth)
		{
			for (i=0 ; i<pix ; i++)
			{
				if (pz[i] <= izi)
				{
					pz[i] = izi;
					pdest[i] = pparticle->color;
				}
			}
		}
		break;
	}
}

#endif	// !id386

