/* $Id: tif_strip.c,v 1.20 2007-03-28 02:50:41 joris Exp $ */

/*
 * Copyright (c) 1991-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 *
 * Strip-organized Image Support Routines.
 */
#include "tiffiop.h"

static uint32
summarize_32(TIFF* tif, uint32 summand1, uint32 summand2, const char* where)
{
	uint32 bytes = summand1 + summand2;

	if (bytes - summand1 != summand2) {
		TIFFErrorExt(tif->tif_clientdata, tif->tif_name, "Integer overflow in %s", where);
		bytes = 0;
	}

	return (bytes);
}

static uint32
multiply_32(TIFF* tif, uint32 nmemb, uint32 elem_size, const char* where)
{
	uint32 bytes = nmemb * elem_size;

	if (elem_size && bytes / elem_size != nmemb) {
		TIFFErrorExt(tif->tif_clientdata, tif->tif_name, "Integer overflow in %s", where);
		bytes = 0;
	}

	return (bytes);
}

static uint64
multiply_64(TIFF* tif, uint64 nmemb, uint64 elem_size, const char* where)
{
	uint64 bytes = nmemb * elem_size;

	if (elem_size && bytes / elem_size != nmemb) {
		TIFFErrorExt(tif->tif_clientdata, tif->tif_name, "Integer overflow in %s", where);
		bytes = 0;
	}

	return (bytes);
}

/*
 * Compute which strip a (row,sample) value is in.
 */
tstrip_t
TIFFComputeStrip(TIFF* tif, uint32 row, tsample_t sample)
{
	TIFFDirectory *td = &tif->tif_dir;
	tstrip_t strip;

	strip = row / td->td_rowsperstrip;
	if (td->td_planarconfig == PLANARCONFIG_SEPARATE) {
		if (sample >= td->td_samplesperpixel) {
			TIFFErrorExt(tif->tif_clientdata, tif->tif_name,
			    "%lu: Sample out of range, max %lu",
			    (unsigned long) sample, (unsigned long) td->td_samplesperpixel);
			return ((tstrip_t) 0);
		}
		strip += sample*td->td_stripsperimage;
	}
	return (strip);
}

/*
 * Compute how many strips are in an image.
 */
uint32
TIFFNumberOfStrips(TIFF* tif)
{
	TIFFDirectory *td = &tif->tif_dir;
	uint32 nstrips;

	nstrips = (td->td_rowsperstrip == (uint32) -1 ? 1 :
	     TIFFhowmany_32(td->td_imagelength, td->td_rowsperstrip));
	if (td->td_planarconfig == PLANARCONFIG_SEPARATE)
		nstrips = multiply_32(tif, nstrips, (uint32)td->td_samplesperpixel,
				   "TIFFNumberOfStrips");
	return (nstrips);
}

/*
 * Compute the # bytes in a variable height, row-aligned strip.
 */
uint64
TIFFVStripSize(TIFF* tif, uint32 nrows)
{
	static const char module[] = "TIFFVStripSize";
	TIFFDirectory *td = &tif->tif_dir;
	if (nrows==(uint32)(-1))
		nrows=td->td_imagelength;
	if ((td->td_planarconfig==PLANARCONFIG_CONTIG)&&
	    (td->td_photometric == PHOTOMETRIC_YCBCR)&&
	    (!isUpSampled(tif)))
	{
		/*
		 * Packed YCbCr data contain one Cb+Cr for every
		 * HorizontalSampling*VerticalSampling Y values.
		 * Must also roundup width and height when calculating
		 * since images that are not a multiple of the
		 * horizontal/vertical subsampling area include
		 * YCbCr data for the extended image.
		 */
		uint16 ycbcrsubsampling[2];
		uint16 samplingblock_samples;
		uint32 samplingblocks_hor;
		uint32 samplingblocks_ver;
		uint64 samplingrow_samples;
		uint64 samplingrow_size;
		assert(td->td_samplesperpixel==3);
		TIFFGetField(tif,TIFFTAG_YCBCRSUBSAMPLING,ycbcrsubsampling+0,
		    ycbcrsubsampling+1);
		if (ycbcrsubsampling[0]*ycbcrsubsampling[1]==0)
		{
			TIFFErrorExt(tif->tif_clientdata,module,
			    "Invalid YCbCr subsampling");
			return 0;
		}
		samplingblock_samples=ycbcrsubsampling[0]*ycbcrsubsampling[1]+2;
		samplingblocks_hor=TIFFhowmany_32(td->td_imagewidth,ycbcrsubsampling[0]);
		samplingblocks_ver=TIFFhowmany_32(nrows,ycbcrsubsampling[1]);
		samplingrow_samples=multiply_64(tif,samplingblocks_hor,samplingblock_samples,module);
		samplingrow_size=TIFFhowmany_64(multiply_64(tif,samplingrow_samples,td->td_bitspersample,module),8);
		return(multiply_64(tif,samplingrow_size,samplingblocks_ver,module));
	} else
		return(multiply_64(tif,nrows,TIFFScanlineSize(tif),module));
}


/*
 * Compute the # bytes in a raw strip.
 */
tsize_t
TIFFRawStripSize(TIFF* tif, tstrip_t strip)
{
	TIFFDirectory* td = &tif->tif_dir;
	tsize_t bytecount = td->td_stripbytecount[strip];

	if (bytecount <= 0) {
		TIFFErrorExt(tif->tif_clientdata, tif->tif_name,
			  "%lu: Invalid strip byte count, strip %lu",
			  (unsigned long) bytecount, (unsigned long) strip);
		bytecount = (tsize_t) -1;
	}

	return bytecount;
}

/*
 * Compute the # bytes in a (row-aligned) strip.
 *
 * Note that if RowsPerStrip is larger than the
 * recorded ImageLength, then the strip size is
 * truncated to reflect the actual space required
 * to hold the strip.
 */
uint64
TIFFStripSize(TIFF* tif)
{
	TIFFDirectory* td = &tif->tif_dir;
	uint32 rps = td->td_rowsperstrip;
	if (rps > td->td_imagelength)
		rps = td->td_imagelength;
	return (TIFFVStripSize(tif, rps));
}

/*
 * Compute a default strip size based on the image
 * characteristics and a requested value.  If the
 * request is <1 then we choose a strip size according
 * to certain heuristics.
 */
uint32
TIFFDefaultStripSize(TIFF* tif, uint32 request)
{
	return (*tif->tif_defstripsize)(tif, request);
}

uint32
_TIFFDefaultStripSize(TIFF* tif, uint32 s)
{
	if ((int32) s < 1) {
		/*
		 * If RowsPerStrip is unspecified, try to break the
		 * image up into strips that are approximately
		 * STRIP_SIZE_DEFAULT bytes long.
		 */
		uint64 scanlinesize;
		uint64 rows;
		scanlinesize=TIFFScanlineSize(tif);
		if (scanlinesize==0)
			scanlinesize=1;
		rows=(uint64)STRIP_SIZE_DEFAULT/scanlinesize;
		if (rows==0)
			rows=1;
		else if (rows>0xFFFFFFFF)
			rows=0xFFFFFFFF;
		s=(uint32)rows;
	}
	return (s);
}

/*
 * Return the number of bytes to read/write in a call to
 * one of the scanline-oriented i/o routines.  Note that
 * this number may be 1/samples-per-pixel if data is
 * stored as separate planes.
 * The ScanlineSize in case of YCbCrSubsampling is defined as the
 * strip size divided by the strip height, i.e. the size of a pack of vertical
 * subsampling lines divided by vertical subsampling. It should thus make
 * sense when multiplied by a multiple of vertical subsampling.
 */
uint64
TIFFScanlineSize(TIFF* tif)
{
	static const char module[] = "TIFFScanlineSize";
	TIFFDirectory *td = &tif->tif_dir;
	uint64 scanline_size;
	if (td->td_planarconfig==PLANARCONFIG_CONTIG)
	{
		if ((td->td_photometric==PHOTOMETRIC_YCBCR)&&(!isUpSampled(tif)))
		{
			uint16 ycbcrsubsampling[2];
			uint16 samplingblock_samples;
			uint32 samplingblocks_hor;
			uint64 samplingrow_samples;
			uint64 samplingrow_size;
			assert(td->td_samplesperpixel==3);
			TIFFGetField(tif,TIFFTAG_YCBCRSUBSAMPLING,ycbcrsubsampling+0,
			    ycbcrsubsampling+1);
			if (ycbcrsubsampling[0]*ycbcrsubsampling[1]==0)
			{
				TIFFErrorExt(tif->tif_clientdata,module,
				    "Invalid YCbCr subsampling");
				return 0;
			}
			samplingblock_samples=ycbcrsubsampling[0]*ycbcrsubsampling[1]+2;
			samplingblocks_hor=TIFFhowmany_32(td->td_imagewidth,ycbcrsubsampling[0]);
			samplingrow_samples=multiply_64(tif,samplingblocks_hor,samplingblock_samples,module);
			samplingrow_size=TIFFhowmany_64(multiply_64(tif,samplingrow_samples,td->td_bitspersample,module),8);  
			assert((samplingrow_size%ycbcrsubsampling[1])==0);
			scanline_size=(samplingrow_size/ycbcrsubsampling[1]);
		}
		else
		{
			uint64 scanline_samples;
			scanline_samples=multiply_64(tif,td->td_imagewidth,td->td_samplesperpixel,module);
			scanline_size=TIFFhowmany_64(multiply_64(tif,scanline_samples,td->td_bitspersample,module),8);
		}
	}
	else
		scanline_size=TIFFhowmany_64(multiply_64(tif,td->td_imagewidth,td->td_bitspersample,module),8);
	return(scanline_size);
}

/*
 * Return the number of bytes required to store a complete
 * decoded and packed raster scanline (as opposed to the
 * I/O size returned by TIFFScanlineSize which may be less
 * if data is store as separate planes).
 */
uint64
TIFFRasterScanlineSize(TIFF* tif)
{
	static const char module[] = "TIFFRasterScanlineSize";
	TIFFDirectory *td = &tif->tif_dir;
	uint64 scanline;
	
	scanline = multiply_64 (tif, td->td_bitspersample, td->td_imagewidth, module);
	if (td->td_planarconfig == PLANARCONFIG_CONTIG) {
		scanline = multiply_64 (tif, scanline, td->td_samplesperpixel, module);
		return (TIFFhowmany8_64(scanline));
	} else
		return (multiply_64 (tif, TIFFhowmany8_64(scanline),
					    td->td_samplesperpixel,
					    module));
}

/* vim: set ts=8 sts=8 sw=8 noet: */
