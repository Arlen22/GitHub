/*
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
 * along with this program; if not, write to the Free Software  Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2012 Blender Foundation, Joshua Leung
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Original Author: Joshua Leung
 * Contributor(s): Joshua Leung
 *
 * ***** END GPL LICENSE BLOCK *****
 *
 * Implements a brush-based "sculpting" tool for posing rigs
 * in an easier and faster manner.
 */

/** \file blender/editors/armature/pose_sculpt.c
 *  \ingroup edarmature
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "MEM_guardedalloc.h"

#include "BLI_math.h"
#include "BLI_blenlib.h"
#include "BLI_utildefines.h"
#include "BLI_dynstr.h"
#include "BLI_ghash.h"
//#include "BLI_pbvh.h"
#include "BLI_threads.h"
#include "BLI_rand.h"

#include "DNA_action_types.h"
#include "DNA_armature_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_armature.h"
#include "BKE_context.h"
#include "BKE_depsgraph.h"
#include "BKE_report.h"

#include "BLT_translation.h"

#include "BIF_gl.h"
#include "BIF_glutil.h"

#include "ED_armature.h"
#include "ED_screen.h"
#include "ED_view3d.h"

#include "armature_intern.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "WM_api.h"
#include "WM_types.h"

/* ******************************************************** */
/* General settings */

/* Get Pose Sculpt Settings */
PSculptSettings *psculpt_settings(Scene *scene)
{
	return (scene->toolsettings) ? &scene->toolsettings->psculpt : NULL;
}

/* Get current brush */
PSculptBrushData *psculpt_get_brush(Scene *scene)
{
	PSculptSettings *pset = psculpt_settings(scene);
	
	if ((pset) && (pset->brushtype >= 0) && (pset->brushtype < PSCULPT_TOT_BRUSH))
		return &pset->brush[pset->brushtype];
	else
		return NULL;
}

void *psculpt_get_current(Scene *scene, Object *ob)
{
	return NULL; // XXX
}

/* ******************************************************** */
/* Polling Callbacks */

int psculpt_poll(bContext *C)
{
	Scene *scene = CTX_data_scene(C);
	Object *ob = CTX_data_active_object(C);
	
	if (ELEM(NULL, scene, ob))
		return false;
	
	/* we only need to be in pose mode... */
	return ((ob->pose) && (ob->mode & OB_MODE_POSE));
}

int psculpt_poll_view3d(bContext *C)
{
	ScrArea *sa = CTX_wm_area(C);
	ARegion *ar = CTX_wm_region(C);
	
	return ((psculpt_poll(C)) && (sa->spacetype == SPACE_VIEW3D) &&
			(ar->regiontype == RGN_TYPE_WINDOW));
}

/* ******************************************************** */
/* Cursor drawing */

/* Helper callback for drawing the cursor itself */
static void brush_drawcursor(bContext *C, int x, int y, void *UNUSED(customdata))
{
	PSculptBrushData *brush = psculpt_get_brush(CTX_data_scene(C));
	
	if (brush) {
		glPushMatrix();
		
		glTranslatef((float)x, (float)y, 0.0f);
		
		glColor4ub(255, 255, 255, 128);
		
		glEnable(GL_LINE_SMOOTH);
		glEnable(GL_BLEND);
		
		glutil_draw_lined_arc(0.0, M_PI * 2.0, brush->size, 40);
		
		glDisable(GL_BLEND);
		glDisable(GL_LINE_SMOOTH);
		
		glPopMatrix();
	}
}

/* Turn brush cursor in 3D view on/off */
static void psculpt_toggle_cursor(bContext *C, bool enable)
{
	PSculptSettings *pset = psculpt_settings(CTX_data_scene(C));
	
	if (pset->paintcursor && !enable) {
		/* clear cursor */
		WM_paint_cursor_end(CTX_wm_manager(C), pset->paintcursor);
		pset->paintcursor = NULL;
	}
	else if (enable) {
		/* enable cursor */
		pset->paintcursor = WM_paint_cursor_activate(CTX_wm_manager(C), 
		                                             psculpt_poll_view3d, 
		                                             brush_drawcursor, NULL);
	}
}

/* ******************************************************** */
/* Brush Operation Callbacks */

/* Defines ------------------------------------------------ */

/* Struct passed to all callback functions */
typedef struct tPSculptContext {
	/* Relevant context data */
	ViewContext vc;
	
	Scene *scene;
	Object *ob;
	
	/* General Brush Data */
	PSculptBrushData *brush;	/* active brush */
	
	const float *mval;			/* mouse coordinates (pixels) */
	float rad;					/* radius of brush (pixels) */
	float dist;					/* distance from brush to item being sculpted (pixels) */
	float fac;					/* brush strength (factor 0-1) */
	
	short invert;				/* "subtract" mode? */
	short first;				/* first run through? */
	
	/* Brush Specific Data */
	float *dvec;				/* mouse travel vector, or something else */
} tPSculptContext;


/* Affected bones */
typedef struct tAffectedBone {
	struct tAffectedBone *next, *prev;
	
	bPoseChannel *pchan;		/* bone in question */
	float fac;					/* (last) strength factor applied to this bone */
} tAffectedBone;

/* Pose Sculpting brush operator data  */
typedef struct tPoseSculptingOp {
	tPSculptContext data;		/* "context" data to pass to brush callbacks later */
	
	Scene *scene;
	Object *ob;
	
	float lastmouse[2];			/* previous mouse position */
	short first;				/* is this the first time we're applying anything? */
	short timerTick;			/* is the current event being processed due to a timer tick? */
	
	wmTimer *timer;				/* timer for in-place accumulation of brush effect */
	
	GHash *affected_bones;		/* list of bones affected by brush */
} tPoseSculptingOp;

/* Callback Function Signature */
typedef void (*PSculptBrushCallback)(tPoseSculptingOp *pso, tPSculptContext *data, bPoseChannel *pchan, float sco1[2], float sco2[2]);

/* Init ------------------------------------------------ */

static void psculpt_init_context_data(bContext *C, tPSculptContext *data)
{
	memset(data, 0, sizeof(*data));
	
	data->scene = CTX_data_scene(C);
	data->ob = CTX_data_active_object(C);
	
	data->brush = psculpt_get_brush(data->scene);
}

static void psculpt_init_view3d_data(bContext *C, tPSculptContext *data)
{
	/* init context data */
	psculpt_init_context_data(C, data);
	
	/* hook up 3D View context */
	view3d_set_viewcontext(C, &data->vc);
	
#if 0
	/* note, the object argument means the modelview matrix does not account for the objects matrix, use viewmat rather than (obmat * viewmat) */
	view3d_get_transformation(data->vc.ar, data->vc.rv3d, NULL, &data->mats);

	if ((data->vc.v3d->drawtype>OB_WIRE) && (data->vc.v3d->flag & V3D_ZBUF_SELECT)) {
		if (data->vc.v3d->flag & V3D_INVALID_BACKBUF) {
			/* needed or else the draw matrix can be incorrect */
			view3d_operator_needs_opengl(C);
			
			view3d_validate_backbuf(&data->vc);
			/* we may need to force an update here by setting the rv3d as dirty
			 * for now it seems ok, but take care!:
			 * rv3d->depths->dirty = 1; */
			ED_view3d_depth_update(data->vc.ar);
		}
	}
#endif
}

/* Brush Utilities ---------------------------------------- */

/* get euler rotation value to work with */
static short get_pchan_eul_rotation(float eul[3], short *order, const bPoseChannel *pchan)
{
	if (ELEM(pchan->rotmode, ROT_MODE_QUAT, ROT_MODE_AXISANGLE)) {
		/* ensure that we're not totally locked... */
		if ((pchan->protectflag & OB_LOCK_ROT4D) &&
			(pchan->protectflag & (OB_LOCK_ROTX | OB_LOCK_ROTY | OB_LOCK_ROTZ | OB_LOCK_ROTW)))
		{
			/* if 4D locked, then just a single flag can cause trouble = no go */
			return false;
		}
		
		/* set rotation order - dummy default */
		if (order) {
			*order = ROT_MODE_EUL;
		}
		
		/* convert rotations to eulers */
		switch (pchan->rotmode) {
			case ROT_MODE_QUAT:
				quat_to_eulO(eul, ROT_MODE_EUL, pchan->quat);
				break;
			
			case ROT_MODE_AXISANGLE:
				axis_angle_to_eulO(eul, ROT_MODE_EUL, pchan->rotAxis, pchan->rotAngle);
				break;
				
			default:
				/* this can't happen */
				return false;
		}
	}
	else {
		/* copy pchan rotation to edit-euler */
		copy_v3_v3(eul, pchan->eul);
		
		/* set rotation order to whatever it is */
		if (order) {
			*order = pchan->rotmode;
		}
	}
	
	return true;
}

/* flush euler rotation value */
static void set_pchan_eul_rotation(const float eul[3], bPoseChannel *pchan)
{
	switch (pchan->rotmode) {
		case ROT_MODE_QUAT: /* quaternion */
			eulO_to_quat(pchan->quat, eul, ROT_MODE_EUL);
			break;
		
		case ROT_MODE_AXISANGLE: /* axis angle */
			eulO_to_axis_angle(pchan->rotAxis, &pchan->rotAngle, eul, ROT_MODE_EUL);
			break;
		
		default: /* euler */
			copy_v3_v3(pchan->eul, eul);
			break;
	}
}

/* ........................................................ */

/* convert pose-space joints of PoseChannel to loc/rot/scale components 
 * <> pchan: (bPoseChannel) pose channel that we're working on
 * < dvec: (vector) vector indicating direction of bone desired
 */
static void apply_pchan_joints(bPoseChannel *pchan, float dvec[3])
{
	float poseMat[4][4], poseDeltaMat[4][4];
	short locks = pchan->protectflag;
	
	/* 1) build pose matrix
	 *    Use method used for Spline IK in splineik_evaluate_bone() : steps 3,4
	 */
	{
		float dmat[3][3], rmat[3][3], tmat[3][3];
		float raxis[3], rangle;
		float smat[4][4], size[3];
		
		/* get scale factors */
		mat4_to_size(size, pchan->pose_mat);
		
		/* compute the raw rotation matrix from the bone's current matrix by extracting only the
		 * orientation-relevant axes, and normalising them
		 */
		copy_v3_v3(rmat[0], pchan->pose_mat[0]);
		copy_v3_v3(rmat[1], pchan->pose_mat[1]);
		copy_v3_v3(rmat[2], pchan->pose_mat[2]);
		normalize_m3(rmat);
		
		/* also, normalise the orientation imposed by the bone, now that we've extracted the scale factor */
		normalize_v3(dvec);
		
		/* calculate smallest axis-angle rotation necessary for getting from the
		 * current orientation of the bone, to the spline-imposed direction
		 */
		cross_v3_v3v3(raxis, rmat[1], dvec);
		
		rangle = dot_v3v3(rmat[1], dvec);
		rangle = acos( MAX2(-1.0f, MIN2(1.0f, rangle)) );
		
		/* construct rotation matrix from the axis-angle rotation found above 
		 *	- this call takes care to make sure that the axis provided is a unit vector first
		 */
		axis_angle_to_mat3(dmat, raxis, rangle);
		
		/* combine these rotations so that the y-axis of the bone is now aligned as the spline dictates,
		 * while still maintaining roll control from the existing bone animation
		 */
		mul_m3_m3m3(tmat, dmat, rmat); // m1, m3, m2
		normalize_m3(tmat); /* attempt to reduce shearing, though I doubt this'll really help too much now... */
		
		/* apply scaling back onto this */
		size_to_mat4(smat, size);
		mul_m4_m3m4(poseMat, tmat, smat);
		
		/* apply location too */
		copy_v3_v3(poseMat[3], pchan->pose_head);
	}
	
	/* 2) take away restpose so that matrix is fit for low-level */
	{
		//float imat[4][4];
		
		//invert_m4_m4(imat, pchan->bone->arm_mat);
		//mult_m4_m4m4(poseDeltaMat, imat, poseMat);
		
		BKE_armature_mat_pose_to_bone(pchan, poseMat, poseDeltaMat);
	}
	
	/* 3) apply these joints to low-level transforms */
	//if ( (locks & (OB_LOCK_ROTX|OB_LOCK_ROTY|OB_LOCK_ROTZ)) ||
	//	 ((locks & OB_LOCK_ROT4D) && (locks & OB_LOCK_ROTW)) )
	if (locks)
	{
		float dloc[3], dsize[3];
		float rmat[3][3];
		
		float eul[3];
		short rotOrder;
		
		/* decompose to loc, size, and rotation matrix */
		mat4_to_loc_rot_size(dloc, rmat, dsize, poseDeltaMat);
		
		/* only apply location if not locked */
		if ((locks & OB_LOCK_LOCX) == 0) pchan->loc[0] = dloc[0];
		if ((locks & OB_LOCK_LOCY) == 0) pchan->loc[1] = dloc[1];
		if ((locks & OB_LOCK_LOCZ) == 0) pchan->loc[2] = dloc[2];
		
		/* scaling is ignored - it shouldn't have changed for now, so just leave it... */
		
		/* apply rotation matrix if we can */
		if (get_pchan_eul_rotation(eul, &rotOrder, pchan)) {
			float oldeul[3] = {eul[0], eul[1], eul[2]};
			
			/* decompose to euler, then knock out anything bad */
			mat3_to_compatible_eulO(eul, oldeul, rotOrder, rmat);
			
			if (locks & OB_LOCK_ROTX) eul[0] = oldeul[0];
			if (locks & OB_LOCK_ROTY) eul[1] = oldeul[1];
			if (locks & OB_LOCK_ROTZ) eul[2] = oldeul[2];
			
			set_pchan_eul_rotation(eul, pchan);
		}
	}
	else {
		/* no locking - use simpler method */
		BKE_pchan_apply_mat4(pchan, poseDeltaMat, true);
	}
}

/* ........................................................ */

/* check if a bone has already been affected by the brush, and add an entry if not */
static tAffectedBone *verify_bone_is_affected(tPoseSculptingOp *pso, tPSculptContext *data, bPoseChannel *pchan, short add)
{
	/* try to find bone */
	tAffectedBone *tab = BLI_ghash_lookup(pso->affected_bones, pchan);
	
	/* add if not found and we're allowed to */
	if ((tab == NULL) && (add)) {
		tab = MEM_callocN(sizeof(tAffectedBone), "tAffectedBone");
		
		tab->pchan = pchan;
		tab->fac = 0.5f; // placeholder
		
		BLI_ghash_insert(pso->affected_bones, pchan, tab);
	}
	
	return tab;
}

/* free affected bone temp data */
static void free_affected_bone(void *tab_p)
{
	// just a wrapper around this for now
	MEM_freeN(tab_p);
}

/* Brushes ------------------------------------------------ */

/* change selection status of bones - used to define masks */
static void brush_select_bone(tPoseSculptingOp *pso, tPSculptContext *data, bPoseChannel *pchan, float UNUSED(sco1[2]), float UNUSED(sco2[2]))
{
	if (pchan->bone) {
		if (data->invert)
			pchan->bone->flag &= ~BONE_SELECTED;
		else
			pchan->bone->flag |= BONE_SELECTED;
	}
}

/* "comb" brush - inspired by Particle Comb */
static void brush_comb(tPoseSculptingOp *pso, tPSculptContext *data, bPoseChannel *pchan, float sco1[2], float sco2[2])
{
	//PSculptBrushData *brush = data->brush;
	short locks = pchan->protectflag;
	float dvec[3] = {0.0f}; /* bone vector */
	
	/* only affect head if it's not locked */
	if ((pchan->parent == NULL) || !(pchan->bone->flag & BONE_CONNECTED)) 
	{
		float cvec[3];
		float dist, fac;
		
		/* calculate strength of action */
		dist = len_v2v2(sco1, data->mval);
		
		fac = (float)pow((double)(1.0f - dist / data->rad), (double)data->fac);
		if (fac > 0.0f) {
			//printf("pchan-H %s (%f/%f) %f\n", pchan->name, dist, data->dist, fac);
			
			/* apply this to bone */
			mul_v3_v3fl(cvec, data->dvec, fac);
			
			if (locks & OB_LOCK_LOCX) cvec[0] = 0.0f;
			if (locks & OB_LOCK_LOCY) cvec[1] = 0.0f;
			if (locks & OB_LOCK_LOCZ) cvec[2] = 0.0f;
			
			add_v3_v3(pchan->pose_head, cvec);
		}
		else {
			//printf("pchan-H %s ignored\n", pchan->name);
		}
	}
	
	/* affect tail */
	{
		float cvec[3];
		float dist, fac;
		float len;
		
		/* get delta vector pointing from head to tail (i.e. bone) */
		sub_v3_v3v3(dvec, pchan->pose_tail, pchan->pose_head);
		len = len_v3(dvec);
		
		/* calculate strength of action */
		dist = len_v2v2(sco2, data->mval);
		
		fac = (float)pow((double)(1.0f - dist / data->rad), (double)data->fac);
		if (fac > 0.0f) {
			//printf("pchan-T %s (%f/%f) %f\n\n", pchan->name, dist, data->dist, fac);
			
			/* apply brush effect to this vector */
			mul_v3_v3fl(cvec, data->dvec, fac);
			add_v3_v3(dvec, cvec);
			
			/* rescale to keep it the same length */
			normalize_v3(dvec);
			mul_v3_fl(dvec, len);
			
			/* set new pose tail */
			// XXX: doesn't this end up doubling up what came before?
			add_v3_v3v3(pchan->pose_tail, pchan->pose_head, dvec);
		}
		else {
			//printf("pchan-T %s ignored\n", pchan->name);
		}
	}
	
	/* convert joints to low-level transforms */
	apply_pchan_joints(pchan, dvec);
}

/* "smooth" brush */
static void brush_smooth(tPoseSculptingOp *pso, tPSculptContext *data, bPoseChannel *pchan, float sco1[2], float sco2[2])
{
	
}

/* "grab" brush */
static void brush_grab(tPoseSculptingOp *pso, tPSculptContext *data, bPoseChannel *pchan, float UNUSED(sco1[2]), float UNUSED(sco2[2]))
{
	PSculptBrushData *brush = data->brush;
	float imat[4][4], mat[4][4];
	float cvec[3];
	float fac;
	
	/* strength of push */
	fac = (float)pow((double)(1.0f - data->dist / data->rad), (double)data->fac);
	if (data->invert) fac = -fac;
	
	if (brush->flag & PSCULPT_BRUSH_FLAG_GRAB_INITIAL) {
		tAffectedBone *tab = verify_bone_is_affected(pso, data, pchan, data->first);
		
		/* if one couldn't be found or added, then it didn't exist the first time round,
		 * so we shouldn't proceed (to avoid clobbering additional bones)
		 */
		if (tab == NULL) {
			return;
		}
		else if (data->first) {
			/* store factor for later */
			tab->fac = fac;
		}
		else if (1 /* brush->flag & PSCULPT_BRUSH_FLAG_NO_FALLOFF */) {
			/* don't use falloff - better for chains */
			fac = 1.0f;
		}
		else {
			/* reuse initial factor */
			fac = tab->fac;
		}
	}
	
	/* compute inverse matrix to convert from screen-space to bone space */
	//mult_m4_m4m4(mat, data->ob->obmat, pchan->bone->arm_mat); original function, pre math cleanup
	mul_m4_m4m4(mat, data->ob->obmat, pchan->bone->arm_mat);
	invert_m4_m4(imat, mat);
	
	/* apply deforms to bone locations only based on amount mouse moves */
	copy_v3_v3(cvec, data->dvec);
	mul_mat3_m4_v3(imat, cvec);
	mul_v3_fl(cvec, fac);
	
	/* knock out invalid transforms */
	if ((pchan->parent) && (pchan->bone->flag & BONE_CONNECTED))
		return;
		
	if (pchan->protectflag & OB_LOCK_LOCX)
		cvec[0] = 0.0f;
	if (pchan->protectflag & OB_LOCK_LOCY)
		cvec[1] = 0.0f;
	if (pchan->protectflag & OB_LOCK_LOCZ)
		cvec[2] = 0.0f;
	
	/* apply to bone */
	add_v3_v3(pchan->loc, cvec);
}

/* "curl" brush */
static void brush_curl(tPoseSculptingOp *pso, tPSculptContext *data, bPoseChannel *pchan, float UNUSED(sco1[2]), float UNUSED(sco2[2]))
{
	PSculptBrushData *brush = data->brush;
	short locks = pchan->protectflag;
	float eul[3] = {0.0f};
	float angle = 0.0f;
	
	/* get temp euler tuple to work on*/
	if (get_pchan_eul_rotation(eul, NULL, pchan) == false)
		return;
	
	/* amount to rotate depends on the strength of the brush 
	 * - 10.0f factor is used to get values of ~x.y degrees vs 0.xy degrees
	 * - rotations are internally represented using radians, which are very sensitive
	 */
	angle = fabsf(1.0f - data->dist / data->rad) * data->fac * 10.0f;	//printf("%f ", angle);
	angle = DEG2RAD(angle);                                             //printf("%f \n", angle);
	
	if (data->invert) angle = -angle;
	
	/* rotate on x/z axes, whichever isn't locked */
	if (ELEM(brush->xzMode, PSCULPT_BRUSH_DO_XZ, PSCULPT_BRUSH_DO_X) && 
		(locks & OB_LOCK_ROTX)==0)
	{
		/* apply to x axis */
		eul[0] += angle;
	}
	
	if (ELEM(brush->xzMode, PSCULPT_BRUSH_DO_XZ, PSCULPT_BRUSH_DO_Z) && 
		(locks & OB_LOCK_ROTZ)==0)
	{
		/* apply to z axis */
		eul[2] += angle;
	}
	
	/* flush values */
	set_pchan_eul_rotation(eul, pchan);
}

/* "twist" brush */
static void brush_twist(tPoseSculptingOp *pso, tPSculptContext *data, bPoseChannel *pchan, float UNUSED(sco1[2]), float UNUSED(sco2[2]))
{
	PSculptBrushData *brush = data->brush;
	short locks = pchan->protectflag;
	float eul[3] = {0.0f};
	float angle = 0.0f;
	
	/* get temp euler tuple to work on*/
	if (get_pchan_eul_rotation(eul, NULL, pchan) == false)
		return;
	
	/* amount to rotate depends on the strength of the brush 
	 * - 10.0f factor is used to get values of ~x.y degrees vs 0.xy degrees
	 * - rotations are internally represented using radians, which are very sensitive
	 */
	angle = fabsf(1.0f - data->dist / data->rad) * data->fac * 10.0f;	//printf("%f ", angle);
	angle = DEG2RAD(angle);                                             //printf("%f \n", angle);
	
	if (data->invert) angle = -angle;
	
	/* just rotate on y, unless locked */
	if ((locks & OB_LOCK_ROTY) == 0) {
		eul[1] += angle;
	}
	
	/* flush values */
	set_pchan_eul_rotation(eul, pchan);
}

/* "stretch" brush */
static void brush_stretch(tPoseSculptingOp *pso, tPSculptContext *data, bPoseChannel *pchan, float UNUSED(sco1[2]), float UNUSED(sco2[2]))
{
	PSculptBrushData *brush = data->brush;
	const float DAMP_FAC = 0.1f; /* damping factor - to be configurable? */
	float fac;
	
	/* scale factor must be greater than 1 for add, and less for subtract */
	fac = fabsf(1.0f - data->dist / data->rad) * data->fac * DAMP_FAC;
	
	if (data->invert)
		fac = 1.0f - fac;
	else
		fac = 1.0f + fac;
	
	/* perform scaling on y-axis - that's what "stretching" is! */
	pchan->size[1] *= fac;
	
	/* scale on x/z axes, whichever isn't locked */
	// TODO: investigate volume preserving stuff?
	if (ELEM(brush->xzMode, PSCULPT_BRUSH_DO_XZ, PSCULPT_BRUSH_DO_X) && 
		(pchan->protectflag & OB_LOCK_SCALEX) == 0)
	{
		/* apply to x axis */
		pchan->size[0] *= fac;
	}
	
	if (ELEM(brush->xzMode, PSCULPT_BRUSH_DO_XZ, PSCULPT_BRUSH_DO_Z) && 
		(pchan->protectflag & OB_LOCK_SCALEZ) == 0)
	{
		/* apply to z axis */
		pchan->size[2] *= fac;
	}
}

/* clear transforms */
static void brush_reset(tPoseSculptingOp *UNUSED(pso), tPSculptContext *UNUSED(data), bPoseChannel *pchan, float UNUSED(sco1[2]), float UNUSED(sco2[2]))
{
	short locks = pchan->protectflag;
	float eul[3] = {0.0f};
	
	/* location locks */
	if ((locks & OB_LOCK_LOCX) == 0)
		pchan->loc[0] = 0.0f;
	if ((locks & OB_LOCK_LOCY) == 0)
		pchan->loc[1] = 0.0f;
	if ((locks & OB_LOCK_LOCZ) == 0)
		pchan->loc[2] = 0.0f;
		
	/* rotation locks */
	if (get_pchan_eul_rotation(eul, NULL, pchan)) {
		if ((locks & OB_LOCK_ROTX) == 0)
			eul[0] = 0.0f;
		if ((locks & OB_LOCK_ROTY) == 0)
			eul[1] = 0.0f;
		if ((locks & OB_LOCK_ROTZ) == 0)
			eul[2] = 0.0f;
			
		set_pchan_eul_rotation(eul, pchan);
	}
	
	/* scaling locks */
	if ((locks & OB_LOCK_SCALEX) == 0)
		pchan->size[0] = 1.0f;
	if ((locks & OB_LOCK_SCALEY) == 0)
		pchan->size[1] = 1.0f;
	if ((locks & OB_LOCK_SCALEZ) == 0)
		pchan->size[2] = 1.0f;
}

/* "radial" brush */
static void brush_radial(tPoseSculptingOp *pso, tPSculptContext *data, bPoseChannel *pchan, float UNUSED(sco1[2]), float UNUSED(sco2[2]))
{
	
}

/* "wrap" brush */
static void brush_wrap(tPoseSculptingOp *pso, tPSculptContext *data, bPoseChannel *pchan, float UNUSED(sco1[2]), float UNUSED(sco2[2]))
{
	
}

/* ******************************************************** */
/* Pose Sculpt - Painting Operator */

/* Init/Exit ----------------------------------------------- */

static int psculpt_brush_init(bContext *C, wmOperator *op)
{
	Scene *scene = CTX_data_scene(C);
	Object *ob = CTX_data_active_object(C);
	PSculptSettings *pset = psculpt_settings(scene);
	tPoseSculptingOp *pso;
	tPSculptContext *data;
	PSculptBrushData *brush;
	
	/* setup operator data */
	pso = MEM_callocN(sizeof(tPoseSculptingOp), "tPoseSculptingOp");
	op->customdata = pso;
	
	pso->first = true;
	
	pso->scene = scene;
	pso->ob = ob;
	
	pso->affected_bones = BLI_ghash_new(BLI_ghashutil_ptrhash, BLI_ghashutil_ptrcmp, "psculpt affected bones gh");
	
	/* ensure that object's inverse matrix is set and valid */
	// XXX: this should generally be valid...
	invert_m4_m4(ob->imat, ob->obmat);
	
	/* setup callback data */
	data = &pso->data;
	psculpt_init_view3d_data(C, data);
	
	brush = data->brush;
	data->invert = (brush && (brush->flag & PSCULPT_BRUSH_FLAG_INV)) || 
	                (RNA_boolean_get(op->ptr, "invert"));
				  
	data->first = true;
	
	/* setup cursor and header drawing */
	ED_area_headerprint(CTX_wm_area(C), IFACE_("Pose Sculpting in progress..."));
	
	WM_cursor_modal_set(CTX_wm_window(C), BC_CROSSCURSOR);
	psculpt_toggle_cursor(C, true);
	
	return true;
}

static void psculpt_brush_exit(bContext *C, wmOperator *op)
{
	tPoseSculptingOp *pso = op->customdata;
	wmWindow *win = CTX_wm_window(C);
	
	/* unregister timer (only used for realtime) */
	if (pso->timer) {
		WM_event_remove_timer(CTX_wm_manager(C), win, pso->timer);
	}
	
	/* clear affected bones hash - second arg is provided to free allocated data */
	BLI_ghash_free(pso->affected_bones, NULL, free_affected_bone);
	
	/* disable cursor and headerprints */
	ED_area_headerprint(CTX_wm_area(C), NULL);
	
	WM_cursor_modal_restore(win);
	psculpt_toggle_cursor(C, false);
	
	/* free operator data */
	MEM_freeN(pso);
	op->customdata = NULL;
}

/* Apply ----------------------------------------------- */

/* Apply brush callback on bones which fall within the brush region 
 * Based on method pose_circle_select() in view3d_select.c
 */
static short psculpt_brush_do_apply(tPoseSculptingOp *pso, tPSculptContext *data, PSculptBrushCallback brush_cb, short selected)
{
	PSculptSettings *pset = psculpt_settings(pso->scene);
	ViewContext *vc = &data->vc;
	bArmature *arm = data->ob->data;
	bPose *pose = data->ob->pose;
	bPoseChannel *pchan;
	bool changed = false;
	
	ED_view3d_init_mats_rv3d(vc->obact, vc->rv3d); /* for foreach's screen/vert projection */
	
	/* check each PoseChannel... */
	// TODO: could be optimised at some point
	for (pchan = pose->chanbase.first; pchan; pchan = pchan->next) {
		eV3DProjStatus ps1, ps2;
		float sco1[2], sco2[2];
		float vec[3];
		bool ok = false;
		
		/* skip invisible bones */
		if (PBONE_VISIBLE(arm, pchan->bone) == 0)
			continue;
			
		/* only affect selected bones? */
		if ((pset->flag & PSCULPT_FLAG_SELECT_MASK) && 
		    (pset->brushtype != PSCULPT_BRUSH_SELECT)) 
		{
			if ((pchan->bone) && !(pchan->bone->flag & BONE_SELECTED))
				continue;
		}
		
		/* project head location to screenspace */
		mul_v3_m4v3(vec, vc->obact->obmat, pchan->pose_head);
		ps1 = ED_view3d_project_float_global(vc->ar, vec, sco1, V3D_PROJ_TEST_CLIP_BB | V3D_PROJ_TEST_CLIP_WIN);
		
		/* project tail location to screenspace */
		mul_v3_m4v3(vec, vc->obact->obmat, pchan->pose_tail);
		ps2 = ED_view3d_project_float_global(vc->ar, vec, sco2, V3D_PROJ_TEST_CLIP_BB | V3D_PROJ_TEST_CLIP_WIN);
		
		/* outright skip any joints which occur off-screen
		 * NOTE: edge_inside_circle doesn't check for these cases, and ends up
		 * making mirror-bones partially out of view getting activated
		 */
		if ((ps1 != V3D_PROJ_RET_OK) || (ps2 != V3D_PROJ_RET_OK)) {
			continue;
		}
		
		/* check if the head and/or tail is in the circle 
		 *	- the call to check also does the selection already
		 */
		// FIXME: this method FAILS on custom bones shapes. Can be quite bad sometimes with production rigs!
		if (edge_inside_circle(data->mval, data->rad, sco1, sco2)) {
			ok = true;
		}
		/* alternatively, check if this is already in the cache for a brush that just wants to affect those initially captured */
		else if ((data->brush->flag & PSCULPT_BRUSH_FLAG_GRAB_INITIAL) && 
				 (data->first == false) && 
				 (verify_bone_is_affected(pso, data, pchan, false) != NULL))
		{
			ok = true;
		}
		
		/* act on bone? */
		if (ok) {
			float mid[2];
			
			/* set distance from cursor to bone - taken as midpoint of bone */
			mid_v2_v2v2(mid, sco1, sco2);
			data->dist = len_v2v2(mid, data->mval);
			
			/* apply callback to this bone */
			brush_cb(pso, data, pchan, sco1, sco2);
			
			/* tag as changed */
			// TODO: add to autokeying cache...
			changed |= true;
		}
	}
	
	return changed;
}

/* Calculate settings for applying brush */
static void psculpt_brush_apply(bContext *C, wmOperator *op, PointerRNA *itemptr)
{
	tPoseSculptingOp *pso = op->customdata;
	
	Scene *scene = pso->scene;
	Object *ob = pso->ob;
	
	int mouse[2];
	float mousef[2];
	float dx, dy;
	short selected = 0;
	
	/* get latest mouse coordinates */
	RNA_float_get_array(itemptr, "mouse", mousef);
	mouse[0] = mousef[0];
	mouse[1] = mousef[1];
	
	if (RNA_boolean_get(itemptr, "pen_flip"))
		pso->data.invert = true;
	
	/* store coordinates as reference, if operator just started running */
	if (pso->first) {
		pso->lastmouse[0] = mouse[0];
		pso->lastmouse[1] = mouse[1];
	}
	
	/* get distance moved */
	dx = mouse[0] - pso->lastmouse[0];
	dy = mouse[1] - pso->lastmouse[1];
	
	/* only apply brush if mouse moved, or if this is the first run, or if the timer ticked */
	if (((dx != 0.0f) || (dy != 0.0f)) || (pso->first) || (pso->timerTick)) 
	{
		PSculptSettings *pset = psculpt_settings(scene);
		PSculptBrushData *brush = psculpt_get_brush(scene);
		ARegion *ar = CTX_wm_region(C);
		
		View3D *v3d = CTX_wm_view3d(C);
		RegionView3D *rv3d = CTX_wm_region_view3d(C);
		float *rvec, zfac;
		
		int tot_steps = 1, step = 1;
		float dmax;
		
		/* init view3D depth buffer stuff, used for finding bones to affect */
		view3d_operator_needs_opengl(C);
		view3d_set_viewcontext(C, &pso->data.vc);
		
		rvec = ED_view3d_cursor3d_get(scene, v3d);
		zfac = ED_view3d_calc_zfac(rv3d, rvec, NULL);
		
		/* Calculate the distance each "step" (i.e. each sub-point on the linear path
		 * between the distance travelled by the brush since the last evaluation step)
		 * takes. Substeps are used to ensure a more consistent application along the
		 * path taken by the brush.
		 */
		dmax = max_ff(fabs(dx), fabs(dy));
		tot_steps = dmax / (0.2f * brush->size) + 1;
		
		dx /= (float)tot_steps;
		dy /= (float)tot_steps;
		
		/* precompute object dependencies */
		invert_m4_m4(ob->imat, ob->obmat);
		
		/* apply the brush for each brush step */
		for (step = 1; step <= tot_steps; step++) {
			tPSculptContext data = pso->data;
			bool changed = false;
			float mval[2];
			
			/* get mouse coordinates of step point */
			mval[0] = pso->lastmouse[0] + step*dx;
			mval[1] = pso->lastmouse[1] + step*dy;
			
			/* set generic mouse parameters */
			data.mval = mval;
			data.rad = (float)brush->size;
			data.fac = brush->strength;
			data.first = pso->first;
			
			/* apply brushes */
			switch (pset->brushtype) {
				case PSCULPT_BRUSH_DRAW:
				{
					float mval_f[2], vec[3];
					
					/* based on particle comb brush */
					data.fac = (brush->strength - 0.5f) * 2.0f;
					if (data.fac < 0.0f)
						data.fac = 1.0f - 9.0f * data.fac;
					else
						data.fac = 1.0f - data.fac;
					
					/* calculate mouse movement in 3D space... */
					if (data.invert) {
						mval_f[0] = -dx;
						mval_f[1] = -dy;
					}
					else {
						mval_f[0] = dx;
						mval_f[1] = dy;
					}
					ED_view3d_win_to_delta(ar, mval_f, vec, zfac); /* screen (2D) -> world (3D) */
					mul_mat3_m4_v3(ob->imat, vec);                 /* world  (3D) -> pose (3D) = pchan endpoints space */
					
					data.dvec = vec;
					
					//printf("comb: (%f %f) -> (%.3f %.3f %.3f) @ %f\n", mval_f[0], mval_f[1], vec[0], vec[1], vec[2], data.fac);
					
					/* apply brush to bones */
					changed = psculpt_brush_do_apply(pso, &data, brush_comb, selected);
				}
					break;
					
				case PSCULPT_BRUSH_SMOOTH:
				{
				
				}
					break;
					
				case PSCULPT_BRUSH_GRAB:
				{
					float mval_f[2], vec[2];
					
					/* based on particle comb brush */
					data.fac = (brush->strength - 0.5f) * 2.0f;
					if (data.fac < 0.0f)
						data.fac = 1.0f - 9.0f * data.fac;
					else
						data.fac = 1.0f - data.fac;
					
					mval_f[0] = dx;
					mval_f[1] = dy;
					ED_view3d_win_to_delta(ar, mval_f, vec, zfac);
					data.dvec = vec;
					
					changed = psculpt_brush_do_apply(pso, &data, brush_grab, selected);
				}
					break;
					
				case PSCULPT_BRUSH_CURL:
				{
					changed = psculpt_brush_do_apply(pso, &data, brush_curl, selected);
				}
					break;
					
				case PSCULPT_BRUSH_STRETCH:
				{
					changed = psculpt_brush_do_apply(pso, &data, brush_stretch, selected);
				}
					break;
					
				case PSCULPT_BRUSH_TWIST:
				{
					changed = psculpt_brush_do_apply(pso, &data, brush_twist, selected);
				}
					break;
					
				case PSCULPT_BRUSH_RADIAL:
				{
				
				}
					break;
					
				case PSCULPT_BRUSH_WRAP:
				{
				
				}
					break;
					
				case PSCULPT_BRUSH_RESET:
				{
					changed = psculpt_brush_do_apply(pso, &data, brush_reset, selected);
				}
					break;
					
				case PSCULPT_BRUSH_SELECT:
				{
					bArmature *arm = (bArmature *)ob->data;
					bool sel_changed = false;
					
					/* no need for recalc, unless some visualisation tools depend on this 
					 * (i.e. mask modifier in 'armature' mode) 
					 */
					sel_changed = psculpt_brush_do_apply(pso, &data, brush_select_bone, selected);
					changed = ((sel_changed) && (arm->flag & ARM_HAS_VIZ_DEPS));
				}
					break;
					
				default:
					printf("Pose Sculpt: Unknown brush type %d\n", pset->brushtype);
			}
			
			/* flush updates */
			if (changed) {
				bArmature *arm = (bArmature *)ob->data;
				
				/* old optimize trick... this enforces to bypass the depgraph 
				 *	- note: code copied from transform_generics.c -> recalcData()
				 */
				// FIXME: shouldn't this use the builtin stuff?
				if ((arm->flag & ARM_DELAYDEFORM) == 0)
					DAG_id_tag_update(&ob->id, OB_RECALC_DATA);  /* sets recalc flags */
				else
					BKE_pose_where_is(scene, ob);
			}
		}
		
		/* cleanup and send updates */
		WM_event_add_notifier(C, NC_OBJECT | ND_POSE | NA_EDITED, ob);
		
		pso->lastmouse[0] = mouse[0];
		pso->lastmouse[1] = mouse[1];
		pso->first = false;
	}
}

/* Running --------------------------------------------- */

/* helper - a record stroke, and apply paint event */
static void psculpt_brush_apply_event(bContext *C, wmOperator *op, const wmEvent *event)
{
	PointerRNA itemptr;
	float mouse[2];
	
	VECCOPY2D(mouse, event->mval);
	
	/* fill in stroke */
	RNA_collection_add(op->ptr, "stroke", &itemptr);
	RNA_float_set_array(&itemptr, "mouse", mouse);
	
	// XXX: tablet data...
	RNA_boolean_set(&itemptr, "pen_flip", event->shift != false); // XXX hardcoded
	
	/* apply */
	psculpt_brush_apply(C, op, &itemptr);
}

/* reapply */
static int psculpt_brush_exec(bContext *C, wmOperator *op)
{
	if (!psculpt_brush_init(C, op))
		return OPERATOR_CANCELLED;
	
	RNA_BEGIN(op->ptr, itemptr, "stroke") 
	{
		psculpt_brush_apply(C, op, &itemptr);
	}
	RNA_END;
	
	psculpt_brush_exit(C, op);
	
	return OPERATOR_FINISHED;
}


/* start modal painting */
static int psculpt_brush_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{	
	Scene *scene = CTX_data_scene(C);
	
	PSculptSettings *pset = psculpt_settings(scene);
	tPoseSculptingOp *pso = NULL;
	
	/* init painting data */
	if (!psculpt_brush_init(C, op))
		return OPERATOR_CANCELLED;
	
	pso = op->customdata;
	
	/* do initial "click" apply */
	psculpt_brush_apply_event(C, op, event);
	
	/* register timer for increasing influence by hovering over an area */
	if (ELEM(pset->brushtype, PSCULPT_BRUSH_CURL, PSCULPT_BRUSH_STRETCH))
	{
		PSculptBrushData *brush = psculpt_get_brush(scene);
		pso->timer = WM_event_add_timer(CTX_wm_manager(C), CTX_wm_window(C), TIMER, brush->rate);
	}
	
	/* register modal handler */
	WM_event_add_modal_handler(C, op);
	
	return OPERATOR_RUNNING_MODAL;
}

/* painting - handle events */
static int psculpt_brush_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
	tPoseSculptingOp *pso = op->customdata;
	
	switch (event->type) {
		/* mouse release or some other mbut click = abort! */
		case LEFTMOUSE:
		case MIDDLEMOUSE:
		case RIGHTMOUSE:
			psculpt_brush_exit(C, op);
			return OPERATOR_FINISHED;
			
		/* timer tick - only if this was our own timer */
		case TIMER:
			if (event->customdata == pso->timer) {
				pso->timerTick = true;
				psculpt_brush_apply_event(C, op, event);
				pso->timerTick = false;
			}
			break;
			
		/* mouse move = apply somewhere else */
		case MOUSEMOVE:
		case INBETWEEN_MOUSEMOVE:
			psculpt_brush_apply_event(C, op, event);
			break;
	}
	
	return OPERATOR_RUNNING_MODAL;
}

/* Operator --------------------------------------------- */

void POSE_OT_brush_paint(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Pose Sculpt";
	ot->idname = "POSE_OT_brush_paint";
	ot->description = "Pose sculpting paint brush";
	
	/* api callbacks */
	ot->exec = psculpt_brush_exec;
	ot->invoke = psculpt_brush_invoke;
	ot->modal = psculpt_brush_modal;
	ot->cancel = psculpt_brush_exit;
	ot->poll = psculpt_poll_view3d;

	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

	/* properties */
	RNA_def_collection_runtime(ot->srna, "stroke", &RNA_OperatorStrokeElement, "Stroke", "");
	RNA_def_boolean(ot->srna, "invert", 0, "Invert Brush Action", "Override brush direction to apply inverse operation");
}

/* ******************************************************** */