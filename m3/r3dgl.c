/*
 * Sega Model 3 Emulator
 * Copyright (C) 2003 Bart Trzynadlowski, Ville Linde, Stefano Teso
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License Version 2 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program (license.txt); if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * r3dgl.c
 *
 * An OS-independent OpenGL-based Real3D rendering engine.
 *
 * Address Notes:
 *
 *      Addresses seem to take the form: CCAAAAAA, where A is the address and
 *      C is a command. The lower 1 or 2 bits of CC is probably part of the
 *      address as well. Addresses are word-granular.
 *
 *      0x04800600  -- Draw the list at 0x800600 (8E culling RAM.)
 *      0x040301EA  -- Draw the list at 0x0301EA (8C culling RAM.)
 *      0x018AA963  -- Draw the model at 0x18AA963 (VROM.)
 *      0x01010000  -- Draw the model at 0x1010000 (polygon RAM.)
 *
 *      Address 0x0000000 = Culling RAM @ 0x8C000000
 *              0x0800000 = Culling RAM @ 0x8E000000
 *              0x1000000 = Polygon RAM @ 0x98000000
 *              0x1800000 = VROM
 *
 *      If we assume that 0x8C000000 is the base of the Real3D memory space as
 *      the PowerPC sees it, then 0x0800000*4+0x8C000000=0x8E000000. However,
 *      0x1000000*4+0x8C000000 != 0x98000000. Also, 0x88000000 on the PowerPC
 *      side is definitely something to do with the Real3D. It seems that
 *      the addresses in culling RAM indicate that the Real3D has an internal
 *      address space of its own.
 */

#include "model3.h"

/******************************************************************/
/* Macros                                                         */
/******************************************************************/

#define R3D_LOG			LOG

#define GETWORDLE(a)    (*(UINT32 *) a)
#define GETWORDBE(a)    BSWAP32(*(UINT32 *) a)


/******************************************************************/
/* Privates                                                       */
/******************************************************************/

/*
 * Memory Regions
 */

static UINT8    *culling_ram_8e;    // pointer to Real3D culling RAM
static UINT8    *culling_ram_8c;    // pointer to Real3D culling RAM
static UINT8    *polygon_ram;       // pointer to Real3D polygon RAM
static UINT8    *vrom;              // pointer to VROM

/*
 * Texture Mapping
 *
 * The smallest Model 3 textures are 32x32 and the total VRAM texture sheet
 * is 2048x2048. Dividing this by 32 gives us 64x64. Each element contains an
 * OpenGL ID for a texture. Larger textures take up multiple spaces in the
 * grid.
 */

static GLint    texture_grid[64*64];
static GLbyte   texture_buffer[512*512*4];  // for 1 texture

/*
 * Function Prototypes (for forward references)
 */

static void draw_block(UINT8 *);

/******************************************************************/
/* Model Drawing                                                  */
/******************************************************************/

/*
 * convert_fixed_to_float():
 *
 * Converts from 13.19 fixed-point format to float. Accepts a signed INT32.
 */

static float convert_fixed_to_float(INT32 num)
{
    float   result;

    result = (float) (num >> 19);                   // 13-bit integer
    result += (float) (num & 0x7ffff) / 524288.0f;  // 19-bit fraction

    return result;
}

/*
 * draw_model_be():
 *
 * Draws a complete big endian (VROM) model.
 */

static void draw_model_be(UINT8 *buf)
{
    struct
    {
        GLfloat x, y, z;
        UINT    uv;
    }       v[4], prev_v[4];    
    UINT    i, stop, tex_enable, tex_w, tex_h, u_base, v_base, u_coord,
            v_coord;

	R3D_LOG("model3.log",
			"#\n"
			"# model\n"
			"#\n"
			"\n"
	);

    if (buf == NULL)
        return;

	if(GETWORDBE(&buf[0*4]) == 0)
		return;

    do
    {
        /*
         * Select a texture
         */

        tex_w = 32 << ((GETWORDBE(&buf[3*4]) >> 3) & 7);
        tex_h = 32 << ((GETWORDBE(&buf[3*4]) >> 0) & 7);

        u_base = ((GETWORDBE(&buf[4*4]) & 0x1F) << 1) | ((GETWORDBE(&buf[5*4]) & 0x80) >> 7);
        v_base = (GETWORDBE(&buf[5*4]) & 0x1F) | ((GETWORDBE(&buf[4*4]) & 0x40) >> 1);
        u_base *= 32;
        v_base *= 32;

        /*
         * Draw
         */

        glColor3ub(buf[4*4+0], buf[4*4+1], buf[4*4+2]);
        tex_enable = buf[6*4+0] & 0x04; // texture enable flag

        stop = buf[1*4+3] & 4;  // last poly?
        if (buf[0*4+3] & 0x40)  // quad
        {
            switch (buf[0*4+3] & 0x0F)
            {
            case 0: // 4 verts

                for (i = 0; i < 4; i++)
                {
                    v[i].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 0*4]));
                    v[i].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 1*4]));
                    v[i].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 2*4]));
                    v[i].uv = GETWORDBE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x5C * sizeof(UINT8);    // advance past this polygon

                break;

            case 1: // 3 verts

                v[0] = prev_v[0];
                for (i = 0; i < 3; i++)
                {
                    v[1 + i].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 0*4]));
                    v[1 + i].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 1*4]));
                    v[1 + i].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 2*4]));
                    v[1 + i].uv = GETWORDBE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x4C * sizeof(UINT8);

                break;

            case 2: // 3 verts

                v[0] = prev_v[1];
                for (i = 0; i < 3; i++)
                {
                    v[1 + i].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 0*4]));
                    v[1 + i].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 1*4]));
                    v[1 + i].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 2*4]));
                    v[1 + i].uv = GETWORDBE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x4C * sizeof(UINT8);

                break;

            case 4: // 3 verts

                v[0] = prev_v[2];
                for (i = 0; i < 3; i++)
                {
                    v[1 + i].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 0*4]));
                    v[1 + i].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 1*4]));
                    v[1 + i].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 2*4]));
                    v[1 + i].uv = GETWORDBE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x4C * sizeof(UINT8);

                break;

            case 8: // 3 verts

                v[0] = prev_v[3];
                for (i = 0; i < 3; i++)
                {
                    v[1 + i].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 0*4]));
                    v[1 + i].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 1*4]));
                    v[1 + i].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 2*4]));
                    v[1 + i].uv = GETWORDBE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x4C * sizeof(UINT8);

                break;

            case 3: // 2 verts

                v[0] = prev_v[0];
                v[1] = prev_v[1];
                for (i = 0; i < 2; i++)
                {
                    v[2 + i].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 0*4]));
                    v[2 + i].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 1*4]));
                    v[2 + i].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 2*4]));
                    v[2 + i].uv = GETWORDBE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x3C * sizeof(UINT8);

                break;

            case 5: // 2 verts

                v[0] = prev_v[0];
                v[1] = prev_v[2];
                for (i = 0; i < 2; i++)
                {
                    v[2 + i].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 0*4]));
                    v[2 + i].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 1*4]));
                    v[2 + i].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 2*4]));
                    v[2 + i].uv = GETWORDBE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x3C * sizeof(UINT8);

                break;

            case 6: // 2 verts

                v[0] = prev_v[1];
                v[1] = prev_v[2];
                for (i = 0; i < 2; i++)
                {
                    v[2 + i].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 0*4]));
                    v[2 + i].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 1*4]));
                    v[2 + i].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 2*4]));
                    v[2 + i].uv = GETWORDBE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x3C * sizeof(UINT8);

                break;

            case 9: // 2 verts

                v[0] = prev_v[0];
                v[1] = prev_v[3];
                for (i = 0; i < 2; i++)
                {
                    v[2 + i].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 0*4]));
                    v[2 + i].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 1*4]));
                    v[2 + i].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 2*4]));
                    v[2 + i].uv = GETWORDBE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x3C * sizeof(UINT8);

                break;

            case 0xC:   // 2 verts

                v[0] = prev_v[2];
                v[1] = prev_v[3];
                for (i = 0; i < 2; i++)
                {
                    v[2 + i].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 0*4]));
                    v[2 + i].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 1*4]));
                    v[2 + i].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 2*4]));
                    v[2 + i].uv = GETWORDBE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x3C * sizeof(UINT8);

                break;
            }

            /*
             * Draw the quad w/ OpenGL
             */

            for (i = 0; i < 4; i++) // save all of these vertices
                prev_v[i] = v[i];

            if (!tex_enable)
                glDisable(GL_TEXTURE_2D);

			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            glBindTexture(GL_TEXTURE_2D, texture_grid[(v_base / 32) * 64 + (u_base / 32)]);
            glBegin(GL_QUADS);
            for (i = 0; i < 4; i++)
            {
                u_coord = v[i].uv >> 16;
                v_coord = v[i].uv & 0xFFFF;

                u_coord >>= 3;
                v_coord >>= 3;

                glTexCoord2f((GLfloat) u_coord / (GLfloat) tex_w, (GLfloat) v_coord / (GLfloat) tex_h);
                glVertex3f(v[i].x, v[i].y, v[i].z);
            }
            glEnd();

			glDisable(GL_BLEND);

            if (!tex_enable)
                glEnable(GL_TEXTURE_2D);
        }
        else                // triangle
        {
            switch (buf[0*4+3] & 0x0F)
            {
            case 0: // 3 verts

                for (i = 0; i < 3; i++)
                {
                    v[i].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 0*4]));
                    v[i].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 1*4]));
                    v[i].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 2*4]));
                    v[i].uv = GETWORDBE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x4C * sizeof(UINT8);    // advance past this polygon

                break;

            case 1: // 2 verts

                v[0] = prev_v[0];
                for (i = 0; i < 2; i++)
                {
                    v[1 + i].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 0*4]));
                    v[1 + i].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 1*4]));
                    v[1 + i].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 2*4]));
                    v[1 + i].uv = GETWORDBE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x3C * sizeof(UINT8);

                break;

            case 2: // 2 verts

                v[0] = prev_v[1];
                for (i = 0; i < 2; i++)
                {
                    v[1 + i].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 0*4]));
                    v[1 + i].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 1*4]));
                    v[1 + i].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 2*4]));
                    v[1 + i].uv = GETWORDBE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x3C * sizeof(UINT8);

                break;

            case 4: // 2 verts

                v[0] = prev_v[2];
                for (i = 0; i < 2; i++)
                {
                    v[1 + i].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 0*4]));
                    v[1 + i].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 1*4]));
                    v[1 + i].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 2*4]));
                    v[1 + i].uv = GETWORDBE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x3C * sizeof(UINT8);

                break;

            case 8: // 2 verts

                v[0] = prev_v[3];
                for (i = 0; i < 2; i++)
                {
                    v[1 + i].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 0*4]));
                    v[1 + i].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 1*4]));
                    v[1 + i].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + i*16 + 2*4]));
                    v[1 + i].uv = GETWORDBE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x3C * sizeof(UINT8);

                break;

            case 3: // 1 vert

                v[0] = prev_v[0];
                v[1] = prev_v[1];
                v[2].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + 0*16 + 0*4]));
                v[2].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + 0*16 + 1*4]));
                v[2].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + 0*16 + 2*4]));
                v[2].uv = GETWORDBE(&buf[0x1C + 0*16 + 3*4]);

                buf += 0x2C * sizeof(UINT8);

                break;

            case 5: // 1 vert

                v[0] = prev_v[0];
                v[1] = prev_v[2];
                v[2].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + 0*16 + 0*4]));
                v[2].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + 0*16 + 1*4]));
                v[2].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + 0*16 + 2*4]));
                v[2].uv = GETWORDBE(&buf[0x1C + 0*16 + 3*4]);

                buf += 0x2C * sizeof(UINT8);

                break;

            case 6: // 1 vert

                v[0] = prev_v[1];
                v[1] = prev_v[2];
                v[2].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + 0*16 + 0*4]));
                v[2].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + 0*16 + 1*4]));
                v[2].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + 0*16 + 2*4]));
                v[2].uv = GETWORDBE(&buf[0x1C + 0*16 + 3*4]);

                buf += 0x2C * sizeof(UINT8);

                break;

            case 9: // 1 vert

                v[0] = prev_v[0];
                v[1] = prev_v[3];
                v[2].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + 0*16 + 0*4]));
                v[2].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + 0*16 + 1*4]));
                v[2].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + 0*16 + 2*4]));
                v[2].uv = GETWORDBE(&buf[0x1C + 0*16 + 3*4]);

                buf += 0x2C * sizeof(UINT8);

                break;

            case 0xC:   // 1 vert

                v[0] = prev_v[2];
                v[1] = prev_v[3];
                v[2].x = convert_fixed_to_float(GETWORDBE(&buf[0x1C + 0*16 + 0*4]));
                v[2].y = convert_fixed_to_float(GETWORDBE(&buf[0x1C + 0*16 + 1*4]));
                v[2].z = convert_fixed_to_float(GETWORDBE(&buf[0x1C + 0*16 + 2*4]));
                v[2].uv = GETWORDBE(&buf[0x1C + 0*16 + 3*4]);

                buf += 0x2C * sizeof(UINT8);

                break;
            }

            /*
             * Draw the triangle w/ OpenGL
             */

            for (i = 0; i < 3; i++) // save all of these vertices
                prev_v[i] = v[i];

            if (!tex_enable)
                glDisable(GL_TEXTURE_2D);

			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            glBindTexture(GL_TEXTURE_2D, texture_grid[(v_base / 32) * 64 + (u_base / 32)]);
            glBegin(GL_TRIANGLES);
            for (i = 0; i < 3; i++)
            {
                u_coord = v[i].uv >> 16;
                v_coord = v[i].uv & 0xFFFF;

                u_coord >>= 3;
                v_coord >>= 3;

                glTexCoord2f((GLfloat) u_coord / (GLfloat) tex_w, (GLfloat) v_coord / (GLfloat) tex_h);
                glVertex3f(v[i].x, v[i].y, v[i].z);
            }
            glEnd();

			glDisable(GL_BLEND);

            if (!tex_enable)
                glEnable(GL_TEXTURE_2D);
        }
    }
    while (!stop);
}

/*
 * draw_model_le():
 *
 * Draws a complete little endian (polygon RAM) model.
 */

static void draw_model_le(UINT8 *buf)
{
    struct
    {
        GLfloat x, y, z;
        UINT    uv;
    }       v[4], prev_v[4];    
    UINT    i, stop, tex_enable, tex_w, tex_h, u_base, v_base, u_coord,
            v_coord;

	R3D_LOG("model3.log",
			"#\n"
			"# model\n"
			"#\n"
			"\n"
	);

    if (buf == NULL)
        return;

	if(GETWORDLE(&buf[0*4]) == 0)
		return;

    do
    {
        /*
         * Select a texture
         */

        tex_w = 32 << ((GETWORDLE(&buf[3*4]) >> 3) & 7);
        tex_h = 32 << ((GETWORDLE(&buf[3*4]) >> 0) & 7);

        u_base = ((GETWORDLE(&buf[4*4]) & 0x1F) << 1) | ((GETWORDLE(&buf[5*4]) & 0x80) >> 7);
        v_base = (GETWORDLE(&buf[5*4]) & 0x1F) | ((GETWORDLE(&buf[4*4]) & 0x40) >> 1);
        u_base *= 32;
        v_base *= 32;

        /*
         * Draw
         */

        glColor3ub(buf[4*4+3], buf[4*4+2], buf[4*4+1]);
        tex_enable = buf[6*4+3] & 0x04; // texture enable flag

        stop = buf[1*4+0] & 4;  // last poly?
        if (buf[0*4+0] & 0x40)  // quad
        {
            switch (buf[0*4+0] & 0x0F)
            {
            case 0: // 4 verts

                for (i = 0; i < 4; i++)
                {
                    v[i].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 0*4]));
                    v[i].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 1*4]));
                    v[i].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 2*4]));
                    v[i].uv = GETWORDLE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x5C * sizeof(UINT8);    // advance past this polygon

                break;

            case 1: // 3 verts

                v[0] = prev_v[0];
                for (i = 0; i < 3; i++)
                {
                    v[1 + i].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 0*4]));
                    v[1 + i].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 1*4]));
                    v[1 + i].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 2*4]));
                    v[1 + i].uv = GETWORDLE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x4C * sizeof(UINT8);

                break;

            case 2: // 3 verts

                v[0] = prev_v[1];
                for (i = 0; i < 3; i++)
                {
                    v[1 + i].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 0*4]));
                    v[1 + i].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 1*4]));
                    v[1 + i].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 2*4]));
                    v[1 + i].uv = GETWORDLE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x4C * sizeof(UINT8);

                break;

            case 4: // 3 verts

                v[0] = prev_v[2];
                for (i = 0; i < 3; i++)
                {
                    v[1 + i].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 0*4]));
                    v[1 + i].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 1*4]));
                    v[1 + i].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 2*4]));
                    v[1 + i].uv = GETWORDLE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x4C * sizeof(UINT8);

                break;

            case 8: // 3 verts

                v[0] = prev_v[3];
                for (i = 0; i < 3; i++)
                {
                    v[1 + i].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 0*4]));
                    v[1 + i].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 1*4]));
                    v[1 + i].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 2*4]));
                    v[1 + i].uv = GETWORDLE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x4C * sizeof(UINT8);

                break;

            case 3: // 2 verts

                v[0] = prev_v[0];
                v[1] = prev_v[1];
                for (i = 0; i < 2; i++)
                {
                    v[2 + i].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 0*4]));
                    v[2 + i].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 1*4]));
                    v[2 + i].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 2*4]));
                    v[2 + i].uv = GETWORDLE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x3C * sizeof(UINT8);

                break;

            case 5: // 2 verts

                v[0] = prev_v[0];
                v[1] = prev_v[2];
                for (i = 0; i < 2; i++)
                {
                    v[2 + i].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 0*4]));
                    v[2 + i].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 1*4]));
                    v[2 + i].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 2*4]));
                    v[2 + i].uv = GETWORDLE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x3C * sizeof(UINT8);

                break;

            case 6: // 2 verts

                v[0] = prev_v[1];
                v[1] = prev_v[2];
                for (i = 0; i < 2; i++)
                {
                    v[2 + i].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 0*4]));
                    v[2 + i].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 1*4]));
                    v[2 + i].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 2*4]));
                    v[2 + i].uv = GETWORDLE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x3C * sizeof(UINT8);

                break;

            case 9: // 2 verts

                v[0] = prev_v[0];
                v[1] = prev_v[3];
                for (i = 0; i < 2; i++)
                {
                    v[2 + i].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 0*4]));
                    v[2 + i].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 1*4]));
                    v[2 + i].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 2*4]));
                    v[2 + i].uv = GETWORDLE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x3C * sizeof(UINT8);

                break;

            case 0xC:   // 2 verts

                v[0] = prev_v[2];
                v[1] = prev_v[3];
                for (i = 0; i < 2; i++)
                {
                    v[2 + i].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 0*4]));
                    v[2 + i].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 1*4]));
                    v[2 + i].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 2*4]));
                    v[2 + i].uv = GETWORDLE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x3C * sizeof(UINT8);

                break;
            }

            /*
             * Draw the quad w/ OpenGL
             */

            for (i = 0; i < 4; i++) // save all of these vertices
                prev_v[i] = v[i];

            if (!tex_enable)
                glDisable(GL_TEXTURE_2D);

			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            glBindTexture(GL_TEXTURE_2D, texture_grid[(v_base / 32) * 64 + (u_base / 32)]);
            glBegin(GL_QUADS);
            for (i = 0; i < 4; i++)
            {
                u_coord = v[i].uv >> 16;
                v_coord = v[i].uv & 0xFFFF;

                u_coord >>= 3;
                v_coord >>= 3;

                glTexCoord2f((GLfloat) u_coord / (GLfloat) tex_w, (GLfloat) v_coord / (GLfloat) tex_h);
                glVertex3f(v[i].x, v[i].y, v[i].z);
            }
            glEnd();

			glDisable(GL_BLEND);

            if (!tex_enable)
                glEnable(GL_TEXTURE_2D);
        }
        else                // triangle
        {
            switch (buf[0*4+0] & 0x0F)
            {
            case 0: // 3 verts

                for (i = 0; i < 3; i++)
                {
                    v[i].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 0*4]));
                    v[i].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 1*4]));
                    v[i].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 2*4]));
                    v[i].uv = GETWORDLE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x4C * sizeof(UINT8);    // advance past this polygon

                break;

            case 1: // 2 verts

                v[0] = prev_v[0];
                for (i = 0; i < 2; i++)
                {
                    v[1 + i].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 0*4]));
                    v[1 + i].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 1*4]));
                    v[1 + i].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 2*4]));
                    v[1 + i].uv = GETWORDLE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x3C * sizeof(UINT8);

                break;

            case 2: // 2 verts

                v[0] = prev_v[1];
                for (i = 0; i < 2; i++)
                {
                    v[1 + i].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 0*4]));
                    v[1 + i].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 1*4]));
                    v[1 + i].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 2*4]));
                    v[1 + i].uv = GETWORDLE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x3C * sizeof(UINT8);

                break;

            case 4: // 2 verts

                v[0] = prev_v[2];
                for (i = 0; i < 2; i++)
                {
                    v[1 + i].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 0*4]));
                    v[1 + i].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 1*4]));
                    v[1 + i].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 2*4]));
                    v[1 + i].uv = GETWORDLE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x3C * sizeof(UINT8);

                break;

            case 8: // 2 verts

                v[0] = prev_v[3];
                for (i = 0; i < 2; i++)
                {
                    v[1 + i].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 0*4]));
                    v[1 + i].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 1*4]));
                    v[1 + i].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + i*16 + 2*4]));
                    v[1 + i].uv = GETWORDLE(&buf[0x1C + i*16 + 3*4]);
                }

                buf += 0x3C * sizeof(UINT8);

                break;

            case 3: // 1 vert

                v[0] = prev_v[0];
                v[1] = prev_v[1];
                v[2].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + 0*16 + 0*4]));
                v[2].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + 0*16 + 1*4]));
                v[2].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + 0*16 + 2*4]));
                v[2].uv = GETWORDLE(&buf[0x1C + 0*16 + 3*4]);

                buf += 0x2C * sizeof(UINT8);

                break;

            case 5: // 1 vert

                v[0] = prev_v[0];
                v[1] = prev_v[2];
                v[2].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + 0*16 + 0*4]));
                v[2].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + 0*16 + 1*4]));
                v[2].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + 0*16 + 2*4]));
                v[2].uv = GETWORDLE(&buf[0x1C + 0*16 + 3*4]);

                buf += 0x2C * sizeof(UINT8);

                break;

            case 6: // 1 vert

                v[0] = prev_v[1];
                v[1] = prev_v[2];
                v[2].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + 0*16 + 0*4]));
                v[2].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + 0*16 + 1*4]));
                v[2].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + 0*16 + 2*4]));
                v[2].uv = GETWORDLE(&buf[0x1C + 0*16 + 3*4]);

                buf += 0x2C * sizeof(UINT8);

                break;

            case 9: // 1 vert

                v[0] = prev_v[0];
                v[1] = prev_v[3];
                v[2].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + 0*16 + 0*4]));
                v[2].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + 0*16 + 1*4]));
                v[2].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + 0*16 + 2*4]));
                v[2].uv = GETWORDLE(&buf[0x1C + 0*16 + 3*4]);

                buf += 0x2C * sizeof(UINT8);

                break;

            case 0xC:   // 1 vert

                v[0] = prev_v[2];
                v[1] = prev_v[3];
                v[2].x = convert_fixed_to_float(GETWORDLE(&buf[0x1C + 0*16 + 0*4]));
                v[2].y = convert_fixed_to_float(GETWORDLE(&buf[0x1C + 0*16 + 1*4]));
                v[2].z = convert_fixed_to_float(GETWORDLE(&buf[0x1C + 0*16 + 2*4]));
                v[2].uv = GETWORDLE(&buf[0x1C + 0*16 + 3*4]);

                buf += 0x2C * sizeof(UINT8);

                break;
            }

            /*
             * Draw the triangle w/ OpenGL
             */

            for (i = 0; i < 3; i++) // save all of these vertices
                prev_v[i] = v[i];

            if (!tex_enable)
                glDisable(GL_TEXTURE_2D);

			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            glBindTexture(GL_TEXTURE_2D, texture_grid[(v_base / 32) * 64 + (u_base / 32)]);
            glBegin(GL_TRIANGLES);
            for (i = 0; i < 3; i++)
            {
                u_coord = v[i].uv >> 16;
                v_coord = v[i].uv & 0xFFFF;

                u_coord >>= 3;
                v_coord >>= 3;

                glTexCoord2f((GLfloat) u_coord / (GLfloat) tex_w, (GLfloat) v_coord / (GLfloat) tex_h);
                glVertex3f(v[i].x, v[i].y, v[i].z);
            }
            glEnd();

			glDisable(GL_BLEND);

            if (!tex_enable)
                glEnable(GL_TEXTURE_2D);
        }
    }
    while (!stop);
}


/******************************************************************/
/* Scene Drawing                                                  */
/******************************************************************/

static UINT32   current_matrix; // number of current matrix to use
static UINT8    *matrix_base;   // pointer to base of matrix table

/*
 * get_float():
 *
 * Fetches a single-precision float.
 */

static float get_float(UINT8 *src)
{
    union
    {
        float   f;
        UINT32  u;
    } f;

    f.u = GETWORDLE(src);

    return f.f;
}

/*
 * translate_r3d_address():
 *
 * Translates an address from the Real3D internal space into a usable pointer.
 */

static UINT8 *translate_r3d_address(UINT32 addr)
{
    if (addr <= 0x007FFFF)                              // 8C culling RAM
        return &culling_ram_8c[addr * 4];
    else if (addr >= 0x0800000 && addr <= 0x083FFFF)    // 8E culling RAM
    {
        addr &= 0x003FFFF;
        return &culling_ram_8e[addr * 4];
    }
    else if (addr >= 0x1000000 && addr <= 0x107FFFF)    // polygon RAM
    {
        addr &= 0x007FFFF;
        return &polygon_ram[addr * 4];
    }
    else if (addr >= 0x1800000 && addr <= 0x1FFFFFF)    // VROM
    {
        addr &= 0x07FFFFF;
        return &vrom[addr * 4];
    }

    return NULL;
    // this one is a kludge to satisfy VON2 -- it probably has no relation to
    // polygon RAM, though
#if 0
    else if (addr >= 0x1200000 && addr <= 0x127FFFF)
    {
        addr &= 0x007FFFF;
        return &polygon_ram[addr * 4];
    }

    error("Unable to translate Real3D address: %08X\n", addr);
    return NULL;
#endif
}

/*
 * set_matrix_base():
 *
 * Sets the base matrix table address.
 */

static void set_matrix_base(UINT32 addr)
{
    matrix_base = translate_r3d_address(addr);
}

/*
 * get_matrix():
 *
 * Fetches a matrix from 8E culling RAM and converts it to OpenGL form.
 */

#define CMINDEX(y, x)	(x*4+y)
static void get_matrix(GLfloat *dest, UINT32 matrix_addr)
{
    UINT8   *src;

    src = &matrix_base[matrix_addr*4];

    dest[CMINDEX(0, 0)] = get_float(&src[3*4]);
    dest[CMINDEX(0, 1)] = get_float(&src[4*4]);
    dest[CMINDEX(0, 2)] = get_float(&src[5*4]);
    dest[CMINDEX(0, 3)] = get_float(&src[0*4]);

    dest[CMINDEX(1, 0)] = get_float(&src[6*4]);
    dest[CMINDEX(1, 1)] = get_float(&src[7*4]);
    dest[CMINDEX(1, 2)] = get_float(&src[8*4]);
    dest[CMINDEX(1, 3)] = get_float(&src[1*4]);

    dest[CMINDEX(2, 0)] = get_float(&src[9*4]);
    dest[CMINDEX(2, 1)] = get_float(&src[10*4]);
    dest[CMINDEX(2, 2)] = get_float(&src[11*4]);
    dest[CMINDEX(2, 3)] = get_float(&src[2*4]);

    dest[CMINDEX(3, 0)] = 0.0;
    dest[CMINDEX(3, 1)] = 0.0;
    dest[CMINDEX(3, 2)] = 0.0;
	dest[CMINDEX(3, 3)] = 1.0;	
}

/*
 * draw_list():
 *
 * Processes a list. Each list element references a 10-word block.
 *
 * On Step 2.0, the endianness of each list element is different from other
 * data. 
 */

static void draw_list(UINT8 *list)
{
    UINT32  addr;

    do
    {
        addr = GETWORDLE(list);

		R3D_LOG("model3.log", " ## list: draw block at %08X\n\n", addr);

        if (addr == 0 || addr == 0x800800)  // safeguard: in case memory has not been uploaded yet
            break;
        if (addr == 0x03000000) // VON2 (what does this mean???)
            break;

        draw_block(translate_r3d_address(addr & 0x01FFFFFF));
        list += 4;  // next element
    }
    while (!(addr & 0x02000000));
}

/*
 * draw_block():
 *
 * Processes a 10-word block. These blocks can either reference models or
 * lists.
 */

static void draw_block(UINT8 *block)
{
    GLfloat m[4*4];
    UINT32  matrix, addr, next_ptr;

    /*
     * Blocks may apparently be chained together in a linked list. We're not
     * yet certain if these link pointers may reference lists or models, but
     * for now, we're assuming they cannot.
     */

    while (1)
    {
        /*
         * Scud Race has weird nodes which are preceeded with pointers. I
         * check the header for bit 0x01000000 to determine if it's a block or
         * a pointer to a block. This is just a hack, really.
         *
		 * Update: these weird 4-word headers are two pairs of pointers,
		 * possibily symmetrical, and can have 0x01000000 either set or clear
		 * in the first word. But at least two of the links must have this
		 * set. These addresses can reference another block (usually the
		 * following) or a model in VROM. Bit 0x00800000 is used to determine
		 * which is the case.
		 */

        if (((GETWORDLE(&block[0*4]) >> 24) == 0x01) ||
		    ((GETWORDLE(&block[1*4]) >> 24) == 0x01) ||
		    ((GETWORDLE(&block[2*4]) >> 24) == 0x01) ||
		    ((GETWORDLE(&block[3*4]) >> 24) == 0x01))
		{
			if(GETWORDLE(&block[0*4]) & 0x00800000)	// a model in VROM
			{
				R3D_LOG("model3.log", " ## block: block/list detected, draw model at %08X\n", GETWORDLE(&block[0*4]));
				draw_model_be(translate_r3d_address(GETWORDLE((&block[0*4]) & 0x00FFFFFF) | 0x01000000));
				return;
			}

			R3D_LOG("model3.log", " ## block: block/list detected, draw block at %08X\n", GETWORDLE(&block[0*4]));
           	block = translate_r3d_address(GETWORDLE(&block[0*4]) & 0x00FFFFFF);
		}

		R3D_LOG("model3.log",
				"#\n"
				"# block:\n"
				"#\n"
				"\n"
				"00: %08X\n"
				"01: %08X\n"
				"02: %08X\n"
				"03: %08X\n"
				"04: %3.5f\n"
				"05: %3.5f\n"
				"06: %3.5f\n"
				"07: %08X\n"
				"08: %08X\n"
				"09: %08X\n"
				"\n",
				GETWORDLE(&block[0 * 4]),
				GETWORDLE(&block[1 * 4]),
				GETWORDLE(&block[2 * 4]),
				GETWORDLE(&block[3 * 4]),
				get_float(&block[4 * 4]),
				get_float(&block[5 * 4]),
				get_float(&block[6 * 4]),
				GETWORDLE(&block[7 * 4]),
				GETWORDLE(&block[8 * 4]),
				GETWORDLE(&block[9 * 4])
		);

        /*
         * Multiply by the specified matrix. If bit 0x20000000 is not set, I
         * presume that no matrix is to be used.
         */

        glPushMatrix();
        matrix = GETWORDLE(&block[3*4]);
        if ((matrix & 0x20000000) && (matrix & 0x3FF))
        {
            current_matrix = matrix & 0x03FF;
            get_matrix(m, (matrix & 0x03FF)*12);
            if ((matrix & 0x3FF) != 0)  // safeguard for Scud Race
                glMultMatrixf(m);
        }
        glTranslatef(get_float(&block[4*4]), get_float(&block[5*4]), get_float(&block[6*4]));
        ++current_matrix;

        /*
         * Draw a model or process a list. If the address is of the form
         * 04XXXXXX, then the address points to a list, otherwise it points to
         * a model.
         */

        addr = GETWORDLE(&block[7*4]);

		if(GETWORDLE(&block[0*4]) & 0x08)
		{
			/*
			 * The block references a 4-element list (Scud Race).
			 * The value of bit 0x01000000 in the address assumes another
			 * (currently unknown) meaning.
			 */

			if(addr & 0xFE000000)
				error("Invalid list address: %08X\n", addr);

			R3D_LOG("model3.log", " ## block: draw block at %08X (exception 1)\n\n", addr);
            draw_block(translate_r3d_address(addr & 0x00FFFFFF));
		}
		else
		{
	        switch ((addr >> 24) & 0xFF)
	        {
			case 0x00:	// block
				if(addr != 0)
				{
					R3D_LOG("model3.log", " ## block: draw block at %08X\n\n", addr);
					draw_block(translate_r3d_address(addr & 0x01FFFFFF));
				}
				break;
			case 0x01:	// model
			case 0x03:	// model in VROM (Scud Race)
				if(addr != 0)
				{
					R3D_LOG("model3.log", " ## block: draw model at %08X\n\n", addr);
					if ((addr & 0x01FFFFFF) >= 0x01800000)  // VROM
						draw_model_be(translate_r3d_address(addr & 0x01FFFFFF));
					else                                    // polygon RAM
						draw_model_le(translate_r3d_address(addr & 0x01FFFFFF));
				}
				break;
	        case 0x04:  // list
				R3D_LOG("model3.log", " ## block: draw list at %08X\n\n", addr);
	            if ((addr & 0x01FFFFFF) >= 0x018000000) error("List in VROM %08X\n", addr);
	            draw_list(translate_r3d_address(addr & 0x01FFFFFF));
	            break;
	        default:
	            error("Unable to handle Real3D address: %08X\n", addr);
	            break;
    	    }
		}

        /*
         * Pop the matrix if we pushed one
         */

//        if ((matrix & 0x20000000))
            glPopMatrix();

        /*
         * Advance to next block in list
         */

        next_ptr = GETWORDLE(&block[8*4]);
        if ((next_ptr & 0x01000000) || (next_ptr == 0)) // no more links
            break;

//        message(0, "next_ptr = %08X", next_ptr);
        block = translate_r3d_address(next_ptr);
    }
}

/*
 * draw_scene():
 *
 * Draws the scene by traversing each of the major nodes starting at 0.
 */

static void draw_scene(void)
{
    UINT32  i, j, next;
    BOOL    stop;

    /*
     * Draw each major node
     */

    i = 0;
    stop = 0;

	LOG_INIT("model3.log");

    do
    {
        message(0, "processing scene descriptor %08X: %08X  %08X  %08X",
        i,
        GETWORDLE(&culling_ram_8e[i + 0]),
        GETWORDLE(&culling_ram_8e[i + 4]),
        GETWORDLE(&culling_ram_8e[i + 8])
        );

		R3D_LOG("model3.log",
				"#\n"
				"# scene at %08X\n"
				"#\n"
				"\n"
				"00: %08X\n"
				"01: %08X\n"
				"02: %08X\n"
				"03: %3.5f\n"
				"04: %3.5f\n"
				"05: %3.5f\n"
				"06: %3.5f\n"
				"07: %3.5f\n"
				"08: %3.5f\n"
				"09: %3.5f\n"
				"0A: %3.5f\n"
				"0B: %3.5f\n"
				"0C: %3.5f\n"
				"0D: %3.5f\n"
				"0E: %3.5f\n"
				"0F: %3.5f\n"
				"10: %3.5f\n"
				"11: %3.5f\n"
				"12: %3.5f\n"
				"13: %3.5f\n"
				"14: %08X\n"
				"15: %08X\n"
				"16: %08X\n"
				"17: %08X\n"
				"18: %08X\n"
				"19: %08X\n"
				"1A: %08X\n"
				"1B: %08X\n"
				"1C: %08X\n"
				"1D: %08X\n"
				"1E: %08X\n"
				"1F: %3.5f\n"
				"20: %3.5f\n"
				"21: %3.5f\n"
				"22: %08X\n"
				"23: %08X\n"
				"24: %08X\n"
				"25: %08X\n"
				"26: %08X\n"
				"27: %08X\n"
				"28: %08X\n"
				"29: %08X\n"
				"2A: %08X\n"
				"2B: %08X\n"
				"2C: %08X\n"
				"2D: %08X\n"
				"2E: %08X\n"
				"2F: %08X\n"
				"\n",
				i,
				GETWORDLE(&culling_ram_8e[i + 0x00 * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x01 * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x02 * 4]),
				get_float(&culling_ram_8e[i + 0x03 * 4]),
				get_float(&culling_ram_8e[i + 0x04 * 4]),
				get_float(&culling_ram_8e[i + 0x05 * 4]),
				get_float(&culling_ram_8e[i + 0x06 * 4]),
				get_float(&culling_ram_8e[i + 0x07 * 4]),
				get_float(&culling_ram_8e[i + 0x08 * 4]),
				get_float(&culling_ram_8e[i + 0x09 * 4]),
				get_float(&culling_ram_8e[i + 0x0A * 4]),
				get_float(&culling_ram_8e[i + 0x0B * 4]),
				get_float(&culling_ram_8e[i + 0x0C * 4]),
				get_float(&culling_ram_8e[i + 0x0D * 4]),
				get_float(&culling_ram_8e[i + 0x0E * 4]),
				get_float(&culling_ram_8e[i + 0x0F * 4]),
				get_float(&culling_ram_8e[i + 0x10 * 4]),
				get_float(&culling_ram_8e[i + 0x11 * 4]),
				get_float(&culling_ram_8e[i + 0x12 * 4]),
				get_float(&culling_ram_8e[i + 0x13 * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x14 * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x15 * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x16 * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x17 * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x18 * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x19 * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x1A * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x1B * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x1C * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x1D * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x1E * 4]),
				get_float(&culling_ram_8e[i + 0x1F * 4]),
				get_float(&culling_ram_8e[i + 0x20 * 4]),
				get_float(&culling_ram_8e[i + 0x21 * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x22 * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x23 * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x24 * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x25 * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x26 * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x27 * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x28 * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x29 * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x2A * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x2B * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x2C * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x2D * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x2E * 4]),
				GETWORDLE(&culling_ram_8e[i + 0x2F * 4])
		);

        set_matrix_base(GETWORDLE(&culling_ram_8e[i + 0x16*4]));

        j = GETWORDLE(&culling_ram_8e[i + 8]);  // get address of 10-word block
        j = (j & 0xFFFF) * 4;
        if (j == 0) // culling RAM probably hasn't been set up yet
            break;

        next = GETWORDLE(&culling_ram_8e[i + 4]);   // get address of next block
        if (next == 0x01000000)                     // 01000000 == STOP
            stop = TRUE;
        i = (next & 0xFFFF) * 4;

		switch((GETWORDLE(&culling_ram_8e[i + 8]) >> 24) & 0xFE)
		{
		case 0x00:	// block
			R3D_LOG("model3.log", " ## scene: draw block at %08X\n\n", j);
			draw_block(&culling_ram_8e[j]);
			break;

		case 0x04:	// list
			R3D_LOG("model3.log", " ## scene: draw list at %08X\n\n", j);
//            draw_list(&culling_ram_8e[j]);
			break;

		default:
			error("Unknown scene descriptor link %08X\n", GETWORDLE(&culling_ram_8e[i + 8]));
		}
    }
    while (!stop);
}

/******************************************************************/
/* Texture Drawing                                                */
/******************************************************************/

static const INT	decode[64] =
{
	 0, 1, 4, 5, 8, 9,12,13,
	 2, 3, 6, 7,10,11,14,15,
	16,17,20,21,24,25,28,29,
	18,19,22,23,26,27,30,31,
	32,33,36,37,40,41,44,45,
	34,35,38,39,42,43,46,47,
	48,49,52,53,56,57,60,61,
	50,51,54,55,58,59,62,63
};

static void draw_texture_tile_8(UINT x, UINT y, UINT8 *buf, UINT w, BOOL little_endian)
{
    UINT    xi, yi, pixel_offs;
    UINT8   lum8;

	for (yi = 0; yi < 8; yi++)
	{
		for (xi = 0; xi < 8; xi++)
		{
            if (little_endian)
            {
                pixel_offs = decode[(yi * 8 + xi) ^ (8|1)]; // (yi ^ 1) * 8 + (xi ^ 1)
                lum8 = buf[pixel_offs];
            }
            else
            {
                pixel_offs = decode[yi * 8 + xi];
                lum8 = buf[pixel_offs + 0];
            }

            texture_buffer[((y + yi) * w + (x + xi)) * 4 + 0] = lum8;
            texture_buffer[((y + yi) * w + (x + xi)) * 4 + 1] = lum8;
            texture_buffer[((y + yi) * w + (x + xi)) * 4 + 2] = lum8;
            texture_buffer[((y + yi) * w + (x + xi)) * 4 + 3] = (UINT8) 0xFF;
        }
    }
}

static void draw_texture_tile_16(UINT x, UINT y, UINT8 *buf, UINT w, BOOL little_endian)
{
    UINT    xi, yi, pixel_offs, rgb16;
    GLbyte  r, g, b, a;

	for (yi = 0; yi < 8; yi++)
	{
		for (xi = 0; xi < 8; xi++)
		{
            /*
             * Grab the pixel offset from the decode[] array and fetch the
             * pixel word
             */

            if (little_endian)
            {
                /*
                 * XOR with 1 in little endian mode -- every word contains 2
                 * 16-bit pixels, thus they are swapped
                 */

                pixel_offs = decode[(yi * 8 + xi) ^ 1] * 2;
                rgb16 = *(UINT16 *) &buf[pixel_offs];
            }
            else
            {
                pixel_offs = decode[yi * 8 + xi] * 2;
                rgb16 = (buf[pixel_offs + 0] << 8) | buf[pixel_offs + 1];
            }

			b = (rgb16 & 0x1f) << 3;
			g = ((rgb16 >> 5) & 0x1f) << 3;
			r = ((rgb16 >> 10) & 0x1f) << 3;
			a = (rgb16 & 0x8000) ? 0x00 : 0xFF;

			/*
			b = (rgb16 & 0xF) << 3;
			g = ((rgb16 >> 4) & 0xF) << 3;
			r = ((rgb16 >> 8) & 0xF) << 3;
			a = (rgb16 & 0xF000) ? 0xFF : 0x00;
			*/

			/*
			 * Write R, G, B, and alpha. On Model 3, an alpha bit of 1
             * indicates a transparent color and 0 is opaque
			 */

            texture_buffer[((y + yi) * w + (x + xi)) * 4 + 0] = r;
            texture_buffer[((y + yi) * w + (x + xi)) * 4 + 1] = g;
            texture_buffer[((y + yi) * w + (x + xi)) * 4 + 2] = b;
            texture_buffer[((y + yi) * w + (x + xi)) * 4 + 3] = a;
        }
    }
}

static void draw_texture_8(UINT w, UINT h, UINT8 *buf, BOOL little_endian)
{
    UINT    xi, yi;

	for (yi = 0; yi < h * 8; yi += 8)
	{
		for (xi = 0; xi < w * 8; xi += 8)
		{
            draw_texture_tile_8(xi, yi, buf, w * 8, little_endian);
            buf += 8 * 8;	// each texture tile is 8x8 and 8-bit color
		}
	}
}

static void draw_texture_16(UINT w, UINT h, UINT8 *buf, BOOL little_endian)
{
    UINT    xi, yi;

	for (yi = 0; yi < h * 8; yi += 8)
	{
		for (xi = 0; xi < w * 8; xi += 8)
		{
            draw_texture_tile_16(xi, yi, buf, w * 8, little_endian);
            buf += 8 * 8 * 2;	// each texture tile is 8x8 and 16-bit color
		}
	}
}

/*
 * void r3dgl_upload_texture(UINT32 header, UINT32 length, UINT8 *src,
 *                           BOOL little_endian);
 *
 * Converts the specified Model 3 texture into OpenGL format and uploads it
 * for use.
 *
 * Parameters:
 *      header        = Header word with size and position info.
 *      length        = Header word containing length information.
 *      src           = Pointer to texture data (no header words.)
 *      little_endian = Set to 1 if little endian, otherwise big endian.
 */

//TODO: remove overwritten textures
//TODO: some textures are 8-bit, this is not handled yet.

void r3dgl_upload_texture(UINT32 header, UINT32 length, UINT8 *src,
                          BOOL little_endian)
{    
    static int  f=0;
    UINT    tiles_x, tiles_y, xpos, ypos, xi, yi;
    GLint   id;

    /*
     * Model 3 texture RAM appears as 2 2048x1024 textures. When textures are
     * uploaded, their size and position within a sheet is given. I treat the
     * texture sheet selection bit as an additional bit to the Y coordinate.
     */

    tiles_x = (header >> 14) & 3;
    tiles_y = (header >> 17) & 3;
    tiles_x = (32 << tiles_x) / 8;  // width in terms of 8x8 tiles
    tiles_y = (32 << tiles_y) / 8;  // height in terms of 8x8 tiles

    ypos = (((header >> 7) & 0x1F) | ((header >> 15) & 0x20)) * 32;
    xpos = ((header >> 0) & 0x3F) * 32;

    /*
     * Render the texture into the texture buffer
     */

    if ((header & 0x0f000000) == 0x02000000)
		return;

	if (header & 0x00800000)
        draw_texture_16(tiles_x, tiles_y, src, little_endian);
	else
		draw_texture_8(tiles_x, tiles_y, src, little_endian);

    /*
     * Get a texture ID for this texture and set its properties, then upload
     * it
     */

    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, tiles_x * 8, tiles_y * 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, texture_buffer);

    /*
     * Mark the appropriate parts of the texture grid with this texture
     */

    for (yi = 0; yi < (tiles_y * 8) / 32; yi++)
    {
        for (xi = 0; xi < (tiles_x * 8) / 32; xi++)
            texture_grid[(yi + ypos / 32) * 64 + (xi + xpos / 32)] = id;
    }
}

/******************************************************************/
/* Interface                                                      */
/******************************************************************/

/*
 * void r3dgl_update_frame(void);
 *
 * Renders the frame.
 *
 * Assumes the following on entry:
 *      - Z-buffering is disabled
 *      - 2D texturing is enabled
 *      - Alpha blending is disabled
 *
 * The entry state is restored on exit.
 */

//TODO: Cannot be resized yet, hardcoded for 496x384

void r3dgl_update_frame(void)
{
//    if (m3_config.step >= 0x20) // we can't render this yet
//        return;

    /*
     * Enable Z-buffering
     */

    glClearDepth(1.0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
//    glDepthFunc(GL_LEQUAL);
    glClear(GL_DEPTH_BUFFER_BIT);

    /*
     * Set up a perspective view and then set the matrix mode to modelview.
     * Each Z coordinate is multiplied by 1 to flip the coordinate system
     * (Model 3 uses a left-hand system, OpenGL is right-handed.)
     */

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
    gluPerspective(45.0, 496.0 / 384.0, 0.1, 100000.0);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
    glScalef(1.0, 1.0, -1.0);

    /*
     * Draw the scene
     */

//    glPolygonMode(GL_FRONT, GL_LINE);
//    glPolygonMode(GL_BACK, GL_LINE);
//    glDisable(GL_TEXTURE_2D);
    draw_scene();
//    glPolygonMode(GL_FRONT, GL_FILL);
//    glPolygonMode(GL_BACK, GL_FILL);
//    glEnable(GL_TEXTURE_2D);

    /*
     * Disable anything we enabled here
     */

    glDisable(GL_DEPTH_TEST);
}

/*
 * void r3dgl_init(UINT8 *culling_ram_8e_ptr, UINT8 *culling_ram_8c_ptr,
 *                 UINT8 *polygon_ram_ptr, UINT8 *vrom_ptr);
 *
 * Initializes the engine by passing pointers to Real3D memory regions.
 *
 * Parameters:
 *      culling_ram_8e_ptr = Pointer to Real3D culling RAM at 0x8E000000.
 *      culling_ram_8c_ptr = Pointer to Real3D culling RAM at 0x8C000000.
 *      polygon_ram_ptr    = Pointer to Real3D polygon RAM.
 *      vrom_ptr           = Pointer to VROM.
 */

void r3dgl_init(UINT8 *culling_ram_8e_ptr, UINT8 *culling_ram_8c_ptr,
                UINT8 *polygon_ram_ptr, UINT8 *vrom_ptr)
{
    culling_ram_8e = culling_ram_8e_ptr;
    culling_ram_8c = culling_ram_8c_ptr;
    polygon_ram = polygon_ram_ptr;
    vrom = vrom_ptr;
}
