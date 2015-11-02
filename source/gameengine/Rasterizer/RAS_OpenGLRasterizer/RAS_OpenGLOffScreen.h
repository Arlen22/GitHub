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
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Benoit Bolsee
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include "RAS_IOffScreen.h"
#include "GPU_extensions.h"

class RAS_ICanvas;

class RAS_OpenGLOffScreen : public RAS_IOffScreen
{
	GPUOffScreen *m_ofs;
	RAS_ICanvas *m_canvas;
	unsigned int m_blitfbo;
	unsigned int m_blittex;
	bool m_bound;

public:
	RAS_OpenGLOffScreen(RAS_ICanvas *canvas);
	~RAS_OpenGLOffScreen();

	bool Create(int width, int height, int samples);
	void Destroy();
	void Bind();
	void Blit();
	void Unbind();
};