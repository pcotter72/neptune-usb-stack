#ifndef VHI_VHI_H
#define VHI_VHI_H
/*
 * vhi.h - the VHI (Video Hardware Interface) driver interface
 *
 * A driver is a library "<name>.vhi" in LIBS:VHI/ whose one function, at offset -30, is
 *
 *   result = vhi_api(method, submethod, attr, errcode, handle)
 *     d0               d2      d3         d4    d5       d6        (a6 = base)
 *
 * This file was written for this tree.
 *
 * Here is the video part: what this driver implements, and the names of the
 * rest so that a refusal can be told from a mistake. Left out are the
 * digital still camera part of the standard, but for the one query that
 * says "not such a camera".
 */

#include <exec/types.h>

/* Methods */
#define VHI_METHOD_OPEN             1 /* returns the handle every other call takes */
#define VHI_METHOD_CLOSE            2
#define VHI_METHOD_SET              3
#define VHI_METHOD_GET              4
#define VHI_METHOD_PERFORM          5

/* Submethods of VHI_METHOD_GET and VHI_METHOD_SET */
#define VHI_CARD_NAME               1 /* get: string, allocated here, freed by the caller */
#define VHI_CARD_MANUFACTURER       2 /* get: string, likewise */
#define VHI_CARD_REVISION           3
#define VHI_CARD_VERSION            4
#define VHI_CARD_DRIVERAUTHOR       5 /* get: string, likewise */
#define VHI_SUPPORTED_MODES         6 /* get: VHI_MODE_ bits */
#define VHI_NUMBER_OF_INPUTS        7
#define VHI_NAME_OF_INPUT           8 /* get: attr is the input's number; string, likewise */
#define VHI_SUPPORTED_OPTIONS       9 /* get: VHI_OPT_ bits */
#define VHI_SUPPORTED_VIDEOFORMATS  10 /* get: VHI_FORMAT_ bits */
#define VHI_MAXIMUM_SIZE            11 /* get: attr is a struct vhi_size to fill */
#define VHI_OPTIONS                 12 /* set: attr is a struct vhi_setoptions */
#define VHI_VIDEOFORMAT             13 /* set: attr is one VHI_FORMAT_ bit */
#define VHI_COLOR_MODE              14 /* 1 = colour, 0 = grayscale */
#define VHI_TRUSTME_MODE            15
#define VHI_TRUSTME_SIZE            16
#define VHI_CARD_DRIVERVERSION      17
#define VHI_CARD_DRIVERREVISION     18
#define VHI_DIGITAL_CAMERA          19 /* get: VHI_DC_TYPE_ */

/* Submethods of VHI_METHOD_PERFORM */
#define VHI_CHECK_DIGITIZE_SIZE     1 /* attr is a struct vhi_dimensions to correct */
#define VHI_DIGITIZE_PICTURE        2 /* attr is a struct vhi_digitize; returns a struct vhi_image */

/* Options (VHI_SUPPORTED_OPTIONS, struct vhi_setoptions) */
#define VHI_OPT_LUMINANCE           1
#define VHI_OPT_CHROMINANCE         2
#define VHI_OPT_NOISE               4
#define VHI_OPT_VIDEOREC            8
#define VHI_OPT_NTSCPAL             16
#define VHI_OPT_INTERLACE           32
#define VHI_OPT_INPUT               64 /* value: the input's number, from 0 */

/* Video formats */
#define VHI_FORMAT_PAL              1
#define VHI_FORMAT_NTSC             2
#define VHI_FORMAT_SECAM            4
#define VHI_FORMAT_PAL60            8
#define VHI_FORMAT_NOVIDEO          16

/* Digital still camera types */
#define VHI_DC_TYPE_NOCAM           0 /* not a driver for one */

/* Modes (VHI_SUPPORTED_MODES) */
#define VHI_MODE_COLOR              1
#define VHI_MODE_GRAYSCALE          2

/* Types of struct vhi_image */
#define VHI_GRAYSCALE_8             1
#define VHI_RGB_24                  2
#define VHI_YUV_411                 3
#define VHI_YUV_422                 4
#define VHI_YUV_444                 5
#define VHI_YUV_ACCUPAK             6
#define VHI_JPEG                    7 /* chunky = the file, width = its size in bytes, height = 0 */
#define VHI_EXIF_JPEG               8

/* Errors, through the errcode pointer */
#define VHI_ERR_UNKNOWN_METHOD      1 /* how a driver says "I do not have that" */
#define VHI_ERR_COULD_NOT_INIT      2
#define VHI_ERR_NO_HARDWARE         3
#define VHI_ERR_NO_INPUT_DATA       4
#define VHI_ERR_ERR_WHILE_DIG       5
#define VHI_ERR_OUT_OF_MEMORY       6
#define VHI_ERR_INTERNAL_ERROR      7
#define VHI_ERR_UNKNOWN_OPTION      8 /* the method is known, the option is not */

struct vhi_setoptions
{
    ULONG option;       /* VHI_OPT_ */
    ULONG value;
};

struct vhi_dimensions
{
    ULONG x1;           /* the rectangle to take from the full picture */
    ULONG y1;
    ULONG x2;
    ULONG y2;
    ULONG dst_width;    /* the size to scale it to, 0 x 0 = as it is */
    ULONG dst_height;
};

struct vhi_size
{
    ULONG max_width;
    ULONG max_height;
    BOOL  fixed;        /* TRUE = no cropping */
    BOOL  scalable;     /* TRUE = the hardware scales */
};

struct vhi_image
{
    APTR  chunky;       /* grayscale, RGB, JPEG */
    APTR  y;            /* planar YUV */
    APTR  u;
    APTR  v;
    ULONG width;        /* pixels; JPEG: bytes */
    ULONG height;       /* pixels; JPEG: 0 */
    ULONG type;         /* VHI_GRAYSCALE_8 ... */
    BOOL  scaled;
};

/* The picture and its data are allocated by the driver and freed by the
   caller - with these two, if custom_memhandling is set, else with
   AllocVec() and FreeVec(). They are C functions: arguments on the stack. */
struct vhi_digitize
{
    struct vhi_dimensions dim;
    BOOL  custom_memhandling;
    APTR  (*CstAllocVec)(ULONG size, ULONG flags);
    void  (*CstFreeVec)(APTR mem);
};

#endif /* VHI_VHI_H */
