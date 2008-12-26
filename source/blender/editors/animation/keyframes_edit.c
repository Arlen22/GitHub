/**
 * $Id: 
 *
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * Contributor(s): Joshua Leung
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include "BLI_blenlib.h"
#include "BLI_arithb.h"

#include "DNA_action_types.h"
#include "DNA_curve_types.h"
#include "DNA_ipo_types.h"
#include "DNA_key_types.h"
#include "DNA_object_types.h"
#include "DNA_space_types.h"
#include "DNA_scene_types.h"

#include "BKE_action.h"
#include "BKE_ipo.h"
#include "BKE_key.h"
#include "BKE_utildefines.h"

#include "ED_anim_api.h"
#include "ED_keyframes_edit.h"
#include "ED_markers.h"

/* This file defines an API and set of callback-operators for editing keyframe data.
 *
 * Two API functions are defined for actually performing the operations on the data:
 *			ipo_keys_bezier_loop() and icu_keys_bezier_loop()
 * which take the data they operate on, a few callbacks defining what operations to perform.
 *
 * As operators which work on keyframes usually apply the same operation on all BezTriples in 
 * every channel, the code has been optimised providing a set of functions which will get the 
 * appropriate bezier-modify function to set. These functions (ANIM_editkeyframes_*) will need
 * to be called before getting any channels.
 * 
 * A set of 'validation' callbacks are provided for checking if a BezTriple should be operated on.
 * These should only be used when using a 'general' BezTriple editor (i.e. selection setters which 
 * don't check existing selection status).
 * 
 * - Joshua Leung, Dec 2008
 */

/* ************************************************************************** */
/* IPO Editing Loops - Exposed API */

// FIXME: it would be useful to be able to supply custom properties to the bezt function...
// workaround for those callbacks that need this now, is to set globals...

/* --------------------------- Base Functions ------------------------------------ */

/* This function is used to loop over BezTriples in the given IpoCurve, applying a given 
 * operation on them, and optionally applies an IPO-curve validate function afterwards.
 */
short icu_keys_bezier_loop(BeztEditData *bed, IpoCurve *icu, BeztEditFunc bezt_ok, BeztEditFunc bezt_cb, IcuEditFunc icu_cb) 
{
    BezTriple *bezt;
	int b;
	
	/* if function to apply to bezier curves is set, then loop through executing it on beztriples */
    if (bezt_cb) {
		/* if there's a validation func, include that check in the loop 
		 * (this is should be more efficient than checking for it in every loop)
		 */
		if (bezt_ok) {
			for (b=0, bezt=icu->bezt; b < icu->totvert; b++, bezt++) {
				/* Only operate on this BezTriple if it fullfills the criteria of the validation func */
				if (bezt_ok(bed, bezt)) {
					/* Exit with return-code '1' if function returns positive
					 * This is useful if finding if some BezTriple satisfies a condition.
					 */
			        if (bezt_cb(bed, bezt)) return 1;
				}
			}
		}
		else {
			for (b=0, bezt=icu->bezt; b < icu->totvert; b++, bezt++) {
				/* Exit with return-code '1' if function returns positive
				 * This is useful if finding if some BezTriple satisfies a condition.
				 */
		        if (bezt_cb(bed, bezt)) return 1;
			}
		}
    }

    /* if ipocurve_function has been specified then execute it */
    if (icu_cb)
        icu_cb(icu);
	
	/* done */	
    return 0;
}

/* This function is used to loop over the IPO curves (and subsequently the keyframes in them) */
short ipo_keys_bezier_loop(BeztEditData *bed, Ipo *ipo, BeztEditFunc bezt_ok, BeztEditFunc bezt_cb, IcuEditFunc icu_cb)
{
    IpoCurve *icu;
	
	/* Sanity check */
	if (ipo == NULL)
		return 0;
	
    /* Loop through each curve in the Ipo */
    for (icu= ipo->curve.first; icu; icu=icu->next) {
        if (icu_keys_bezier_loop(bed, icu, bezt_ok, bezt_cb, icu_cb))
            return 1;
    }

    return 0;
}

/* -------------------------------- Further Abstracted ----------------------------- */

/* This function is used to apply operation to all keyframes, regardless of the type */
short animchannel_keys_bezier_loop(BeztEditData *bed, bAnimListElem *ale, BeztEditFunc bezt_ok, BeztEditFunc bezt_cb, IcuEditFunc icu_cb)
{
	return 0;
}

/* ************************************************************************** */
/* BezTriple Validation Callbacks */

static short ok_bezier_frame(BeztEditData *bed, BezTriple *bezt)
{
	/* frame is stored in f1 property (this float accuracy check may need to be dropped?) */
	return IS_EQ(bezt->vec[1][0], bed->f1);
}

static short ok_bezier_framerange(BeztEditData *bed, BezTriple *bezt)
{
	/* frame range is stored in float properties */
	return ((bezt->vec[1][0] > bed->f1) && (bezt->vec[1][0] < bed->f2));
}

static short ok_bezier_selected(BeztEditData *bed, BezTriple *bezt)
{
	/* this macro checks all beztriple handles for selection... */
	return BEZSELECTED(bezt);
}

static short ok_bezier_value(BeztEditData *bed, BezTriple *bezt)
{
	/* value is stored in f1 property 
	 *	- this float accuracy check may need to be dropped?
	 *	- should value be stored in f2 instead so that we won't have conflicts when using f1 for frames too?
	 */
	return IS_EQ(bezt->vec[1][1], bed->f1);
}


BeztEditFunc ANIM_editkeyframes_ok(short mode)
{
	/* eEditKeyframes_Validate */
	switch (mode) {
		case BEZT_OK_FRAME: /* only if bezt falls on the right frame (float) */
			return ok_bezier_frame;
		case BEZT_OK_FRAMERANGE: /* only if bezt falls within the specified frame range (floats) */
			return ok_bezier_framerange;
		case BEZT_OK_SELECTED:	/* only if bezt is selected */
			return ok_bezier_selected;
		case BEZT_OK_VALUE: /* only if bezt value matches (float) */
			return ok_bezier_value;
		default: /* nothing was ok */
			return NULL;
	}
}

/* ******************************************* */
/* Transform */

static short snap_bezier_nearest(BeztEditData *bed, BezTriple *bezt)
{
	if (bezt->f2 & SELECT)
		bezt->vec[1][0]= (float)(floor(bezt->vec[1][0]+0.5));
	return 0;
}

static short snap_bezier_nearestsec(BeztEditData *bed, BezTriple *bezt)
{
	const Scene *scene= bed->scene;
	const float secf = FPS;
	
	if (bezt->f2 & SELECT)
		bezt->vec[1][0]= (float)(floor(bezt->vec[1][0]/secf + 0.5f) * secf);
	return 0;
}

static short snap_bezier_cframe(BeztEditData *bed, BezTriple *bezt)
{
	const Scene *scene= bed->scene;
	if (bezt->f2 & SELECT)
		bezt->vec[1][0]= (float)CFRA;
	return 0;
}

static short snap_bezier_nearmarker(BeztEditData *bed, BezTriple *bezt)
{
	//if (bezt->f2 & SELECT)
	//	bezt->vec[1][0]= (float)find_nearest_marker_time(bezt->vec[1][0]);  // XXX missing function!
	return 0;
}

// calchandles_ipocurve
BeztEditFunc ANIM_editkeyframes_snap(short type)
{
	/* eEditKeyframes_Snap */
	switch (type) {
		case SNAP_KEYS_NEARFRAME: /* snap to nearest frame */
			return snap_bezier_nearest;
		case SNAP_KEYS_CURFRAME: /* snap to current frame */
			return snap_bezier_cframe;
		case SNAP_KEYS_NEARMARKER: /* snap to nearest marker */
			return snap_bezier_nearmarker;
		case SNAP_KEYS_NEARSEC: /* snap to nearest second */
			return snap_bezier_nearestsec;
		default: /* just in case */
			return snap_bezier_nearest;
	}
}

/* --------- */

static short mirror_bezier_cframe(BeztEditData *bed, BezTriple *bezt)
{
	const Scene *scene= bed->scene;
	float diff;
	
	if (bezt->f2 & SELECT) {
		diff= ((float)CFRA - bezt->vec[1][0]);
		bezt->vec[1][0]= ((float)CFRA + diff);
	}
	
	return 0;
}

static short mirror_bezier_yaxis(BeztEditData *bed, BezTriple *bezt)
{
	float diff;
	
	if (bezt->f2 & SELECT) {
		diff= (0.0f - bezt->vec[1][0]);
		bezt->vec[1][0]= (0.0f + diff);
	}
	
	return 0;
}

static short mirror_bezier_xaxis(BeztEditData *bed, BezTriple *bezt)
{
	float diff;
	
	if (bezt->f2 & SELECT) {
		diff= (0.0f - bezt->vec[1][1]);
		bezt->vec[1][1]= (0.0f + diff);
	}
	
	return 0;
}

static short mirror_bezier_marker(BeztEditData *bed, BezTriple *bezt)
{
	static TimeMarker *marker;
	static short initialised = 0;
	const Scene *scene= bed->scene;
	
	/* In order for this mirror function to work without
	 * any extra arguments being added, we use the case
	 * of bezt==NULL to denote that we should find the 
	 * marker to mirror over. The static pointer is safe
	 * to use this way, as it will be set to null after 
	 * each cycle in which this is called.
	 */
	
	if (bezt) {
		/* mirroring time */
		if ((bezt->f2 & SELECT) && (marker)) {
			const float diff= (marker->frame - bezt->vec[1][0]);
			bezt->vec[1][0]= (marker->frame + diff);
		}
	}
	else {
		/* initialisation time */
		if (initialised) {
			/* reset everything for safety */
			marker = NULL;
			initialised = 0;
		}
		else {
			/* try to find a marker */
			for (marker= scene->markers.first; marker; marker=marker->next) {
				if (marker->flag & SELECT) {
					initialised = 1;
					break;
				}
			}			
			
			if (initialised == 0) 
				marker = NULL;
		}
	}
	
	return 0;
}

/* Note: for markers case, need to set global vars (eww...) */
// calchandles_ipocurve
BeztEditFunc ANIM_editkeyframes_mirror(short type)
{
	switch (type) {
		case 1: /* mirror over current frame */
			return mirror_bezier_cframe;
		case 2: /* mirror over frame 0 */
			return mirror_bezier_yaxis;
		case 3: /* mirror over value 0 */
			return mirror_bezier_xaxis;
		case 4: /* mirror over marker */
			return mirror_bezier_marker; // XXX in past, this func was called before/after with NULL, probably will need globals instead
		default: /* just in case */
			return mirror_bezier_yaxis;
			break;
	}
}

/* This function is called to calculate the average location of the
 * selected keyframes, and place the current frame at that location.
 *
 * It must be called like so:
 *	snap_cfra_ipo_keys(scene, NULL, -1); // initialise the static vars first
 *	for (ipo...) snap_cfra_ipo_keys(scene, ipo, 0); // sum up keyframe times
 *	snap_cfra_ipo_keys(scene, NULL, 1); // set current frame after taking average
 */
// XXX this thing needs to be refactored!
void snap_cfra_ipo_keys(BeztEditData *bed, Ipo *ipo, short mode)
{
	static int cfra;
	static int tot;
	
	Scene *scene= bed->scene;
	IpoCurve *icu;
	BezTriple *bezt;
	int a;
	
	
	if (mode == -1) {
		/* initialise a new snap-operation */
		cfra= 0;
		tot= 0;
	}
	else if (mode == 1) {
		/* set current frame - using average frame */
		if (tot != 0)
			CFRA = cfra / tot;
	}
	else {
		/* loop through keys in ipo, summing the frame
		 * numbers of those that are selected 
		 */
		if (ipo == NULL) 
			return;
		
		for (icu= ipo->curve.first; icu; icu= icu->next) {
			for (a=0, bezt=icu->bezt; a < icu->totvert; a++, bezt++) {
				if (BEZSELECTED(bezt)) {
					cfra += bezt->vec[1][0];
					tot++;
				}
			}
		}
	}	
}

/* ******************************************* */
/* Settings */

/* Sets the selected bezier handles to type 'auto' */
static short set_bezier_auto(BeztEditData *bed, BezTriple *bezt) 
{
	/* is a handle selected? If so set it to type auto */
	if((bezt->f1  & SELECT) || (bezt->f3 & SELECT)) {
		if (bezt->f1 & SELECT) bezt->h1= 1; /* the secret code for auto */
		if (bezt->f3 & SELECT) bezt->h2= 1;
		
		/* if the handles are not of the same type, set them
		 * to type free
		 */
		if (bezt->h1 != bezt->h2) {
			if ELEM(bezt->h1, HD_ALIGN, HD_AUTO) bezt->h1= HD_FREE;
			if ELEM(bezt->h2, HD_ALIGN, HD_AUTO) bezt->h2= HD_FREE;
		}
	}
	return 0;
}

/* Sets the selected bezier handles to type 'vector'  */
static short set_bezier_vector(BeztEditData *bed, BezTriple *bezt) 
{
	/* is a handle selected? If so set it to type vector */
	if ((bezt->f1 & SELECT) || (bezt->f3 & SELECT)) {
		if (bezt->f1 & SELECT) bezt->h1= HD_VECT;
		if (bezt->f3 & SELECT) bezt->h2= HD_VECT;
		
		/* if the handles are not of the same type, set them
		 * to type free
		 */
		if (bezt->h1 != bezt->h2) {
			if ELEM(bezt->h1, HD_ALIGN, HD_AUTO) bezt->h1= HD_FREE;
			if ELEM(bezt->h2, HD_ALIGN, HD_AUTO) bezt->h2= HD_FREE;
		}
	}
	return 0;
}

#if 0 // xxx currently not used (only used by old code as a check)
static short bezier_isfree(BeztEditData *bed, BezTriple *bezt) 
{
	/* queries whether the handle should be set
	 * to type 'free' or 'align'
	 */
	if ((bezt->f1 & SELECT) && (bezt->h1)) return 1;
	if ((bezt->f3 & SELECT) && (bezt->h2)) return 1;
	return 0;
}

static short set_bezier_align(BeztEditData *bed, BezTriple *bezt) 
{
	/* Sets selected bezier handles to type 'align' */
	if (bezt->f1 & SELECT) bezt->h1= HD_ALIGN;
	if (bezt->f3 & SELECT) bezt->h2= HD_ALIGN;
	return 0;
}
#endif // xxx currently not used (only used by old code as a check, but can't replicate that now)

static short set_bezier_free(BeztEditData *bed, BezTriple *bezt) 
{
	/* Sets selected bezier handles to type 'free'  */
	if (bezt->f1 & SELECT) bezt->h1= HD_FREE;
	if (bezt->f3 & SELECT) bezt->h2= HD_FREE;
	return 0;
}

/* Set all Bezier Handles to a single type */
// calchandles_ipocurve
BeztEditFunc ANIM_editkeyframes_sethandles(short code)
{
	switch (code) {
		case 1: /* auto */
			return set_bezier_auto;
		case 2: /* vector */
			return set_bezier_vector;
			
		default: /* free or align? */
			return set_bezier_free; // err.. to set align, we need 'align' to be set
	}
}

#if 0
void sethandles_ipo_keys(Ipo *ipo, int code)
{
	/* this function lets you set bezier handles all to
	 * one type for some Ipo's (e.g. with hotkeys through
	 * the action window).
	 */ 

	/* code==1: set autohandle */
	/* code==2: set vectorhandle */
	/* als code==3 (HD_ALIGN) toggelt het, vectorhandles worden HD_FREE */
	
	switch (code) {
	case 1: /* auto */
		ipo_keys_bezier_loop(ipo, set_bezier_auto, calchandles_ipocurve);
		break;
	case 2: /* vector */
		ipo_keys_bezier_loop(ipo, set_bezier_vector, calchandles_ipocurve);
		break;
	default: /* free or align? */
		if (ipo_keys_bezier_loop(ipo, bezier_isfree, NULL)) /* free */ 
			ipo_keys_bezier_loop(ipo, set_bezier_free, calchandles_ipocurve);
		else /* align */
			ipo_keys_bezier_loop(ipo, set_bezier_align, calchandles_ipocurve);
		break;
	}
}
#endif

/* ------- */

void set_ipocurve_mixed(IpoCurve *icu)
{
	/* Sets the type of the IPO curve to mixed, as some (selected)
	 * keyframes were set to other interpolation modes
	 */
	icu->ipo= IPO_MIXED;
	
	/* recalculate handles, as some changes may have occurred */
	calchandles_ipocurve(icu);
}

static short set_bezt_constant(BeztEditData *bed, BezTriple *bezt) 
{
	if (bezt->f2 & SELECT) 
		bezt->ipo= IPO_CONST;
	return 0;
}

static short set_bezt_linear(BeztEditData *bed, BezTriple *bezt) 
{
	if (bezt->f2 & SELECT) 
		bezt->ipo= IPO_LIN;
	return 0;
}

static short set_bezt_bezier(BeztEditData *bed, BezTriple *bezt) 
{
	if (bezt->f2 & SELECT) 
		bezt->ipo= IPO_BEZ;
	return 0;
}

/* Set the interpolation type of the selected BezTriples in each IPO curve to the specified one */
// set_ipocurve_mixed() !
BeztEditFunc ANIM_editkeyframes_ipo(short code)
{
	switch (code) {
		case 1: /* constant */
			return set_bezt_constant;
		case 2: /* linear */	
			return set_bezt_linear;
		default: /* bezier */
			return set_bezt_bezier;
	}
}

#if 0
void setipotype_ipo(Ipo *ipo, int code)
{
	/* Sets the type of the selected bezts in each ipo curve in the
	 * Ipo to a value based on the code
	 */
	switch (code) {
	case 1:
		ipo_keys_bezier_loop(ipo, set_bezt_constant, set_ipocurve_mixed);
		break;
	case 2:
		ipo_keys_bezier_loop(ipo, set_bezt_linear, set_ipocurve_mixed);
		break;
	case 3:
		ipo_keys_bezier_loop(ipo, set_bezt_bezier, set_ipocurve_mixed);
		break;
	}
}
#endif

// XXX will we keep this?
void setexprap_ipoloop(Ipo *ipo, int code)
{
	IpoCurve *icu;
	
	/* Loop through each curve in the Ipo */
	for (icu=ipo->curve.first; icu; icu=icu->next)
		icu->extrap= code;
}

/* ******************************************* */
/* Selection */

static short select_bezier_add(BeztEditData *bed, BezTriple *bezt) 
{
	/* Select the bezier triple */
	BEZ_SEL(bezt);
	return 0;
}

static short select_bezier_subtract(BeztEditData *bed, BezTriple *bezt) 
{
	/* Deselect the bezier triple */
	BEZ_DESEL(bezt);
	return 0;
}

static short select_bezier_invert(BeztEditData *bed, BezTriple *bezt) 
{
	/* Invert the selection for the bezier triple */
	bezt->f2 ^= SELECT;
	if (bezt->f2 & SELECT) {
		bezt->f1 |= SELECT;
		bezt->f3 |= SELECT;
	}
	else {
		bezt->f1 &= ~SELECT;
		bezt->f3 &= ~SELECT;
	}
	return 0;
}

// NULL
BeztEditFunc ANIM_editkeyframes_select(short selectmode)
{
	switch (selectmode) {
		case SELECT_ADD: /* add */
			return select_bezier_add;
		case SELECT_SUBTRACT: /* subtract */
			return select_bezier_subtract;
		case SELECT_INVERT: /* invert */
			return select_bezier_invert;
		default: /* replace (need to clear all, then add) */
			return select_bezier_add;
	}
}


short is_ipo_key_selected(Ipo *ipo)
{
	IpoCurve *icu;
	BezTriple *bezt;
	int i;
	
	if (ipo == NULL)
		return 0;
	
	for (icu=ipo->curve.first; icu; icu=icu->next) {
		for (i=0, bezt=icu->bezt; i<icu->totvert; i++, bezt++) {
			if (BEZSELECTED(bezt))
				return 1;
		}
	}
	
	return 0;
}

void set_ipo_key_selection(Ipo *ipo, short sel)
{
	IpoCurve *icu;
	BezTriple *bezt;
	int i;
	
	if (ipo == NULL)
		return;
	
	for (icu=ipo->curve.first; icu; icu=icu->next) {
		for (i=0, bezt=icu->bezt; i<icu->totvert; i++, bezt++) {
			if (sel == 2) {
				BEZ_INVSEL(bezt);
			}
			else if (sel == 1) {
				BEZ_SEL(bezt);
			}
			else {
				BEZ_DESEL(bezt);
			}
		}
	}
}

// err... this is this still used?
int fullselect_ipo_keys(Ipo *ipo)
{
	IpoCurve *icu;
	int tvtot = 0;
	int i;
	
	if (!ipo)
		return tvtot;
	
	for (icu=ipo->curve.first; icu; icu=icu->next) {
		for (i=0; i<icu->totvert; i++) {
			if (icu->bezt[i].f2 & SELECT) {
				tvtot+=3;
				icu->bezt[i].f1 |= SELECT;
				icu->bezt[i].f3 |= SELECT;
			}
		}
	}
	
	return tvtot;
}


void borderselect_icu_key(IpoCurve *icu, float xmin, float xmax, BeztEditFunc select_cb)
{
	/* Selects all bezier triples in the Ipocurve 
	 * between times xmin and xmax, using the selection
	 * function.
	 */
	BezTriple *bezt;
	int i;
	
	/* loop through all of the bezier triples in
	 * the Ipocurve -- if the triple occurs between
	 * times xmin and xmax then select it using the selection
	 * function
	 */
	for (i=0, bezt=icu->bezt; i<icu->totvert; i++, bezt++) {
		if ((bezt->vec[1][0] > xmin) && (bezt->vec[1][0] < xmax)) {
			/* scene is NULL (irrelevant here) */
			select_cb(NULL, bezt); 
		}
	}
}

void borderselect_ipo_key(Ipo *ipo, float xmin, float xmax, short selectmode)
{
	/* Selects all bezier triples in each Ipocurve of the
	 * Ipo between times xmin and xmax, using the selection mode.
	 */
	
	IpoCurve *icu;
	BeztEditFunc select_cb;
	
	/* If the ipo is no good then return */
	if (ipo == NULL)
		return;
	
	/* Set the selection function based on the
	 * selection mode.
	 */
	select_cb= ANIM_editkeyframes_select(selectmode);
	if (select_cb == NULL)
		return;
	
	/* loop through all of the bezier triples in all
		* of the Ipocurves -- if the triple occurs between
		* times xmin and xmax then select it using the selection
		* function
		*/
	for (icu=ipo->curve.first; icu; icu=icu->next) {
		borderselect_icu_key(icu, xmin, xmax, select_cb);
	}
}

void select_icu_key(BeztEditData *bed, IpoCurve *icu, float selx, short selectmode)
{
    /* Selects all bezier triples in the Ipocurve
	 * at time selx, using the selection mode.
	 * This is kind of sloppy the obvious similarities
	 * with the above function, forgive me ...
	 */
    BeztEditFunc select_cb;
	BezTriple *bezt;
	int i;
	
    /* If the icu is no good then return */
    if (icu == NULL)
        return;
	
    /* Set the selection function based on the selection mode. */
    switch (selectmode) {
		case SELECT_ADD:
			select_cb = select_bezier_add;
			break;
		case SELECT_SUBTRACT:
			select_cb = select_bezier_subtract;
			break;
		case SELECT_INVERT:
			select_cb = select_bezier_invert;
			break;
		default:
			return;
    }
	
    /* loop through all of the bezier triples in
	 * the Ipocurve -- if the triple occurs at
	 * time selx then select it using the selection
	 * function
	 */
    for (i=0, bezt=icu->bezt; i<icu->totvert; i++, bezt++) {
        if (bezt->vec[1][0] == selx) {
            select_cb(bed, bezt);
        }
    }
}

void select_ipo_key(BeztEditData *bed, Ipo *ipo, float selx, short selectmode)
{
	/* Selects all bezier triples in each Ipocurve of the
	 * Ipo at time selx, using the selection mode.
	 */
	IpoCurve *icu;
	BezTriple *bezt;
	BeztEditFunc select_cb;
	int i;
	
	/* If the ipo is no good then return */
	if (ipo == NULL)
		return;
	
	/* Set the selection function based on the
	 * selection mode.
	 */
	select_cb= ANIM_editkeyframes_select(selectmode);
	if (select_cb == NULL)
		return;
	
	/* loop through all of the bezier triples in all
	 * of the Ipocurves -- if the triple occurs at
	 * time selx then select it using the selection
	 * function
	 */
	for (icu=ipo->curve.first; icu; icu=icu->next) {
		for (i=0, bezt=icu->bezt; i<icu->totvert; i++, bezt++) {
			if (bezt->vec[1][0] == selx) {
				select_cb(bed, bezt);
			}
		}
	}
}


