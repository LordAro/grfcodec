
/*****************************************\
*                                         *
* GRFMerge - A program to integrate a     *
*            .GRD file generated by       *
*            GRFDiff into the respective  *
*            GRF file.                    *
*                                         *
*                                         *
* Copyright (C) 2003 by Josef Drexler     *
*                      <jdrexler@uwo.ca>  *
*                                         *
* Permission granted to copy and redist-  *
* ribute under the terms of the GNU GPL.  *
* For more info please read the file      *
* COPYING which should have come with     *
* this file.                              *
*                                         *
\*****************************************/

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "getopt.h"
#include "typesize.h"
#include "version.h"
#include "grfcomm.h"

static const U32 GRDmagic = 0x67fb49ad;
#define TEMPFILE "grfmerge.tmp"

static int alwaysyes = 0;
static char *grdfile = NULL;
static long grdofs = 0;
static int onlyshow = 0, issfx = 0;

static void usage(void)
{
	printf(
		"\nUsage:\n"
		"    GRFMerge [options] %s[<GRF-File>]\n"
		"	Change sprites in the GRF file to the new ones from the GRD file.\n"
		"	If the GRF file is not specified, GRFMerge will modify the one\n"
		"	which the GRD file was generated from.\n"
		"\n"
		"Options:\n"
		"	-h  Show this help\n"
		"	-l  Only show which sprites the GRD file contains, don't integrate them\n"
		"	-y  Answer 'y' to all questions\n"
		"\n"
		"GRFMerge is Copyright (C) 2003 by Josef Drexler\n"
		"It may be freely copied and distributed.\n",
		issfx?"":"<GRD-File> "
		);

	exit(1);
}

static int checkisselfextr(const char *exe)
{
	static const char *action = "reading exe";
	FILE *f;
	char c[3];
	U8 r, e;
	U32 magic = 0;

	f = fopen(exe, "rb");
	if (!f && errno == ENOENT) {
		// try appending .exe for Win2k
		char *altexe = (char*) malloc(strlen(exe)+5);
		strcpy(altexe, exe);
		strcat(altexe, ".exe");
		f = fopen(altexe, "rb");
		free(altexe);
	}
	if (!f) return 0;

	cfread(action, c, 2, 1, f);
	if (c[0] != 'M' || c[1] != 'Z')
		return 0;

	fseek(f, 0x1c, SEEK_SET);
	cfread(action, c, 2, 1, f);
	c[2] = 0;

	if (!strcmp(c, "JD")) {
		cfread(action, &r, 1, 1, f);
		cfread(action, &e, 1, 1, f);

		grdofs = (long) r * (1L << e);
		fseek(f, grdofs, SEEK_SET);

		cfread(action, &magic, 4, 1, f);
	}
	fclose(f);

	return issfx = magic == GRDmagic;
}

static void die(const char *text, ...)
{
	va_list argptr;

	va_start(argptr, text);
	vprintf(text, argptr);
	va_end(argptr);

	exit(2);
}

static int yesno(const char *txt)
{
	char c;

	printf(" [Y/N] ");
	if (alwaysyes) {
		printf("Y\n");
	} else {
		c = tolower(getc(stdin));

		// skip the newline
		getc(stdin);

		if (c != 'y') {
			printf("%s", txt);
			return 0;
		}
	}
	return 1;
}

static char *block = NULL;
#define BLOCKSIZE 8192
static void copyblock(size_t size, FILE *from, FILE *to)
{
	size_t thisblock;

	while (size > 0) {
		if (size > BLOCKSIZE)
			thisblock = BLOCKSIZE;
		else
			thisblock = size;

		cfread("copying block", block, 1, thisblock, from);

		if (to) cfwrite("copying block", block, 1, thisblock, to);

		size -= thisblock;
	}
}

// this function reads a piece of data, and copies it to the other files
static void copy(void *data, size_t size, size_t n, FILE *from, FILE *to)
{
	size_t res;

	res = fread(data, size, n, from);
	if (res != n)
		die("Error while reading, wanted %d, got %d: %s", n, res, strerror(errno));

	if (to) {
		res = fwrite(data, size, n, to);
		if (res != n)
			die("Error while writing, wanted %d, wrote %d: %s", n, res, strerror(errno));
	}
}

//
// Copy the data of a sprite from one file to another
// this is complicated because for some sprites, the GRF file stores
// the uncompressed length instead of how many bytes are in the file
//
// This code mostly copied from sprites.c, but to keep this program
// small I didn't link in sprites.c and removed the unnecessary steps
// from decodesprite (we're not actually interested in the uncompressed data,
// just the length)
//

static U16 copysprite(FILE *from, FILE *to)
{
	U8 info, ofs;
	S8 code;
	U16 size;
	size_t reallen;

#ifdef DEBUG
	if (from)
		printf("Copying from pos. %ld\n", ftell(from));
	if (to)
		printf("Copying to pos. %ld\n", ftell(to));
#endif

	if (feof(from)) return 0;

	copy(&size, 2, 1, from, to);

	if (!size) return 0;	// EOF is also indicated by a zero-size sprite

	copy(&info, 1, 1, from, to);

	if (info == 0xff) {	// verbatim data
		copyblock(size, from, to);
		return 1;
	}

	if (info & 2) {	// size is compressed size
		copyblock(size-1, from, to);	// already copied one byte
		return 1;
	}

	// the size given is the uncompressed size, so we need to go through
	// the uncompression routine until we have enough bytes
	copyblock(7, from, to);
	size -= 8;
	while (size > 0) {
		copy(&code, 1, 1, from, to);

		if (code < 0) {
			copy(&ofs, 1, 1, from, to);
			reallen = -(code >> 3);
		} else {
			reallen = code ? code : 128;
			copyblock(reallen, from, to);
		}
		if (size < reallen)
			die("\nOops, got too many bytes. How did that happen?\n"
			"Size is %d, len is %d at GRF file pos %ld\n",
			size, reallen, ftell(from));

		size -= reallen;
	}
	return 1;
}

static void skipsprite(FILE *f)
{
	copysprite(f, NULL);
}

static void showpct(long now, long total, int spriteno, int *pct)
{
	int newpct = 100L*now/total;

	if (newpct == *pct)
		return;

	printf("\rSprite%5d  Done:%3d%%  \r", spriteno, newpct);
	*pct = newpct;
}

static int mergeset(FILE *grd, const char *grffile)
{
	static const char *action = "reading GRD";
	FILE *grf = NULL, *tmp = NULL;
	char grflen, *grfname;
	const char *c;
	char tempfile[16];
	U16 version, i, j, numsprites, spriteno, curno;
	int lastfrom = -2, lastto = lastfrom, lastpct = -1;
	long grfsize = 0;
	U32 dummy;
	int skipped = onlyshow;

	cfread(action, &version, 2, 1, grd);

	if (version > 1) die("This is a GRD file version %d, I don't know how to handle that.\n", version);

	cfread(action, &numsprites, 2, 1, grd);
	cfread(action, &grflen, 1, 1, grd);

	grfname = (char*) malloc(grflen + 4);		// +4 for .bak extension (safety margin)
	if (!grfname) die("Out of memory.\n");

	cfread(action, grfname, 1, grflen, grd);

	if (onlyshow) {
		printf("Generated from: %s.grf\nSprites in file: ", grfname);
	} else {
		if (grffile) {
			c=strrchr(grffile, '\\');
			if (!c) c=strchr(grffile, ':');
			if (!c)
				c=grffile;
			else
				c++;

			if (strrchr(c, '.'))
				j=strrchr(c, '.') - c;
			else
				j=strlen(c);

			if (strnicmp(c, grfname, j)) {
				printf("Warning, this GRD file was generated from %s.GRF.\n", grfname);
				printf("Are you sure you want to apply it to %s?", grffile);
				if (!yesno("Skipping file\n")) {
					for (i=0; i<numsprites; i++) {
						cfread(action, &spriteno, 2, 1, grd);
						skipsprite(grd);
					}
					return 1;
				};
			}
		} else {
			strcat(grfname, ".grf");
			grffile = grfname;
		}

		grf = fopen(grffile, "rb");
		if (!grf) {
			printf("Can't open %s: %s. File skipped.\n", grffile, strerror(errno));
			skipped = 1;
		}
	}
	if (!skipped) {
		fseek(grf, 0, SEEK_END);
		grfsize = ftell(grf);
		fseek(grf, 0, SEEK_SET);

		c=strchr(grffile, ':');
		if (c) {
			strncpy(tempfile, grffile, sizeof(tempfile)-1);
			i=c-grffile+1;
			if (i<sizeof(tempfile))
				tempfile[i]=0;
		} else
			tempfile[0] = 0;
		strncat(tempfile, TEMPFILE, sizeof(tempfile)-strlen(tempfile)-1);

		tmp = fopen(tempfile, "wb");
		if (!tmp) die("Can't open %s: %s\n", tempfile, strerror(errno));

		printf("Writing temporary file %s\n", tempfile);
	}

	curno = 0;
	for (i=0; i<numsprites; i++) {
		cfread(action, &spriteno, 2, 1, grd);

		if (skipped) {
			if (onlyshow) {
				if (spriteno != lastto+1) {
					if (lastfrom != lastto) printf("-%d", lastto);
					if (lastfrom >= 0) printf(", ");
					lastfrom = spriteno;
					printf("%d", lastfrom);
				}
				lastto = spriteno;
			}

			skipsprite(grd);
		} else {

			while (++curno <= spriteno) {
				copysprite(grf, tmp);
				showpct(ftell(grf), grfsize, curno, &lastpct);
			}

			skipsprite(grf);
			copysprite(grd, tmp);
			showpct(ftell(grf), grfsize, curno, &lastpct);
		}
	}

	if (onlyshow) {
		if (lastfrom < 0)
			printf("No sprites.");
		else if (lastfrom != lastto)
			printf("-%d", lastto);
		printf("\n");
	} else if (!skipped) {
		// copy remaining sprites, if any
		for (; copysprite(grf, tmp); )
			showpct(ftell(grf), grfsize, ++curno, &lastpct);
		showpct(grfsize, grfsize, curno, &lastpct);

		// write the dummy checksum
		dummy = 0;
		cfwrite("writing dummy checksum", &dummy, 4, 1, tmp);
	}

	if (tmp) fclose(tmp);
	if (grf) fclose(grf);

	if (!skipped) {
		char* c;

		printf("\nDone\n");

		// rename grf to bak if bak doesn't exist
		strcpy(block, grffile);
		c = strrchr(block, '.');
		if (!c) c = block + strlen(block);
		strcpy(c, ".bak");

		tmp = fopen(block, "rb");

		if (!tmp && (errno == ENOENT)) {
			// .bak doesn't exist, rename .grf to .bak
			printf("Renaming %s to %s\n", grffile, block);
			if (rename(grffile, block)) {
				printf("Error while renaming: %s\n", strerror(errno));
				printf("Shall I delete it instead?");
				if (!yesno("Aborted.\n")) {
					die("");
				}
				errno = EEXIST;		// go delete it
			} else {
				errno = ENOENT;		// don't try to delete it
			}
		}
		if (tmp || (errno != ENOENT)) {
			printf("Deleting %s\n", grffile);
			if (remove(grffile))
				die("Error while deleting: %s\n", strerror(errno));
		}

		// rename tmp to grf
		printf("Renaming %s to %s\n", tempfile, grffile);
		if (rename(tempfile, grffile))
			die("Error while renaming: %s\n", strerror(errno));

		printf("All done!\n");
	}

	free(grfname);

	return 1;
}

static int domerge(int argc, char **argv)
{
	FILE *grd;
	U32 magic;
	int first = 1;

	grd = fopen(grdfile, "rb");
	if (!grd) die("Can't open %s: %s\n", grdfile, strerror(errno));

	block = (char*) malloc(BLOCKSIZE);
	if (!block) die("Out of memory.\n");

	// read all sets of sprites
	while (1) {
		if (feof(grd))
			break;

		fseek(grd, grdofs, SEEK_SET);
		cfread("reading GRD", &magic, 4, 1, grd);

		if (magic != GRDmagic) {
			if (first)
				die("This is not a GRD file.\n");

			break;
		}

		first = 0;

		mergeset(grd, optind < argc ? argv[optind++] : NULL);

		grdofs = ftell(grd);

	};

	if (grd) fclose(grd);

	free(block);

	return 1;
}

int main(int argc, char **argv)
{
	char opt;

	printf("GRFMerge version " GRFCODECVER " - Copyright (C) 2003 by Josef Drexler\n");

#ifdef WIN32
	//  debugint();
#endif

	checkisselfextr(argv[0]);

	while (1) {
		opt = getopt(argc, argv, "hlyv");

		if (opt == (char) EOF)
			break;

		switch (opt) {
		case 'l':
			onlyshow = 1;
			break;
		case 'v':
			/* The version is already printed. */
			return 0;
		case 'y':
			alwaysyes = 1;
			break;
		default:
			usage();
		}
	}
	if (!issfx && (optind < argc) )	// no first arg if we're self-extracting
		grdfile = argv[optind++];

	if (grdfile)
		// see if the GRD file specified is a self-extracting executable
		// also sets grdofs to skip the .exe code
		checkisselfextr(grdfile);
	else {
		// no GRD file specified; see if we're a self-extracting executable
		if (issfx)
			grdfile = argv[0];
		else {
			printf("No GRD file specified!\n");
			exit(2);
		}
	}

	return domerge(argc, argv);
}
