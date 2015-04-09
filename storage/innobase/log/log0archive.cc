// TODO laurynas header

#include "log0archive.h"

#include "buf0buf.h" /* page_id_t */
#include "log0recv.h" /* recv_sys */
#include "os0file.h" /* os_file_status */

/* States of an archiving operation */
#define	LOG_ARCHIVE_READ	1
#define	LOG_ARCHIVE_WRITE	2

/* Pointer to this variable is used as the i/o-message when we do i/o to an
archive */
byte	log_archive_io;

/******************************************************//**
Pads the current log block full with dummy log records. Used in producing
consistent archived log files. */
static
void
log_pad_current_log_block(void)
/*===========================*/
{
	byte		b		= MLOG_DUMMY_RECORD;
	ulint		pad_length;
	ulint		i;
	lsn_t		lsn;

	ut_ad(log_mutex_own());
	/* We retrieve lsn only because otherwise gcc crashed on HP-UX */
	lsn = log_reserve_and_open(OS_FILE_LOG_BLOCK_SIZE);

	pad_length = OS_FILE_LOG_BLOCK_SIZE
		- (log_sys->buf_free % OS_FILE_LOG_BLOCK_SIZE)
		- LOG_BLOCK_TRL_SIZE;
	if (pad_length
	    == (OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE
		- LOG_BLOCK_TRL_SIZE)) {

		pad_length = 0;
	}

	for (i = 0; i < pad_length; i++) {
		log_write_low(&b, 1);
	}

	lsn = log_sys->lsn;

	log_close();
	log_mutex_exit();

	ut_a(lsn % OS_FILE_LOG_BLOCK_SIZE == LOG_BLOCK_HDR_SIZE);
}

/******************************************************//**
Generates an archived log file name. */

void
log_archived_file_name_gen(
/*=======================*/
	char*	buf,	/*!< in: buffer where to write */
	ulint	buf_len,/*!< in: buffer length */
	ulint	id __attribute__((unused)),
	/*!< in: group id;
	currently we only archive the first group */
	lsn_t	file_no)/*!< in: file number */
{
	ulint	dirnamelen;

	dirnamelen = strlen(srv_arch_dir);

	ut_a(buf_len > dirnamelen +
	     IB_ARCHIVED_LOGS_SERIAL_LEN +
	     IB_ARCHIVED_LOGS_PREFIX_LEN + 2);

	strcpy(buf, srv_arch_dir);

	if (buf[dirnamelen-1] != SRV_PATH_SEPARATOR) {
		buf[dirnamelen++] = SRV_PATH_SEPARATOR;
	}
	sprintf(buf + dirnamelen, IB_ARCHIVED_LOGS_PREFIX
		"%0" IB_TO_STR(IB_ARCHIVED_LOGS_SERIAL_LEN) "llu",
		(unsigned long long)file_no);
}

/******************************************************//**
Get offset within archived log file to continue to write
with. */

void
log_archived_get_offset(
/*=====================*/
	log_group_t*	group,		/*!< in: log group */
	lsn_t		file_no,	/*!< in: archive log file number */
	lsn_t		archived_lsn,	/*!< in: last archived LSN */
	lsn_t*		offset)		/*!< out: offset within archived file */
{
	char		file_name[OS_FILE_MAX_PATH];
	bool		exists;
	os_file_type_t	type;

	log_archived_file_name_gen(file_name,
				   sizeof(file_name), group->id, file_no);

	ut_a(os_file_status(file_name, &exists,	&type));

	if (!exists) {
		*offset = 0;
		return;
	}

	*offset = archived_lsn - file_no + LOG_FILE_HDR_SIZE;

	if (archived_lsn != LSN_MAX) {
		*offset = archived_lsn - file_no + LOG_FILE_HDR_SIZE;
	} else {
		/* Archiving was OFF prior startup */
		*offset = 0;
	}

	ut_a(group->file_size >= *offset + LOG_FILE_HDR_SIZE);

	return;
}

void
log_group_archive_file_header_write(
/*================================*/
	log_group_t*	group,		/*!< in: log group */
	ulint		nth_file,	/*!< in: header to the nth file in the
					archive log file space */
	lsn_t		file_no,	/*!< in: archived file number */
	ib_uint64_t	start_lsn)	/*!< in: log file data starts at this
					lsn */
{
	byte*	buf;
	ulint	dest_offset;

	ut_ad(log_mutex_own());

	ut_a(nth_file < group->n_files);

	buf = *(group->archive_file_header_bufs + nth_file);

	mach_write_to_4(buf + LOG_GROUP_ID, group->id);
	mach_write_to_8(buf + LOG_FILE_START_LSN, start_lsn);
	mach_write_to_4(buf + LOG_FILE_NO, file_no);

	mach_write_to_4(buf + LOG_FILE_ARCH_COMPLETED, FALSE);

	dest_offset = nth_file * group->file_size;

	log_sys->n_log_ios++;

	MONITOR_INC(MONITOR_LOG_IO);

	const ulint	page_no
		= (ulint) (dest_offset / univ_page_size.physical());

	fil_io(OS_FILE_WRITE | OS_FILE_LOG, true,
	       page_id_t(group->archive_space_id, page_no),
	       univ_page_size,
	       (ulint) (dest_offset % univ_page_size.physical()),
	       2 * OS_FILE_LOG_BLOCK_SIZE,
	       buf, &log_archive_io);
}

/******************************************************//**
Writes a log file header to a completed archived log file. */
static
void
log_group_archive_completed_header_write(
/*=====================================*/
	log_group_t*	group,		/*!< in: log group */
	ulint		nth_file,	/*!< in: header to the nth file in the
					archive log file space */
	ib_uint64_t	end_lsn)	/*!< in: end lsn of the file */
{
	byte*	buf;
	ulint	dest_offset;

	ut_ad(log_mutex_own());
	ut_a(nth_file < group->n_files);

	buf = *(group->archive_file_header_bufs + nth_file);

	mach_write_to_4(buf + LOG_FILE_ARCH_COMPLETED, TRUE);
	mach_write_to_8(buf + LOG_FILE_END_LSN, end_lsn);

	dest_offset = nth_file * group->file_size + LOG_FILE_ARCH_COMPLETED;

	log_sys->n_log_ios++;

	MONITOR_INC(MONITOR_LOG_IO);

	const ulint	page_no
		= (ulint) (dest_offset / univ_page_size.physical());

	fil_io(OS_FILE_WRITE | OS_FILE_LOG, true,
	       page_id_t(group->archive_space_id, page_no),
	       univ_page_size,
	       (ulint) (dest_offset % univ_page_size.physical()),
	       OS_FILE_LOG_BLOCK_SIZE,
	       buf + LOG_FILE_ARCH_COMPLETED,
	       &log_archive_io);
}

/******************************************************//**
							Does the archive writes for a single log group. */
static
void
log_group_archive(
/*==============*/
	log_group_t*    group)  /*!< in: log group */
{
	os_file_t       file_handle;
	lsn_t           start_lsn;
	lsn_t           end_lsn;
	char            name[OS_FILE_MAX_PATH];
	byte*           buf;
	ulint           len;
	bool		ret;
	lsn_t           next_offset;
	ulint           n_files;
	ulint           open_mode;

	ut_ad(log_mutex_own());

	start_lsn = log_sys->archived_lsn;

	ut_a(start_lsn % OS_FILE_LOG_BLOCK_SIZE == 0);

	end_lsn = log_sys->next_archived_lsn;

	ut_a(end_lsn % OS_FILE_LOG_BLOCK_SIZE == 0);

	buf = log_sys->archive_buf;

	n_files = 0;

	next_offset = group->archived_offset;
loop:
	if ((next_offset % group->file_size == 0)
	    || (fil_space_get_size(group->archive_space_id) == 0)) {

		/* Add the file to the archive file space; create or open the
		file */

		if (next_offset % group->file_size == 0) {
			open_mode = OS_FILE_CREATE;
			if (n_files == 0) {
				/* Adjust archived_file_no to match start_lsn
				which is written in file header as well */
				group->archived_file_no = start_lsn;
			}
		} else {
			open_mode = OS_FILE_OPEN;
		}

		log_archived_file_name_gen(name, sizeof(name), group->id,
					   group->archived_file_no +
					   n_files * (group->file_size -
						      LOG_FILE_HDR_SIZE));

		file_handle = os_file_create(innodb_log_file_key,
					     name, open_mode,
					     OS_FILE_AIO,
					     OS_DATA_FILE, srv_read_only_mode,
					     &ret);

		if (!ret && (open_mode == OS_FILE_CREATE)) {
			file_handle = os_file_create(
				innodb_log_file_key, name, OS_FILE_OPEN,
				OS_FILE_AIO, OS_DATA_FILE, srv_read_only_mode,
				&ret);
		}

		if (!ret) {
			fprintf(stderr,
				"InnoDB: Cannot create or open"
				" archive log file %s.\n"
				"InnoDB: Cannot continue operation.\n"
				"InnoDB: Check that the log archive"
				" directory exists,\n"
				"InnoDB: you have access rights to it, and\n"
				"InnoDB: there is space available.\n", name);
			exit(1);
		}

		DBUG_PRINT("ib_log", ("Created archive file %s", name));

		ret = os_file_close(file_handle);

		ut_a(ret);

		/* Add the archive file as a node to the space */
		fil_space_t*	archive_space
			= fil_tablespace_exists_in_mem(group->archive_space_id);
		ut_ad(archive_space); // TODO laurynas

		ut_a(fil_node_create(name, group->file_size / UNIV_PAGE_SIZE,
				     archive_space, false));

		if (next_offset % group->file_size == 0) {
			log_group_archive_file_header_write(
				group, n_files,
				group->archived_file_no +
				n_files * (group->file_size - LOG_FILE_HDR_SIZE),
				start_lsn);

			next_offset += LOG_FILE_HDR_SIZE;
		}
	}

	len = end_lsn - start_lsn;

	if (group->file_size < (next_offset % group->file_size) + len) {

		len = group->file_size - (next_offset % group->file_size);
	}

	DBUG_PRINT("ib_log", ("Archiving starting at lsn " LSN_PF ", len %lu"
			      " to group %lu\n",
			      start_lsn,
			      (ulong) len, (ulong) group->id));

	log_sys->n_pending_archive_ios++;

	log_sys->n_log_ios++;

	MONITOR_INC(MONITOR_LOG_IO);

	const ulint	page_no
		= (ulint) (next_offset / univ_page_size.physical());

	fil_io(OS_FILE_WRITE | OS_FILE_LOG, false,
	       page_id_t(group->archive_space_id, page_no),
	       univ_page_size,
	       (ulint) (next_offset % UNIV_PAGE_SIZE),
	       ut_calc_align(len, OS_FILE_LOG_BLOCK_SIZE), buf,
	       &log_archive_io);

	start_lsn += len;
	next_offset += len;
	buf += len;

	if (next_offset % group->file_size == 0) {
		n_files++;
	}

	if (end_lsn != start_lsn) {

		goto loop;
	}

	group->next_archived_file_no = group->archived_file_no +
		n_files * (group->file_size - LOG_FILE_HDR_SIZE);
	group->next_archived_offset = next_offset % group->file_size;

	ut_a(group->next_archived_offset % OS_FILE_LOG_BLOCK_SIZE == 0);
}

/*****************************************************//**
						       (Writes to the archive of each log group.) Currently, only the first
						       group is archived. */
static
void
log_archive_groups(void)
/*====================*/
{
	log_group_t*	group;

	ut_ad(log_mutex_own());

	group = UT_LIST_GET_FIRST(log_sys->log_groups);

	log_group_archive(group);
}

/*****************************************************//**
						       Completes the archiving write phase for (each log group), currently,
						       the first log group. */
static
void
log_archive_write_complete_groups(void)
/*===================================*/
{
	log_group_t*	group;
	lsn_t		end_offset;
	ulint		trunc_files;
	ulint		n_files;
	ib_uint64_t	start_lsn;
	ib_uint64_t	end_lsn;
	ulint		i;

	ut_ad(log_mutex_own());

	group = UT_LIST_GET_FIRST(log_sys->log_groups);

	group->archived_file_no = group->next_archived_file_no;
	group->archived_offset = group->next_archived_offset;

	/* Truncate from the archive file space all but the last
	file, or if it has been written full, all files */

	n_files = (UNIV_PAGE_SIZE
		   * fil_space_get_size(group->archive_space_id))
		/ group->file_size;
	ut_ad(n_files > 0);

	end_offset = group->archived_offset;

	if (end_offset % group->file_size == 0) {

		trunc_files = n_files;
	} else {
		trunc_files = n_files - 1;
	}

	DBUG_PRINT("ib_log", ("Complete file(s) archived to group %lu",
			      (ulong) group->id));

	/* Calculate the archive file space start lsn */
	start_lsn = log_sys->next_archived_lsn
		- (end_offset - LOG_FILE_HDR_SIZE + trunc_files
		   * (group->file_size - LOG_FILE_HDR_SIZE));
	end_lsn = start_lsn;

	for (i = 0; i < trunc_files; i++) {

		end_lsn += group->file_size - LOG_FILE_HDR_SIZE;

		/* Write a notice to the headers of archived log
		files that the file write has been completed */

		log_group_archive_completed_header_write(group, i, end_lsn);
	}

	fil_space_truncate_start(group->archive_space_id,
				 trunc_files * group->file_size);

	DBUG_PRINT("ib_log", ("Archiving writes completed"));
}

/******************************************************//**
							Completes an archiving i/o. */
static
void
log_archive_check_completion_low(void)
/*==================================*/
{
	ut_ad(log_mutex_own());

	if (log_sys->n_pending_archive_ios == 0
	    && log_sys->archiving_phase == LOG_ARCHIVE_READ) {

		DBUG_PRINT("ib_log", ("Archiving read completed"));

		/* Archive buffer has now been read in: start archive writes */

		log_sys->archiving_phase = LOG_ARCHIVE_WRITE;

		log_archive_groups();
	}

	if (log_sys->n_pending_archive_ios == 0
	    && log_sys->archiving_phase == LOG_ARCHIVE_WRITE) {

		log_archive_write_complete_groups();

		log_sys->archived_lsn = log_sys->next_archived_lsn;

		rw_lock_x_unlock_gen(&(log_sys->archive_lock), LOG_ARCHIVE);
	}
}

/******************************************************//**
Completes an archiving i/o. */

void
log_io_complete_archive(void)
/*=========================*/
{
	log_group_t*	group;

	log_mutex_enter();

	group = UT_LIST_GET_FIRST(log_sys->log_groups);

	log_mutex_exit();

	fil_flush(group->archive_space_id);

	log_mutex_enter();

	ut_ad(log_sys->n_pending_archive_ios > 0);

	log_sys->n_pending_archive_ios--;

	log_archive_check_completion_low();

	log_mutex_exit();
}

/********************************************************************//**
Starts an archiving operation.
@return	TRUE if succeed, FALSE if an archiving operation was already running */

ibool
log_archive_do(
/*===========*/
	ibool	sync,	/*!< in: TRUE if synchronous operation is desired */
	ulint*	n_bytes)/*!< out: archive log buffer size, 0 if nothing to
			archive */
{
	ibool   calc_new_limit;
	lsn_t	start_lsn;
	lsn_t	limit_lsn	= LSN_MAX;

	calc_new_limit = TRUE;
loop:
	log_mutex_enter();

	switch (log_sys->archiving_state) {
	case LOG_ARCH_OFF:
	arch_none:
		log_mutex_exit();

		*n_bytes = 0;

		return(TRUE);
	case LOG_ARCH_STOPPED:
	case LOG_ARCH_STOPPING2:
		log_mutex_exit();

		os_event_wait(log_sys->archiving_on);

		goto loop;
	}

	start_lsn = log_sys->archived_lsn;

	if (calc_new_limit) {
		ut_a(log_sys->archive_buf_size % OS_FILE_LOG_BLOCK_SIZE == 0);
		limit_lsn = start_lsn + log_sys->archive_buf_size;

		*n_bytes = log_sys->archive_buf_size;

		if (limit_lsn >= log_sys->lsn) {

			limit_lsn = ut_uint64_align_down(
				log_sys->lsn, OS_FILE_LOG_BLOCK_SIZE);
		}
	}

	if (log_sys->archived_lsn >= limit_lsn) {

		goto arch_none;
	}

#if 0 // TODO laurynas
	if (log_sys->written_to_all_lsn < limit_lsn) {

		log_mutex_exit();

		log_write_up_to(limit_lsn, LOG_WAIT_ALL_GROUPS, TRUE);

		calc_new_limit = FALSE;

		goto loop;
	}
#endif

	if (log_sys->n_pending_archive_ios > 0) {
		/* An archiving operation is running */

		log_mutex_exit();

		if (sync) {
			rw_lock_s_lock(&(log_sys->archive_lock));
			rw_lock_s_unlock(&(log_sys->archive_lock));
		}

		*n_bytes = log_sys->archive_buf_size;

		return(FALSE);
	}

	rw_lock_x_lock_gen(&(log_sys->archive_lock), LOG_ARCHIVE);

	log_sys->archiving_phase = LOG_ARCHIVE_READ;

	log_sys->next_archived_lsn = limit_lsn;

	DBUG_PRINT("ib_log", ("Archiving from lsn " LSN_PF " to lsn "
			      LSN_PF "\n",
			      log_sys->archived_lsn, limit_lsn));

	/* Read the log segment to the archive buffer */

	log_group_read_log_seg(LOG_ARCHIVE, log_sys->archive_buf,
			       UT_LIST_GET_FIRST(log_sys->log_groups),
			       start_lsn, limit_lsn, false);

	log_mutex_exit();

	if (sync) {
		rw_lock_s_lock(&(log_sys->archive_lock));
		rw_lock_s_unlock(&(log_sys->archive_lock));
	}

	*n_bytes = log_sys->archive_buf_size;

	return(TRUE);
}

/****************************************************************//**
Writes the log contents to the archive at least up to the lsn when this
function was called. */
static
void
log_archive_all(void)
/*=================*/
{
	lsn_t	present_lsn;

	log_mutex_enter();

	if (log_sys->archiving_state == LOG_ARCH_OFF) {
		log_mutex_exit();

		return;
	}

	present_lsn = log_sys->lsn;

	log_pad_current_log_block();

	for (;;) {

		ulint	archived_bytes;

		log_mutex_enter();

		if (present_lsn <= log_sys->archived_lsn) {

			log_mutex_exit();

			return;
		}

		log_mutex_exit();

		log_archive_do(TRUE, &archived_bytes);

		if (archived_bytes == 0)
			return;
	}
}

/*****************************************************//**
Closes the possible open archive log file (for each group) the first group,
and if it was open, increments the group file count by 2, if desired. */

void
log_archive_close_groups(
/*=====================*/
	bool	increment_file_count)	/*!< in: true if we want to increment
					the file count */
{
	log_group_t*	group;
	ulint		trunc_len;

	ut_ad(log_mutex_own());

	if (log_sys->archiving_state == LOG_ARCH_OFF) {

		return;
	}

	group = UT_LIST_GET_FIRST(log_sys->log_groups);

	trunc_len = UNIV_PAGE_SIZE
		* fil_space_get_size(group->archive_space_id);
	if (trunc_len > 0) {
		ut_a(trunc_len == group->file_size);

		/* Write a notice to the headers of archived log
		files that the file write has been completed */

		log_group_archive_completed_header_write(
			group, 0, log_sys->archived_lsn);

		fil_space_truncate_start(group->archive_space_id,
					 trunc_len);
		if (increment_file_count) {
			group->archived_offset = 0;
		}

	}
}

/****************************************************************//**
Writes the log contents to the archive up to the lsn when this function was
called, and stops the archiving. When archiving is started again, the archived
log file numbers start from 2 higher, so that the archiving will not write
again to the archived log files which exist when this function returns.
@return	DB_SUCCESS or DB_ERROR */
static
ulint
log_archive_stop(void)
/*==================*/
{
	ibool	success;

	log_mutex_enter();

	if (log_sys->archiving_state != LOG_ARCH_ON) {

		log_mutex_exit();

		return(DB_ERROR);
	}

	log_sys->archiving_state = LOG_ARCH_STOPPING;

	log_mutex_exit();

	log_archive_all();

	log_mutex_enter();

	log_sys->archiving_state = LOG_ARCH_STOPPING2;
	os_event_reset(log_sys->archiving_on);

	log_mutex_exit();

	/* Wait for a possible archiving operation to end */

	rw_lock_s_lock(&(log_sys->archive_lock));
	rw_lock_s_unlock(&(log_sys->archive_lock));

	log_mutex_enter();

	/* Close all archived log files, incrementing the file count by 2,
	if appropriate */

	log_archive_close_groups(TRUE);

	log_mutex_exit();

	/* Make a checkpoint, so that if recovery is needed, the file numbers
	of new archived log files will start from the right value */

	success = FALSE;

	while (!success) {
		success = log_checkpoint(TRUE, TRUE);
	}

	log_mutex_enter();

	log_sys->archiving_state = LOG_ARCH_STOPPED;

	log_mutex_exit();

	return(DB_SUCCESS);
}

/****************************************************************//**
Stop archiving the log so that a gap may occur in the archived log files.
@return	DB_SUCCESS or DB_ERROR */

ulint
log_archive_noarchivelog(void)
/*==========================*/
{
loop:
	log_mutex_enter();

	if (log_sys->archiving_state == LOG_ARCH_STOPPED
	    || log_sys->archiving_state == LOG_ARCH_OFF) {

		log_sys->archiving_state = LOG_ARCH_OFF;

		os_event_set(log_sys->archiving_on);

		log_mutex_exit();

		return(DB_SUCCESS);
	}

	log_mutex_exit();

	log_archive_stop();

	os_thread_sleep(500000);

	goto loop;
}

/****************************************************************//**
Start archiving the log so that a gap may occur in the archived log files.
@return	DB_SUCCESS or DB_ERROR */

ulint
log_archive_archivelog(void)
/*========================*/
{
	log_mutex_enter();

	if (log_sys->archiving_state == LOG_ARCH_OFF) {

		log_sys->archiving_state = LOG_ARCH_ON;

		log_sys->archived_lsn
			= ut_uint64_align_down(log_sys->lsn,
					       OS_FILE_LOG_BLOCK_SIZE);
		log_mutex_exit();

		return(DB_SUCCESS);
	}

	log_mutex_exit();

	return(DB_ERROR);
}

/****************************************************************//**
Tries to establish a big enough margin of free space in the log groups, such
that a new log entry can be catenated without an immediate need for
archiving. */

void
log_archive_margin(void)
/*====================*/
{
	log_t*	log		= log_sys;
	ulint	age;
	ibool	sync;
	ulint	dummy;
loop:
	log_mutex_enter();

	if (log->archiving_state == LOG_ARCH_OFF) {
		log_mutex_exit();

		return;
	}

	age = log->lsn - log->archived_lsn;

	if (age > log->max_archived_lsn_age) {

		/* An archiving is urgent: we have to do synchronous i/o */

		sync = TRUE;

	} else if (age > log->max_archived_lsn_age_async) {

		/* An archiving is not urgent: we do asynchronous i/o */

		sync = FALSE;
	} else {
		/* No archiving required yet */

		log_mutex_exit();

		return;
	}

	log_mutex_exit();

	log_archive_do(sync, &dummy);

	if (sync == TRUE) {
		/* Check again that enough was written to the archive */

		goto loop;
	}
}
