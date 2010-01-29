/*
*  PgFincore
*  This project let you see what objects are in the FS cache memory
*  Copyright (C) 2009 Cédric Villemain
*/

/* { POSIX stuff */
#include <errno.h> /* errno */
#include <fcntl.h> /* fcntl, open */
#include <stdlib.h> /* exit, calloc, free */
#include <sys/stat.h> /* stat, fstat */
#include <sys/types.h> /* size_t, mincore */
#include <unistd.h> /* sysconf, close */
#include <sys/mman.h> /* mmap, mincore */
#define _XOPEN_SOURCE 600 /* fadvise */
#include <fcntl.h>  /* fadvise */
/* } */

/* { PostgreSQL stuff */
#include "postgres.h" /* general Postgres declarations */
#include "access/heapam.h" /* relation_open */
#include "catalog/catalog.h" /* relpath */
#include "catalog/namespace.h" /* makeRangeVarFromNameList */
#include "utils/builtins.h" /* textToQualifiedNameList */
#include "utils/rel.h" /* Relation */
#include "funcapi.h" /* SRF */
#include "catalog/pg_type.h" /* TEXTOID for tuple_desc */
#include "storage/fd.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif
/* } */

/*
*
* pgfincore_fctx structure is needed
* to keep track of relation path, segment number and action.
*
*/
typedef struct
{
  int action;				/* the action  mincore, fadvise...*/
  Relation rel;				/* the relation */
  unsigned int segcount;	/* the segment current number */
  char *relationpath;		/* the relation path */
} pgfincore_fctx;

Datum pgsysconf(PG_FUNCTION_ARGS);
Datum pgfincore(PG_FUNCTION_ARGS);
static Datum pgmincore_file(char *filename, int action, FunctionCallInfo fcinfo);
static Datum pgfadvise_file(char *filename, int action, FunctionCallInfo fcinfo);
static int pgfadv_snapshot(char *filename, int fd, int action);

/*
 * pgsysconf
 * just output the actual system value for
 * _SC_PAGESIZE     --> Page Size
 * _SC_AVPHYS_PAGES --> Free page in memory
 *
 */
PG_FUNCTION_INFO_V1(pgsysconf);
Datum
pgsysconf(PG_FUNCTION_ARGS)
{
  HeapTuple	tuple;
  TupleDesc tupdesc;
  Datum		values[2];
  bool		nulls[2];

  tupdesc = CreateTemplateTupleDesc(2, false);
  TupleDescInitEntry(tupdesc, (AttrNumber) 1, "block_size",  INT8OID, -1, 0);
  TupleDescInitEntry(tupdesc, (AttrNumber) 2, "block_free",  INT8OID, -1, 0);
  tupdesc = BlessTupleDesc(tupdesc);

  values[0] = Int64GetDatum(sysconf(_SC_PAGESIZE));  /* Page size */
  values[1] = Int64GetDatum(sysconf(_SC_AVPHYS_PAGES));  /* free page in memory */

  tuple = heap_form_tuple(tupdesc, values, nulls);
  elog(DEBUG1, "pgsysconf: page_size %ld bytes, free page in memory %ld", values[0], values[1]);
  PG_RETURN_DATUM( HeapTupleGetDatum(tuple) );
}

/*
*
* pgfincore is a function that handle the process to have a sharelock on the relation
* and to walk the segments.
* for each segment it call the appropriate function depending on 'action' parameter
*
*/
PG_FUNCTION_INFO_V1(pgfincore);
Datum
pgfincore(PG_FUNCTION_ARGS)
{
  FuncCallContext *funcctx;
  pgfincore_fctx  *fctx;
  Datum 		  result;
  char			  pathname[MAXPGPATH];
  bool 			  isnull;

  /* stuff done only on the first call of the function */
  if (SRF_IS_FIRSTCALL())
  {
	MemoryContext oldcontext;
	Oid			  relOid    = PG_GETARG_OID(0);
	text		  *forkName = PG_GETARG_TEXT_P(1);
	int			  action	= PG_GETARG_INT32(2);

	/* create a function context for cross-call persistence */
	funcctx = SRF_FIRSTCALL_INIT();

	/*
	 * switch to memory context appropriate for multiple function calls
	 */
	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

	/* allocate memory for user context */
	fctx = (pgfincore_fctx *) palloc(sizeof(pgfincore_fctx));

	/* open the current relation, accessShareLock */
	fctx->rel = relation_open(relOid, AccessShareLock);

	/* Because temp tables are not in the same directory, we failed, can be fixed  */
	if (fctx->rel->rd_istemp || fctx->rel->rd_islocaltemp)
	{
		relation_close(fctx->rel, AccessShareLock);
		elog(NOTICE,
			 "Table %s is a temporary table, actually pgfincore does not work on those relations.",
			 fctx->relationpath);
		pfree(fctx);
		SRF_RETURN_DONE(funcctx);
	}

	/* we get the common part of the filename of each segment of a relation */
	fctx->relationpath = relpath(fctx->rel->rd_node,
								 forkname_to_number( text_to_cstring(forkName) ));

	/* Here we keep track of current action in all calls */
	fctx->action = action;

	/* segcount is used to get the next segment of the current relation */
	fctx->segcount = 0;

	/* And finally we keep track of our initialization */
	elog(DEBUG1, "pgfincore: init done for %s", fctx->relationpath);
	funcctx->user_fctx = fctx;
	MemoryContextSwitchTo(oldcontext);
  }

  /* After the first call, we recover our context */
  funcctx = SRF_PERCALL_SETUP();
  fctx = funcctx->user_fctx;

  /* If we are still looking the first segment, relationpath should not be suffixed */
  if (fctx->segcount == 0)
	snprintf(pathname,
			 MAXPGPATH,
			 "%s",
			 fctx->relationpath);
  else
	snprintf(pathname,
			 MAXPGPATH,
			 "%s.%u",
			 fctx->relationpath,
			 fctx->segcount);

  elog(DEBUG1, "pgfincore: about to work with %s, current action : %d", pathname, fctx->action);

  /*
  * This function handle several sub-case by the action value switch
  */
  switch (fctx->action)
  {
	case 10 : /* MINCORE */
	case 11 : /* MINCORE with snapshot in a file */
	  result = pgmincore_file(pathname, fctx->action, fcinfo);
	break;
	case 20 : /* POSIX_FADV_WILLNEED */
	case 21 : /* POSIX_FADV_WILLNEED from snapshot file*/
	case 30 : /* POSIX_FADV_DONTNEED */
	case 40 : /* POSIX_FADV_NORMAL */
	case 50 : /* POSIX_FADV_SEQUENTIAL */
	case 60 : /* POSIX_FADV_RANDOM */
	  /* pgfadvise_file handle several flags, thanks to the same action value */
	  result = pgfadvise_file(pathname, fctx->action, fcinfo);
	break;
  }

  /*
  * When we have work with all segment of the current relation, test success
  * We exit from the SRF
  */
  if (DatumGetInt64(GetAttributeByName((HeapTupleHeader)result, "block_disk", &isnull)) == 0
	|| isnull )
  {
	relation_close(fctx->rel, AccessShareLock);
	pfree(fctx);
	elog(DEBUG1, "pgfincore: closing %s", fctx->relationpath);
	SRF_RETURN_DONE(funcctx);
  }

  /* prepare the number of the next segment */
  fctx->segcount++;

  /* Ok, return results, and go for next call */
  SRF_RETURN_NEXT(funcctx, result);
}

/*
 * pgmincore_file handle the mmaping, mincore process (and access file, etc.)
 */
static Datum
pgmincore_file(char *filename, int action, FunctionCallInfo fcinfo)
{
  HeapTuple	tuple;
  TupleDesc tupdesc;
  Datum		values[5];
  bool		nulls[5];
  int  flag=1;
  int64  block_disk = 0;
  int64  block_mem  = 0;
  int64  group_mem  = 0;

  // for open file
  int fd;
  // for stat file
  struct stat st;
  // for mmap file
  void *pa = (char *)0;
  // for calloc file
  unsigned char *vec = (unsigned char *)0;

  // OS things
  int64 pageSize  = sysconf(_SC_PAGESIZE); /* Page size */
  register int64 pageIndex;

  tupdesc = CreateTemplateTupleDesc(5, false);
  TupleDescInitEntry(tupdesc, (AttrNumber) 1, "relpath",    TEXTOID, -1, 0);
  TupleDescInitEntry(tupdesc, (AttrNumber) 2, "block_size", INT8OID, -1, 0);
  TupleDescInitEntry(tupdesc, (AttrNumber) 3, "block_disk", INT8OID, -1, 0);
  TupleDescInitEntry(tupdesc, (AttrNumber) 4, "block_mem",  INT8OID, -1, 0);
  TupleDescInitEntry(tupdesc, (AttrNumber) 5, "group_mem",  INT8OID, -1, 0);
  tupdesc = BlessTupleDesc(tupdesc);

/* Do the main work */
  /*
  * Open, fstat file
  */
  fd = open(filename, O_RDONLY);
  if (fd == -1)
    goto error;

  if (fstat(fd, &st) == -1)
  {
    close(fd);
    elog(ERROR, "Can not stat object file : %s",
		 filename);
    goto error;
  }

  /*
  * if file ok
  * then process
  */
  if (st.st_size != 0)
  {
	/* number of block in the current file */
	block_disk = st.st_size/pageSize;

	/* TODO We need to split mmap size to be sure (?) to be able to mmap */
	pa = mmap(NULL, st.st_size, PROT_NONE, MAP_SHARED, fd, 0);
	if (pa == MAP_FAILED) {
	  close(fd);
	  elog(ERROR, "Can not mmap object file : %s, errno = %i,%s\nThis error can happen if there is not enought space in memory to do the projection. Please mail cedric@villemain.org with '[pgfincore] ENOMEM' as subject.",
		  filename, errno, strerror(errno));
	  goto error;
	}

	/* Prepare our vector containing all blocks information */
	vec = calloc(1, (st.st_size+pageSize-1)/pageSize);
	if ((void *)0 == vec)
	{
	  munmap(pa, st.st_size);
	  close(fd);
	  elog(ERROR, "Can not calloc object file : %s",
		  filename);
	  goto error;
	}

	/* Affect vec with mincore */
	if (mincore(pa, st.st_size, vec) != 0)
	{
	  free(vec);
	  munmap(pa, st.st_size);
	  close(fd);
	  elog(ERROR, "mincore(%p, %ld, %p): %s\n",
			  pa, (int64)st.st_size, vec, strerror(errno));
	  goto error;
	}

	/* handle the results */
	for (pageIndex = 0; pageIndex <= st.st_size/pageSize; pageIndex++)
	{
	  // block in memory
	  if (vec[pageIndex] & 1)
	  {
		block_mem++;
		elog (DEBUG5, "in memory blocks : %ld / %ld",
			  pageIndex, block_disk);

		/* we flag to detect contigous blocks in the same state */
		if (flag)
		  group_mem++;
		flag = 0;
	  }
	  else
		flag=1;
	}
  }
  elog(DEBUG1, "pgfincore %s: %ld of %ld block in linux cache, %ld groups",
	   filename, block_mem,  block_disk, group_mem);

	/*
	* the data from mincore is fwrite to a file contigous to the relation file
	* in the PGDATA, suffix : _mincore
	* FIXME use some postgres internal for that ?
	*/
	if (action == 11)
	{
		char        path[MAXPGPATH];
	    FILE       *file;
		int64       count = 0;

		snprintf(path, sizeof(path), "%s_mincore", filename);
		file = AllocateFile(path, PG_BINARY_W);
		fwrite(block_mem, sizeof(block_mem), 1, file);
		count = fwrite(vec, 1, ((st.st_size+pageSize-1)/pageSize) , file);

		elog(DEBUG1, "writeStat count : %ld", count);

        if (count != ((st.st_size+pageSize-1)/pageSize))
            ereport(ERROR,
                    (errcode_for_file_access(),
                     errmsg("could not write file \"%s\"_mincore: %m", path)));
		FreeFile(file);
    }

  values[0] = CStringGetTextDatum(filename);
  values[1] = Int64GetDatum(pageSize);
  values[2] = Int64GetDatum(block_disk);
  values[3] = Int64GetDatum(block_mem);
  values[4] = Int64GetDatum(group_mem);

  memset(nulls, 0, sizeof(nulls));

  tuple = heap_form_tuple(tupdesc, values, nulls);

  //   free things
  free(vec);
  munmap(pa, st.st_size);
  close(fd);

  return HeapTupleGetDatum(tuple); /* return filename, block_disk, block_mem, group_mem   */

error:
  values[0] = CStringGetTextDatum(filename);
  values[1] = Int64GetDatum(false);
  values[2] = Int64GetDatum(false);
  values[3] = Int64GetDatum(false);
  values[4] = Int64GetDatum(false);
  memset(nulls, 0, sizeof(nulls));
  tuple = heap_form_tuple(tupdesc, values, nulls);
  return (HeapTupleGetDatum(tuple));
}

/*
 * pgfadvise_file
 */
static Datum
pgfadvise_file(char *filename, int action, FunctionCallInfo fcinfo)
{
  HeapTuple	tuple;
  TupleDesc tupdesc;
  Datum		values[4];
  bool		nulls[4];

  // for open file
  int fd;
  // for stat file
  struct stat st;

  // OS things
  int64 pageSize  = sysconf(_SC_PAGESIZE); /* Page size */

  tupdesc = CreateTemplateTupleDesc(4, false);
  TupleDescInitEntry(tupdesc, (AttrNumber) 1, "relpath",     TEXTOID, -1, 0);
  TupleDescInitEntry(tupdesc, (AttrNumber) 2, "block_size",  INT8OID, -1, 0);
  TupleDescInitEntry(tupdesc, (AttrNumber) 3, "block_disk",  INT8OID, -1, 0);
  TupleDescInitEntry(tupdesc, (AttrNumber) 4, "block_free",  INT8OID, -1, 0);
  tupdesc = BlessTupleDesc(tupdesc);

/* Do the main work */
  /* Open, fstat file
  *
  */
  fd = open(filename, O_RDONLY);

  if (fd == -1)
    goto error;

  if (fstat(fd, &st) == -1)
  {
    close(fd);
    elog(ERROR, "Can not stat object file : %s", filename);
    goto error;
  }

  /*
  * apply relevant function
  */
  switch (action)
  {
	case 20 : /* FADVISE_WILLNEED */
	  elog(DEBUG1, "pgfadv_willneed: setting flag");
	  posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
	break;

	case 21 : /* FADVISE_WILLNEED from mincore file */
	  elog(DEBUG1, "pgfadv_willneed: setting flag from file");
	  pgfadv_snapshot(filename, fd, action);
	break;

	case 30 : /* FADVISE_DONTNEED */
	  elog(DEBUG1, "pgfadv_dontneed: setting flag");
	  posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	break;

	case 40 : /* POSIX_FADV_NORMAL */
	  elog(DEBUG1, "pgfadv_normal: setting flag");
	  posix_fadvise(fd, 0, 0, POSIX_FADV_NORMAL);
	break;

	case 50 : /* POSIX_FADV_SEQUENTIAL */
	  elog(DEBUG1, "pgfadv_sequential: setting flag");
	  posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
	break;

	case 60 : /* POSIX_FADV_RANDOM */
	  elog(DEBUG1, "pgfadv_random: setting flag");
	  posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
	break;
  }

  values[0] = CStringGetTextDatum(filename);
  values[1] = Int64GetDatum(pageSize);
  values[2] = Int64GetDatum(st.st_size/pageSize);
  values[3] = Int64GetDatum( sysconf(_SC_AVPHYS_PAGES) ); /* free page cache */

  memset(nulls, 0, sizeof(nulls));

  tuple = heap_form_tuple(tupdesc, values, nulls);

  //   free things
  close(fd);

  return HeapTupleGetDatum(tuple);

error:
  values[0] = CStringGetTextDatum(filename);
  values[1] = Int64GetDatum(false);
  values[2] = Int64GetDatum(false);
  values[3] = Int64GetDatum(false);
  memset(nulls, 0, sizeof(nulls));
  tuple = heap_form_tuple(tupdesc, values, nulls);
  return (HeapTupleGetDatum(tuple));
}

/*
*
* pgfadv_snapshot to handle work with _mincore files.
* it is actually used for loading block from disk to buffer cache
*
*/
static int
pgfadv_snapshot(char *filename, int fd, int action)
{
  char        path[MAXPGPATH];
  FILE       *file;
  int blockNum = 0;
  int64 block_mem = 0;
  unsigned int c;
  unsigned int count = 0;
  /*
  * We handle the effective_io_concurrency...
  */
  unsigned int effective_io_concurrency = 1;

  // OS things
  int64 pageSize  = sysconf(_SC_PAGESIZE); /* Page size */

  switch (action)
  {
	case 21 : /* FADVISE_WILLNEED from mincore file */
	  /* Open _mincore file */
	  snprintf(path, sizeof(path), "%s_mincore", filename);
	  file = AllocateFile(path, PG_BINARY_R);
	  if (file == NULL)
	  {
		  if (errno == ENOENT)
			  return;				/* ignore not-found error */
		  goto error;
	  }

	  fread(block_mem, sizeof(block_mem), 1, file);
	  /* for each bit we read */
	  while ((c = fgetc(file)) != EOF)
	  {
		  blockNum++;

		  /*  Is this bit set ? */
		  if (c & 01)
		  {
			count++;

			/* We are going to claim as much blocks as effective_io_concurrency
			* and call once fadvise
			*/
			if (count == effective_io_concurrency)
			{
			  posix_fadvise(fd, ((blockNum-count)*pageSize), count*pageSize, POSIX_FADV_WILLNEED);
			  count=0;
			}
		  }
	  }

	  /* We perhaps have some remaining blocks to claim */
	  if (count)
		posix_fadvise(fd, ((blockNum-count)*pageSize), count*pageSize, POSIX_FADV_WILLNEED);

	  FreeFile(file);
	  elog(DEBUG1, "pgfadv_snapshot: loading %d blocks from relpath %s", block_mem, path);
	break;
  }

  return block_mem;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not read mincore file \"%s\": %m",
					path)));
	if (file)
		FreeFile(file);
	/* If possible, throw away the bogus file; ignore any error */
	unlink(path);
}
