/*====================================================================*
 -  Copyright (C) 2001 Leptonica.  All rights reserved.
 -  This software is distributed in the hope that it will be
 -  useful, but with NO WARRANTY OF ANY KIND.
 -  No author or distributor accepts responsibility to anyone for the
 -  consequences of using this software, or for whether it serves any
 -  particular purpose or works at all, unless he or she says so in
 -  writing.  Everyone is granted permission to copy, modify and
 -  redistribute this source code, for commercial or non-commercial
 -  purposes, with the following restrictions: (1) the origin of this
 -  source code must not be misrepresented; (2) modified versions must
 -  be plainly marked as such; and (3) this notice may not be removed
 -  or altered from any source or modified source distribution.
 *====================================================================*/

/*
 *  jpegio.c
 *
 *    Read jpeg from file
 *          PIX            *pixReadJpeg()  [ special top level ]
 *          PIX            *pixReadStreamJpeg()
 *
 *    Write jpeg to file
 *          l_int32         pixWriteJpeg()  [ special top level ]
 *          l_int32         pixWriteStreamJpeg()
 *
 *    Read/write to memory   [not on windows]
 *          PIX            *pixReadMemJpeg()
 *          l_int32         pixWriteMemJpeg()
 *
 *    Static helpers
 *          static void     jpeg_error_do_not_exit()
 *          static l_uint8  jpeg_getc()
 *          static l_int32  jpeg_comment_callback()
 *
 *    Documentation: libjpeg.doc can be found, along with all
 *    source code, at ftp://ftp.uu.net/graphics/jpeg
 *    Download and untar the file:  jpegsrc.v6b.tar.gz
 *    A good paper on jpeg can also be found there: wallace.ps.gz
 *
 *    The functions in libjpeg make it very simple to compress
 *    and decompress images.  On input (decompression from file),
 *    3 component color images can be read into either an 8 bpp Pix
 *    with a colormap or a 32 bpp Pix with RGB components.  For output
 *    (compression to file), all color Pix, whether 8 bpp with a
 *    colormap or 32 bpp, are written compressed as a set of three
 *    8 bpp (rgb) images.
 *
 *    The default behavior of the jpeg library is to call exit.
 *    This is often undesirable, and the caller should make the
 *    decision when to abort a process.  So I inserted setjmp(s)
 *    in the reader and writer, wrote a static error handler that
 *    does not exit, and set up the cinfo structure so that the
 *    low-level jpeg library will call this error handler instead
 *    of the default function error_exit().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "allheaders.h"

static void jpeg_error_do_not_exit(j_common_ptr cinfo);
static l_uint8 jpeg_getc(j_decompress_ptr cinfo);
static jmp_buf jpeg_jmpbuf;

    /* Note: 'boolean' is defined in jmorecfg.h.  We use it explicitly
     * here because for windows where __MINGW32__ is defined,
     * the prototype for jpeg_comment_callback() is given as
     * returning a boolean.  */
static boolean jpeg_comment_callback(j_decompress_ptr cinfo);


/*---------------------------------------------------------------------*
 *                              Reading Jpeg                           *
 *---------------------------------------------------------------------*/
/*!
 *  pixReadJpeg()
 *
 *      Input:  filename
 *              colormap flag (0 means return RGB image if color;
 *                             1 means create colormap and return 8 bpp
 *                               palette image if color)
 *              reduction (scaling factor: 1, 2, 4 or 8)
 *              &pnwarn (<optional return> number of warnings about
 *                       corrupted data)
 *      Return: pix, or null on error
 *
 *  Images reduced by factors of 2, 4 or 8 can be returned
 *  significantly faster than full resolution images.
 *
 *  The jpeg library will return warnings (or exit) if
 *  the jpeg data is bad.  Use this function if you want the
 *  jpeg library to create an 8 bpp palette image, or to
 *  tell if the jpeg data has been corrupted.  For corrupt jpeg
 *  data, there are two possible outcomes:
 *    (1) a damaged pix will be returned, along with a nonzero
 *        number of warnings, or
 *    (2) for sufficiently serious problems, the library will attempt
 *        to exit (caught by our error handler) and no pix will be returned.
 */
PIX *
pixReadJpeg(const char  *filename,
            l_int32      cmflag,
            l_int32      reduction,
            l_int32     *pnwarn)
{
FILE  *fp;
PIX   *pix;

    PROCNAME("pixReadJpeg");

    if (!filename)
        return (PIX *)ERROR_PTR("filename not defined", procName, NULL);
    if (pnwarn)
        *pnwarn = 0;  /* init */
    if (cmflag != 0 && cmflag != 1)
        cmflag = 0;  /* default */
    if (reduction != 1 && reduction != 2 && reduction != 4 && reduction != 8)
        return (PIX *)ERROR_PTR("reduction not in {1,2,4,8}", procName, NULL);

    if ((fp = fopenReadStream(filename)) == NULL)
        return (PIX *)ERROR_PTR("image file not found", procName, NULL);
    pix = pixReadStreamJpeg(fp, cmflag, reduction, pnwarn, 0);
    fclose(fp);

    if (!pix)
        return (PIX *)ERROR_PTR("image not returned", procName, NULL);
    return pix;
}


/*!
 *  pixReadStreamJpeg()
 *
 *      Input:  stream
 *              colormap flag (0 means return RGB image if color;
 *                             1 means create colormap and return 8 bpp
 *                               palette image if color)
 *              reduction (scaling factor: 1, 2, 4 or 8)
 *              &pnwarn (<optional return> number of warnings)
 *              hint: a bitwise OR of L_HINT_* values
 *      Return: pix, or null on error
 *
 *  Usage: see pixReadJpeg()
 */
PIX *
pixReadStreamJpeg(FILE     *fp,
                  l_int32   cmflag,
                  l_int32   reduction,
                  l_int32  *pnwarn,
                  l_int32   hint)
{
l_uint8                        cyan, yellow, magenta, black, white;
l_int32                        rval, gval, bval;
l_int32                        i, j, k;
l_int32                        w, h, wpl, spp, ncolors, cindex;
l_uint32                      *data;
l_uint32                      *line, *ppixel;
JSAMPROW                       rowbuffer;
PIX                           *pix;
PIXCMAP                       *cmap;
struct jpeg_decompress_struct  cinfo;
struct jpeg_error_mgr          jerr;
l_uint8                       *comment = NULL;

    PROCNAME("pixReadStreamJpeg");

    if (!fp)
        return (PIX *)ERROR_PTR("fp not defined", procName, NULL);
    if (pnwarn)
        *pnwarn = 0;  /* init */
    if (cmflag != 0 && cmflag != 1)
        cmflag = 0;  /* default */
    if (reduction != 1 && reduction != 2 && reduction != 4 && reduction != 8)
        return (PIX *)ERROR_PTR("reduction not in {1,2,4,8}", procName, NULL);

    if (BITS_IN_JSAMPLE != 8)  /* set in jmorecfg.h */
        return (PIX *)ERROR_PTR("BITS_IN_JSAMPLE != 8", procName, NULL);

    rewind(fp);

    pix = NULL;  /* init */
    if (setjmp(jpeg_jmpbuf)) {
        pixDestroy(&pix);
        FREE(rowbuffer);
        return (PIX *)ERROR_PTR("internal jpeg error", procName, NULL);
    }

    rowbuffer = NULL;
    cinfo.err = jpeg_std_error(&jerr);
    jerr.error_exit = jpeg_error_do_not_exit; /* catch error; do not exit! */

    jpeg_create_decompress(&cinfo);

    cinfo.client_data = &comment;
    jpeg_set_marker_processor(&cinfo, JPEG_COM, jpeg_comment_callback);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.scale_denom = reduction;
    if (hint & L_HINT_GRAY)
        cinfo.out_color_space = JCS_GRAYSCALE;
    jpeg_calc_output_dimensions(&cinfo);

        /* Allocate the image and a row buffer */
    spp = cinfo.out_color_components;
    w = cinfo.output_width;
    h = cinfo.output_height;
    int ycck = (cinfo.jpeg_color_space == JCS_YCCK && spp == 4 && cmflag == 0);
    if (spp != 1 && spp != 3 && !ycck) {
        if (comment) FREE(comment);
        return (PIX *)ERROR_PTR("spp must be 1 or 3 or YCCK", procName, NULL);
    }
    if ((spp == 3 && cmflag == 0) || ycck) {  /* rgb pix */
        rowbuffer = (JSAMPROW)CALLOC(sizeof(JSAMPLE), spp * w);
        pix = pixCreate(w, h, 32);
    }
    else {  /* 8 bpp gray or colormapped */
        rowbuffer = (JSAMPROW)CALLOC(sizeof(JSAMPLE), w);
        pix = pixCreate(w, h, 8);
    }
    if (!rowbuffer || !pix) {
        if (comment) FREE(comment);
	if (rowbuffer) FREE(rowbuffer);
	pixDestroy(&pix);
        return (PIX *)ERROR_PTR("rowbuffer or pix not made", procName, NULL);
    }

    if (comment) {
        pixSetText(pix, (char *)comment);
	FREE(comment);
    }

    if (spp == 1)  /* Grayscale or colormapped */
        jpeg_start_decompress(&cinfo);
    else  {        /* Color; spp == 3 or YCCK */
        if (cmflag == 0) {   /* -- 24 bit color in 32 bit pix or CYYK -- */
            cinfo.quantize_colors = FALSE;
            jpeg_start_decompress(&cinfo);
        }
        else {      /* Color quantize to 8 bits */
            cinfo.quantize_colors = TRUE;
            cinfo.desired_number_of_colors = 256;
            jpeg_start_decompress(&cinfo);

                /* Construct a pix cmap */
            cmap = pixcmapCreate(8);
            ncolors = cinfo.actual_number_of_colors;
            for (cindex = 0; cindex < ncolors; cindex++)
            {
                rval = cinfo.colormap[0][cindex];
                gval = cinfo.colormap[1][cindex];
                bval = cinfo.colormap[2][cindex];
                pixcmapAddColor(cmap, rval, gval, bval);
            }
            pixSetColormap(pix, cmap);
        }
    }
    wpl  = pixGetWpl(pix);
    data = pixGetData(pix);

        /* Decompress */
    if ((spp == 3 && cmflag == 0) || ycck) {   /* -- 24 bit color -- */
        for (i = 0; i < h; i++) {
            if (jpeg_read_scanlines(&cinfo, &rowbuffer, (JDIMENSION)1) != 1)
                return (PIX *)ERROR_PTR("bad read scanline", procName, NULL);
            ppixel = data + i * wpl;
            if (spp == 3) {
                for (j = k = 0; j < w; j++) {
                    SET_DATA_BYTE(ppixel, COLOR_RED, rowbuffer[k++]);
                    SET_DATA_BYTE(ppixel, COLOR_GREEN, rowbuffer[k++]);
                    SET_DATA_BYTE(ppixel, COLOR_BLUE, rowbuffer[k++]);
                    ppixel++;
                }
            } else {
                    /* This is a conversion from CMYK -> RGB that ignores
                       color profiles, and is invoked when the image header
                       claims to be in YCCK colorspace.  libjpeg may be
                       doing YCCK -> CMYK under the hood. To understand why
                       the colors are inverted on read-in, see the "Special
                       color spaces" section of "Using the IJG JPEG Library"
                       by Thomas G. Lane.  */
                for (j = k = 0; j < w; j++) {
                    cyan = 255 - rowbuffer[k++];
                    magenta = 255 - rowbuffer[k++];
                    yellow = 255 - rowbuffer[k++];
                    white = rowbuffer[k++];
                    black = 255 - white;
                    rval = 255 - (cyan    * white) / 255 - black;
                    gval = 255 - (magenta * white) / 255 - black;
                    bval = 255 - (yellow  * white) / 255 - black;
                    rval = L_MIN(L_MAX(rval, 0), 255);
                    gval = L_MIN(L_MAX(gval, 0), 255);
                    bval = L_MIN(L_MAX(bval, 0), 255);
                    SET_DATA_BYTE(ppixel, COLOR_RED, rval);
                    SET_DATA_BYTE(ppixel, COLOR_GREEN, gval);
                    SET_DATA_BYTE(ppixel, COLOR_BLUE, bval);
                    ppixel++;
                }
            }
        }
    }
    else {    /* 8 bpp grayscale or colormapped pix */
        for (i = 0; i < h; i++) {
            if (jpeg_read_scanlines(&cinfo, &rowbuffer, (JDIMENSION)1) != 1)
                return (PIX *)ERROR_PTR("bad read scanline", procName, NULL);
            line = data + i * wpl;
            for (j = 0; j < w; j++)
                SET_DATA_BYTE(line, j, rowbuffer[j]);
        }
    }

    if (pnwarn)
        *pnwarn = cinfo.err->num_warnings;

    switch (cinfo.density_unit)
    {
    case 1:  /* pixels per inch */
        pixSetXRes(pix, cinfo.X_density);
        pixSetYRes(pix, cinfo.Y_density);
        break;
    case 2:  /* pixels per centimeter */
        pixSetXRes(pix, (l_int32)((l_float32)cinfo.X_density * 2.54 + 0.5));
        pixSetYRes(pix, (l_int32)((l_float32)cinfo.Y_density * 2.54 + 0.5));
        break;
    default:   /* the pixel density may not be defined; ignore */
        break;
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    FREE(rowbuffer);

    return pix;
}



/*---------------------------------------------------------------------*
 *                             Writing Jpeg                            *
 *---------------------------------------------------------------------*/
/*!
 *  pixWriteJpeg()
 *
 *      Input:  filename
 *              pix
 *              quality (1 - 100; 75 is default)
 *              progressive (0 for baseline sequential; 1 for progressive)
 *      Return: 0 if OK; 1 on error
 */
l_int32
pixWriteJpeg(const char  *filename,
             PIX         *pix,
             l_int32      quality,
             l_int32      progressive)
{
FILE  *fp;

    PROCNAME("pixWriteJpeg");

    if (!pix)
        return ERROR_INT("pix not defined", procName, 1);
    if (!filename)
        return ERROR_INT("filename not defined", procName, 1);

    if ((fp = fopen(filename, "wb+")) == NULL)
        return ERROR_INT("stream not opened", procName, 1);

    if (pixWriteStreamJpeg(fp, pix, quality, progressive)) {
        fclose(fp);
        return ERROR_INT("pix not written to stream", procName, 1);
    }

    fclose(fp);
    return 0;
}


/*!
 *  pixWriteStreamJpeg()
 *
 *      Input:  stream
 *              pix  (8 or 32 bpp)
 *              quality  (1 - 100; 75 is default value; 0 is also default)
 *              progressive (0 for baseline sequential; 1 for progressive)
 *      Return: 0 if OK, 1 on error
 *
 *  Notes:
 *      (1) Under the covers, the library transforms rgb to a
 *          luminence-chromaticity triple, each component of which is
 *          also 8 bits, and compresses that.  It uses 2 Huffman tables,
 *          a higher resolution one (with more quantization levels)
 *          for luminosity and a lower resolution one for the chromas.
 *      (2) Progressive encoding gives better compression, at the
 *          expense of slower encoding and decoding.
 *      (3) There are three possibilities:
 *          * Grayscale image, no colormap: compress as 8 bpp image.
 *          * 24 bpp full color image: copy each line into the color
 *            line buffer, and compress as three 8 bpp images.
 *          * 8 bpp colormapped image: convert each line to three
 *            8 bpp line images in the color line buffer, and
 *            compress as three 8 bpp images.
 */
l_int32
pixWriteStreamJpeg(FILE    *fp,
                   PIX     *pix,
                   l_int32  quality,
                   l_int32  progressive)
{
l_uint8                      byteval;
l_int32                      xres, yres;
l_int32                      i, j, k;
l_int32                      w, h, d, wpl, spp, colorflg, rowsamples;
l_int32                     *rmap, *gmap, *bmap;
l_uint32                    *ppixel, *line, *data;
JSAMPROW                     rowbuffer;
PIXCMAP                     *cmap;
struct jpeg_compress_struct  cinfo;
struct jpeg_error_mgr        jerr;
const char                  *text;

    PROCNAME("pixWriteStreamJpeg");

    if (!fp)
        return ERROR_INT("stream not open", procName, 1);
    if (!pix)
        return ERROR_INT("pix not defined", procName, 1);
    rewind(fp);

    if (setjmp(jpeg_jmpbuf)) {
        FREE(rowbuffer);
        if (colorflg == 1) {
            FREE(rmap);
            FREE(gmap);
            FREE(bmap);
        }
        return ERROR_INT("internal jpeg error", procName, 1);
    }

    rowbuffer = NULL;
    rmap = NULL;
    gmap = NULL;
    bmap = NULL;
    w = pixGetWidth(pix);
    h = pixGetHeight(pix);
    d = pixGetDepth(pix);
    if (d != 8 && d != 32)
        return ERROR_INT("bpp must be 8 or 32", procName, 1);

    if (quality <= 0)
        quality = 75;  /* default */

    if (d == 32)
        colorflg = 2;    /* 24 bpp rgb; no colormap */
    else if ((cmap = pixGetColormap(pix)) == NULL)
        colorflg = 0;    /* 8 bpp grayscale; no colormap */
    else {
        colorflg = 1;    /* 8 bpp; colormap */
        pixcmapToArrays(cmap, &rmap, &gmap, &bmap);
    }

    cinfo.err = jpeg_std_error(&jerr);
    jerr.error_exit = jpeg_error_do_not_exit; /* catch error; do not exit! */

    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);

    cinfo.image_width  = w;
    cinfo.image_height = h;

    if (colorflg == 0) {
        cinfo.input_components = 1;
        cinfo.in_color_space = JCS_GRAYSCALE;
    }
    else {  /* colorflg == 1 or 2 */
        cinfo.input_components = 3;
        cinfo.in_color_space = JCS_RGB;
    }

    jpeg_set_defaults(&cinfo);

        /* Setting optimize_coding to TRUE seems to improve compression
	 * by approx 2-4 percent, and increases comp time by approx 20%. */
    cinfo.optimize_coding = FALSE;

    xres = pixGetXRes(pix);
    yres = pixGetYRes(pix);
    if ((xres != 0) && (yres != 0)) {
        cinfo.density_unit = 1;  /* designates pixels per inch */
        cinfo.X_density = xres;
        cinfo.Y_density = yres;
    }

    jpeg_set_quality(&cinfo, quality, TRUE);
    if (progressive) {
        jpeg_simple_progression(&cinfo);
    }

    jpeg_start_compress(&cinfo, TRUE);

    if ((text = pixGetText(pix))) {
        jpeg_write_marker(&cinfo, JPEG_COM, (const JOCTET *)text, strlen(text));
    }

        /* Allocate row buffer */
    spp = cinfo.input_components;
    rowsamples = spp * w;
    if ((rowbuffer = (JSAMPROW)CALLOC(sizeof(JSAMPLE), rowsamples)) == NULL)
        return ERROR_INT("calloc fail for rowbuffer", procName, 1);

    data = pixGetData(pix);
    wpl  = pixGetWpl(pix);
    for (i = 0; i < h; i++) {
        line = data + i * wpl;
        if (colorflg == 0) {        /* 8 bpp gray */
            for (j = 0; j < w; j++)
                rowbuffer[j] = GET_DATA_BYTE(line, j);
        }
        else if (colorflg == 1) {  /* 8 bpp colormapped */
            for (j = 0; j < w; j++) {
                byteval = GET_DATA_BYTE(line, j);
                rowbuffer[3 * j + COLOR_RED] = rmap[byteval];
                rowbuffer[3 * j + COLOR_GREEN] = gmap[byteval];
                rowbuffer[3 * j + COLOR_BLUE] = bmap[byteval];
            }
        }
        else { /* 24 bpp color */
            ppixel = line;
            for (j = k = 0; j < w; j++) {
                rowbuffer[k++] = GET_DATA_BYTE(ppixel, COLOR_RED);
                rowbuffer[k++] = GET_DATA_BYTE(ppixel, COLOR_GREEN);
                rowbuffer[k++] = GET_DATA_BYTE(ppixel, COLOR_BLUE);
                ppixel++;
            }
        }
        jpeg_write_scanlines(&cinfo, &rowbuffer, 1);
    }

    jpeg_finish_compress(&cinfo);

    FREE(rowbuffer);
    if (colorflg == 1) {
        FREE(rmap);
        FREE(gmap);
        FREE(bmap);
    }

    jpeg_destroy_compress(&cinfo);
    return 0;
}


/*---------------------------------------------------------------------*
 *                         Read/write to memory                        *
 *---------------------------------------------------------------------*/
#if !defined (__MINGW32__) && !defined(_CYGWIN_ENVIRON)

extern FILE *open_memstream(char **data, size_t *size);
extern FILE *fmemopen(void *data, size_t size, const char *mode);

/*!
 *  pixReadMemJpeg()
 *
 *      Input:  data (const; jpeg-encoded)
 *              size (of data)
 *              colormap flag (0 means return RGB image if color;
 *                             1 means create colormap and return 8 bpp
 *                               palette image if color)
 *              reduction (scaling factor: 1, 2, 4 or 8)
 *              &pnwarn (<optional return> number of warnings)
 *              hint: a bitwise OR of L_HINT_* values
 *      Return: pix, or null on error
 *
 *  Notes:
 *      (1) The @size byte of @data must be a null character.
 *      (2) See pixReadJpeg() for usage.
 */
PIX *
pixReadMemJpeg(const l_uint8  *cdata,
               l_uint32        size,
               l_int32         cmflag,
               l_int32         reduction,
               l_int32        *pnwarn,
               l_int32         hint)
{
l_uint8  *data;
FILE     *fp;
PIX      *pix;

    PROCNAME("pixReadMemJpeg");

    if (!cdata)
        return (PIX *)ERROR_PTR("cdata not defined", procName, NULL);

    data = (l_uint8 *)cdata;  /* we're really not going to change this */
    if ((fp = fmemopen(data, (size_t)size, "r")) == NULL)
        return (PIX *)ERROR_PTR("stream not opened", procName, NULL);
    pix = pixReadStreamJpeg(fp, cmflag, reduction, pnwarn, hint);
    fclose(fp);
    return pix;
}


/*!
 *  pixWriteMemJpeg()
 *
 *      Input:  &data (<return> data of tiff compressed image)
 *              &size (<return> size of returned data)
 *              pix
 *              quality  (1 - 100; 75 is default value; 0 is also default)
 *              progressive (0 for baseline sequential; 1 for progressive)
 *      Return: 0 if OK, 1 on error
 *
 *  Notes:
 *      (1) See pixWriteStreamJpeg() for usage.  This version writes to
 *          memory instead of to a file stream.
 */
l_int32
pixWriteMemJpeg(l_uint8  **pdata,
                l_uint32  *psize,
                PIX       *pix,
                l_int32    quality,
                l_int32    progressive)
{
FILE  *fp;

    PROCNAME("pixWriteMemJpeg");

    if (!pdata)
        return ERROR_INT("&data not defined", procName, 1 );
    if (!psize)
        return ERROR_INT("&size not defined", procName, 1 );
    if (!pix)
        return ERROR_INT("&pix not defined", procName, 1 );

    if ((fp = open_memstream((char **)pdata, (size_t *)psize)) == NULL)
        return ERROR_INT("stream not opened", procName, 1);
    pixWriteStreamJpeg(fp, pix, quality, progressive);
    fclose(fp);
    return 0;
}

#else

PIX *
pixReadMemJpeg(const l_uint8  *data,
               l_uint32        size,
               l_int32         cmflag,
               l_int32         reduction,
               l_int32        *pnwarn,
               l_int32         hint)
{
    return (PIX *)ERROR_PTR("jpeg read from memory not implemented on windows",
                            "pixReadMemJpeg", NULL);
}


l_int32
pixWriteMemJpeg(l_uint8  **pdata,
                l_uint32  *psize,
                PIX       *pix,
                l_int32    quality,
                l_int32    progressive)
{
    return ERROR_INT("jpeg write to memory not implemented on windows",
                     "pixWriteMemJpeg", 1);
}

#endif  /* !defined (__MINGW32__) && !defined(_CYGWIN_ENVIRON) */



/*---------------------------------------------------------------------*
 *                           Static helpers                            *
 *---------------------------------------------------------------------*/
    /* The default jpeg error_exit() kills the process.
     * We don't want leptonica to allow this to happen.
     * If you want this default behavior, remove the
     * calls to this in the functions above. */
static void
jpeg_error_do_not_exit(j_common_ptr cinfo)
{
    (*cinfo->err->output_message) (cinfo);
    jpeg_destroy(cinfo);
    longjmp(jpeg_jmpbuf, 0);
    return;
}

    /* This function was borrowed from libjpeg. */
static l_uint8
jpeg_getc(j_decompress_ptr cinfo)
{
struct jpeg_source_mgr *datasrc;

    datasrc = cinfo->src;
    if (datasrc->bytes_in_buffer == 0) {
        if (! (*datasrc->fill_input_buffer) (cinfo)) {
            return 0;
        }
    }
    datasrc->bytes_in_buffer--;
    return GETJOCTET(*datasrc->next_input_byte++);
}


    /* This function is required for reading jpeg comments, and
     * was contributed by Antony Dovgal.  Why 'boolean'?  See
     * note above the declaration. */
static boolean
jpeg_comment_callback(j_decompress_ptr cinfo)
{
l_int32    length, i;
l_uint32   c;
l_uint8  **comment;

    comment = (l_uint8 **)cinfo->client_data;
    length = jpeg_getc(cinfo) << 8;
    length += jpeg_getc(cinfo);
    length -= 2;

    if (length <= 0)
        return 1;

    *comment = (l_uint8 *)MALLOC(length + 1);
    if (!(*comment))
        return 0;

    for (i = 0; i < length; i++) {
        c = jpeg_getc(cinfo);
        (*comment)[i] = c;
    }
    (*comment)[length] = 0;

    return 1;
}
