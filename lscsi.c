/* lscsi.c
 *
 * Interpret the arbitrary sp_ SCSI pass thru of "spscsi.h"
 * via SG_IO since <scsi/sg.h> version 3 i.e. since Linux kernel 2.4.
 *
 * Bugs include:
 *
 *	Suppose auto sense occurred if ioctl SG_IO returns nonnegative.
 *	Do not distinguish ioctl failed from auto sense unintelligible.
 *	Pass thru the xFF limits on CDB length and sense length.
 *	Pass thru the data per cdb limit of one contiguous virtual allocation.
 *
 * Simplifications include:
 *
 *	Require caller to avoid null pointers and negative lengths.
 *	Require caller to mis/align struct, cdb, sense, and data.
 *	Never pass thru -1 SG_DXFER_NONE with nonnull dxferp.
 *	Never pass thru -4 SG_DXFER_TO_FROM_DEV.
 *	Never pass thru -5 SG_DXFER_UNKNOWN.
 *
 *	Declare struct sp only outside this file (in "spscsi.h").
 *	List declarations via: grep $'^[^/ *\x09{}]'
 *	Conform to http://lxr.linux.no/source/Documentation/CodingStyle
 */

/* Link with standard C libraries. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Link with local C libraries. */

#include <fcntl.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Link with ../ccscsi/. */

#include "xscsi.h"

struct sp
{
	sg_io_hdr_t sih;
	int fd;
	char sense[16384];
};

size_t sizeof_struct_sp(void)
{
	return sizeof (struct sp);
}


#define USUAL_SENSE 0x12 /* x12 Win XP/2K, x0E Win ME/9X */
#define USUAL_SECONDS (28 * 60 * 60) /* 28 hours = more than a day */

/* Exit after printing why. */

static void exits(char const * st, char const * file, int line)
{
	if (*st != '\0') {
		fprintf(stderr, "%s: ", st);
	}
	fprintf(stderr, "file %s line %d\n", file, line);
	exit(-line);
}

/* Exit after printing errno. */

static void exite(char const * st, char const * file, int line)
{
	int en = errno;
	perror(st);
	fprintf(stderr, "errno %d at file %s line %d\n", en, file, line);
	exit(-line);
}

/* Disconnect. */

void sp_close(struct sp * sp)
{
	int i = close(sp->fd);
	if (i != 0) exite("close", __FILE__, __LINE__);
	sp->fd = -1;
}

/* Begin a new command, but stay connected. */

void sp_zero(struct sp * sp)
{
	sg_io_hdr_t * sih = &sp->sih;
	memset(sih, '\0', sizeof *sih);
	sih->interface_id = 'S';
	sih->dxfer_direction = SG_DXFER_NONE; /* often -1 */
	sp_sense(sp, &sp->sense[0], USUAL_SENSE);
	sp_late(sp, USUAL_SECONDS, 0);
}

/* Connect and return nonnegative, else decide errno. */

int sp_open(struct sp * sp, char const * name)
{
	int mode = (O_RDONLY | O_NONBLOCK);
	int fd = open(name, mode);
	sp_zero(sp);
	sp->fd = fd;
	if (0 <= fd) {
		int version = 0;
		int i = ioctl(fd, SG_GET_VERSION_NUM, &version);
		if (0 <= i) {
			if (30000 <= version) {
//				fprintf(stderr, "%d\n", fd);
				return fd;
			}
		}
		sp_close(sp);
		errno = EINVAL;
	}
	return -1;
}

/* Hint from where and how much CDB to copy out, return where. */

char * sp_cdb(struct sp * sp, char * cdb, int max)
{
	sg_io_hdr_t * sih = &sp->sih;
	unsigned char uch = ((unsigned char) max);
	if (uch != max) exits("large", __FILE__, __LINE__);
	sih->cmd_len = uch;
	sih->cmdp = cdb;
	return sih->cmdp;
}

/* Hint where and how much data to copy in or out, return where. */

char * sp_data(struct sp * sp, char * data, int max)
{
	sg_io_hdr_t * sih = &sp->sih;
	sih->dxfer_len = max;
	sih->dxferp = data;
	return sih->dxferp;
}

/* Hint to where and how much sense to copy in, return where. */

char * sp_sense(struct sp * sp, char * sense, int max)
{
	sg_io_hdr_t * sih = &sp->sih;
	unsigned char uch = ((unsigned char) max);
	if (uch != max) exits("large", __FILE__, __LINE__);
	sih->mx_sb_len = uch;
	sih->sbp = sense;
	return sih->sbp;
}

/* Hint when to time out and reset, return ns. */

int sp_late(struct sp * sp, int s, int ns)
{
	sg_io_hdr_t * sih = &sp->sih;
	int ms = ((s * 1000) + ((ns + 999999) / 1000 / 1000));
	if ((ms / 1000) != s) exits("large", __FILE__, __LINE__);
	sih->timeout = ms;
	return ((sih->timeout % 1000) * 1000 * 1000);
}

/* Zero all but the least significant set bit of a mask. */

static int lsb(int mask)
{
	return (mask & -mask);
}

/* Construct an sp_read/ sp_write exit int from bits of auto sense. */

static int int_from_sense(char const * chars, int length)
{
	int exit_int = SP_THRU; /* unintelligible sense */

	/* Require minimal sense. */

	if (2 < length) {
		int sk = (chars[2] & 0x0F);
		int response_code = (chars[0] & 0x7F);
		if ((response_code == 0x70) || (response_code == 0x71)) {
			exit_int |= SP_SENSE; /* intelligible sense */

			/* Distinguish x70 Current vs. other sense. */

			if (response_code != 0x70) {
				exit_int |= SP_DEFERRED;
			}

			/* Pass back SK. */

			exit_int |= (sk * lsb(SP_SK));

			/* Interpret additional length, not quite like t10. */

			if (7 < length) {
				int al = (chars[7] & 0xFF);
				if (al != 0x00) {
					int max = (7 + 1 + al);
					if (max < length) {
						length = max;
					}
				}
			}

			/* Pass back ASC and ASCQ. */

			if (0xC < length) {
				int asc = (chars[0xC] & 0xFF);
				exit_int |= (asc * lsb(SP_ASC));
			}
			if (0xD < length) {
				int ascq = (chars[0xD] & 0xFF);
				exit_int |= (ascq * lsb(SP_ASCQ));
			}
		}
	}
	return exit_int;
}

/* Pass thru.  Return zero else positive residue else negative trouble. */

int sp_speak(struct sp * sp)
{
	int fd = sp->fd;
	sg_io_hdr_t * sih = &sp->sih;
	int max = sih->dxfer_len;
	char const * sense_chars = sih->sbp;
	int sense_max = sih->mx_sb_len;
	int i;
	int residue;
	int sense_enough;
	int exit_int;

	/* Trust caller to have decided much. */

	; /* sih->interface_id */
	; /* sih->cmdp sih->cmd_len sih->sbp sih->mx_sb_len sih->timeout */
	; /* sih->dxfer_direction sih->dxferp sih->dxfer_len */

	/* Trust caller to have zeroed much. */

	; /* sih->iovec_count sih->flags sih->pack_id sih->usr_ptr */
	; /* sih->status sih->masked_status sih->msg_status */
	; /* sih->sb_len_wr sih->host_status sih->drive_status */
	; /* sih->resid sih->duration sih->info */

	/* Trace. */

#if 0
	fprintf(stderr, "%d '%c' %d %dms\n",
		fd, sih->interface_id, sih->dxfer_direction, sih->timeout);
	fprintf(stderr, "x %X %X %X\n",
		sih->cmd_len, sih->mx_sb_len, sih->dxfer_len);
#endif

	/* Speak. */

	i = ioctl(fd, SG_IO, sih);
	residue = sih->resid;
	sense_enough = sih->sb_len_wr;

	/* Compress much into the exit int. */

	exit_int = residue; /* zero if ok else positive residue */

	if (i < 0) {
		exit_int = SP_THRU; /* ioctl failed */
	} else if ((residue < 0) || (max < residue)) {
		exit_int = SP_THRU;
		exit_int |= SP_DATA_THRU; /* data not counted */
	} else if ((sih->info & SG_INFO_OK_MASK) != SG_INFO_OK) {
		if ((sense_enough < 0) || (sense_max < sense_enough)) {
			exit_int = SP_THRU;
			exit_int |= SP_SENSE_THRU; /* sense not counted */
		} else {
			exit_int = SP_THRU;
			exit_int |= int_from_sense(sense_chars, sense_enough);
			if (0 != residue) {
				exit_int |= SP_RESIDUE;
			}
		}
	}

	return exit_int;
}

/* Pass thru and copy zero or more bytes of data in. */

int sp_read(struct sp * sp, char * to, int max)
{
	sg_io_hdr_t * sih = &sp->sih;
	sp_data(sp, to, max);
	sih->dxfer_direction = SG_DXFER_FROM_DEV; /* often -3 */
	return sp_speak(sp);
}

/* Pass thru and copy zero or more bytes of data out. */

int sp_write(struct sp * sp, char const * from, int max)
{
	sg_io_hdr_t * sih = &sp->sih;
	sp_data(sp, (char *) from, max);
	sih->dxfer_direction = SG_DXFER_TO_DEV; /* often -2 */
	return sp_speak(sp);
}

/* Get the last length of data copied in. */

int sp_data_enough(struct sp * sp)
{
	sg_io_hdr_t * sih = &sp->sih;
	return (sih->dxfer_len - sih->resid);
}

/* Get the last length of sense copied in. */

int sp_sense_enough(struct sp * sp)
{
	sg_io_hdr_t * sih = &sp->sih;
	return sih->sb_len_wr;
}

/* end of file */
