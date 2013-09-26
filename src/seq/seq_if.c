/*************************************************************************\
Copyright (c) 1991-1994 The Regents of the University of California
                        and the University of Chicago.
                        Los Alamos National Laboratory
Copyright (c) 2010-2012 Helmholtz-Zentrum Berlin f. Materialien
                        und Energie GmbH, Germany (HZB)
This file is distributed subject to a Software License Agreement found
in the file LICENSE that is included with this distribution.
\*************************************************************************/
/*	Interface functions from state program to run-time sequencer.
 *
 *	Author:  Andy Kozubal
 *	Date:    1 March, 1994
 *
 *	Experimental Physics and Industrial Control System (EPICS)
 *
 *	This software was produced under  U.S. Government contracts:
 *	(W-7405-ENG-36) at the Los Alamos National Laboratory,
 *	and (W-31-109-ENG-38) at Argonne National Laboratory.
 *
 *	Initial development by:
 *	  The Controls and Automation Group (AT-8)
 *	  Ground Test Accelerator
 *	  Accelerator Technology Division
 *	  Los Alamos National Laboratory
 *
 *	Co-developed with
 *	  The Controls and Computing Group
 *	  Accelerator Systems Division
 *	  Advanced Photon Source
 *	  Argonne National Laboratory
 */
#include "seq.h"
#include "seq_debug.h"

static pvStat check_connected(DBCHAN *dbch, PVMETA *meta)
{
	if (!dbch->connected)
	{
		meta->status = pvStatDISCONN;
		meta->severity = pvSevrINVALID;
		meta->message = "disconnected";
		return meta->status;
	}
	else
	{
		return pvStatOK;
	}
}

/*
 * Get value from a channel.
 * TODO: add optional timeout argument.
 */
epicsShareFunc pvStat seq_pvGet(SS_ID ss, VAR_ID varId, enum compType compType, double tmo)
{
	PROG		*sp = ss->prog;
	CHAN		*ch = sp->chan + varId;
	pvStat		status;
	PVREQ		*req;
	epicsEventId	getSem = ss->getSem[varId];
	DBCHAN		*dbch = ch->dbch;
	PVMETA		*meta = metaPtr(ch,ss);

	/* Anonymous PV and safe mode, just copy from shared buffer.
	   Note that completion is always immediate, so no distinction
	   between SYNC and ASYNC needed. See also pvGetComplete. */
	if (optTest(sp, OPT_SAFE) && !dbch)
	{
		/* Copy regardless of whether dirty flag is set or not */
		ss_read_buffer(ss, ch, FALSE);
		return pvStatOK;
	}
	/* No named PV and traditional mode => user error */
	if (!dbch)
	{
		errlogSevPrintf(errlogMajor,
			"pvGet(%s): user error (variable not assigned)\n",
			ch->varName
		);
		return pvStatERROR;
	}

	if (compType == DEFAULT)
	{
		compType = optTest(sp, OPT_ASYNC) ? ASYNC : SYNC;
	}

	if (compType == SYNC)
	{
		double before, after;
		pvTimeGetCurrentDouble(&before);
		if (tmo <= 0.0)
		{
			errlogSevPrintf(errlogMajor,
				"pvGet(%s,SYNC,%f): user error (timeout must be positive)\n",
				ch->varName, tmo);
			return pvStatERROR;
		}
		switch (epicsEventWaitWithTimeout(getSem, tmo))
		{
		case epicsEventWaitOK:
			status = check_connected(dbch, meta);
			if (status != pvStatOK) return epicsEventSignal(getSem), status;
			pvTimeGetCurrentDouble(&after);
			tmo -= (after - before);
			if (tmo <= 0.0)
				tmo = 0.001;
			break;
		case epicsEventWaitTimeout:
			errlogSevPrintf(errlogMajor,
				"pvGet(ss %s, var %s, pv %s): failed (timeout "
				"waiting for other get requests to finish)\n",
				ss->ssName, ch->varName, dbch->dbName
			);
			return pvStatERROR;
		case epicsEventWaitError:
			/* try to recover */
			ss->getReq[varId] = NULL;
			epicsEventSignal(getSem);
			errlogSevPrintf(errlogFatal,
				"pvGet: epicsEventWaitWithTimeout() failure\n");
			return pvStatERROR;
		}
	}
	else if (compType == ASYNC)
	{
		switch (epicsEventTryWait(getSem))
		{
		case epicsEventWaitOK:
			if (ss->getReq[varId] != NULL)
			{
				/* previous request timed out but user
				   did not call pvGetComplete */
				ss->getReq[varId] = NULL;
			}
			status = check_connected(dbch, meta);
			if (status != pvStatOK) return epicsEventSignal(getSem), status;
			break;
		case epicsEventWaitTimeout:
			errlogSevPrintf(errlogMajor,
				"pvGet(ss %s, var %s, pv %s): user error "
				"(there is already a get pending for this variable/"
				"state set combination)\n",
				ss->ssName, ch->varName, dbch->dbName
			);
			return pvStatERROR;
		case epicsEventWaitError:
			/* try to recover */
			ss->getReq[varId] = NULL;
			epicsEventSignal(getSem);
			errlogSevPrintf(errlogFatal,
				"pvGet: epicsEventTryWait() failure\n");
			return pvStatERROR;
		}
	}

	/* Allocate and initialize a pv request */
	req = (PVREQ *)freeListMalloc(sp->pvReqPool);
	req->ss = ss;
	req->ch = ch;

	assert(ss->getReq[varId] == NULL);
	ss->getReq[varId] = req;

	/* Perform the PV get operation with a callback routine specified.
	   Requesting more than db channel has available is ok. */
	status = pvVarGetCallback(
			&dbch->pvid,		/* PV id */
			ch->type->getType,	/* request type */
			ch->count,		/* element count */
			req);			/* user arg */
	if (status != pvStatOK)
	{
		meta->status = pvStatERROR;
		meta->severity = pvSevrMAJOR;
		meta->message = "get failure";
		errlogSevPrintf(errlogFatal, "pvGet(var %s, pv %s): pvVarGetCallback() failure: %s\n",
			ch->varName, dbch->dbName, pvVarGetMess(dbch->pvid));
		ss->getReq[varId] = NULL;
		freeListFree(sp->pvReqPool, req);
		epicsEventSignal(getSem);
		check_connected(dbch, meta);
		return status;
	}

	/* Synchronous: wait for completion */
	if (compType == SYNC)
	{
		epicsEventWaitStatus event_status;

		pvSysFlush(sp->pvSys);
		event_status = epicsEventWaitWithTimeout(getSem, tmo);
		ss->getReq[varId] = NULL;
		epicsEventSignal(getSem);
		switch (event_status)
		{
		case epicsEventWaitOK:
			status = check_connected(dbch, meta);
			if (status != pvStatOK) return status;
			if (optTest(sp, OPT_SAFE))
				/* Copy regardless of whether dirty flag is set or not */
				ss_read_buffer(ss, ch, FALSE);
			break;
		case epicsEventWaitTimeout:
			meta->status = pvStatTIMEOUT;
			meta->severity = pvSevrMAJOR;
			meta->message = "get completion timeout";
			return meta->status;
		case epicsEventWaitError:
			meta->status = pvStatERROR;
			meta->severity = pvSevrMAJOR;
			meta->message = "get completion failure";
			return meta->status;
		}
	}

	return pvStatOK;
}

/*
 * Return whether the last get completed. In safe mode, as a
 * side effect, copy value from shared buffer to state set local buffer.
 */
epicsShareFunc boolean seq_pvGetComplete(
	SS_ID		ss,
	VAR_ID		varId,
	unsigned	length,
	boolean		any,
	boolean		*complete)
{
	PROG		*sp = ss->prog;
	boolean		anyDone = FALSE, allDone = TRUE;
	unsigned	n;

	for (n = 0; n < length; n++)
	{
		epicsEventId	getSem = ss->getSem[varId+n];
		boolean		done = FALSE;
		CHAN		*ch = sp->chan + varId + n;
		pvStat		status;

		if (!ch->dbch)
		{
			/* Anonymous PVs always complete immediately */
			if (!optTest(sp, OPT_SAFE))
				errlogSevPrintf(errlogMajor,
					"pvGetComplete(%s): user error (variable not assigned)\n",
					ch->varName);
			done = TRUE;
		}
		else if (!ss->getReq[varId+n])
		{
			errlogSevPrintf(errlogMinor,
				"pvGetComplete(%s): no pending get request for this variable\n",
				ch->varName);
			done = TRUE;
		}
		else
		{
			switch (epicsEventTryWait(getSem))
			{
			case epicsEventWaitOK:
				ss->getReq[varId+n] = NULL;
				epicsEventSignal(getSem);
				status = check_connected(ch->dbch, metaPtr(ch,ss));
				if (status == pvStatOK && optTest(sp, OPT_SAFE))
				{
					/* In safe mode, copy value and meta data from shared buffer
					   to ss local buffer. */
					/* Copy regardless of whether dirty flag is set or not */
					ss_read_buffer(ss, ch, FALSE);
				}
				done = TRUE;
				break;
			case epicsEventWaitTimeout:
				break;
			case epicsEventWaitError:
				ss->getReq[varId+n] = NULL;
				epicsEventSignal(getSem);
				errlogSevPrintf(errlogFatal, "pvGetComplete(%s): "
					"epicsEventTryWait(getSem[%d]) failure\n", ch->varName, varId);
				break;
			}
		}

		anyDone = anyDone || done;
		allDone = allDone && done;

		if (complete)
		{
			complete[n] = done;
		}
		else if (any && done)
		{
			break;
		}
	}

	DEBUG("pvGetComplete: varId=%u, length=%u, anyDone=%u, allDone=%u\n",
		varId, length, anyDone, allDone);

	return any?anyDone:allDone;
}

/*
 * Cancel the last asynchronous get request.
 */
epicsShareFunc void seq_pvGetCancel(
	SS_ID		ss,
	VAR_ID		varId,
	unsigned	length)
{
	PROG		*sp = ss->prog;
	unsigned	n;

	for (n = 0; n < length; n++)
	{
		epicsEventId	getSem = ss->getSem[varId+n];
		CHAN		*ch = ss->prog->chan + varId + n;

		if (!ch->dbch)
		{
			if (!optTest(sp, OPT_SAFE))
				errlogSevPrintf(errlogMinor,
					"pvGetCancel(%s): user error (variable not assigned)\n",
					ch->varName);
		}
		else
		{
			ss->getReq[varId+n] = NULL;
			epicsEventSignal(getSem);
		}
	}
}

/* -------------------------------------------------------------------------- */

struct putq_cp_arg {
	CHAN	*ch;
	void	*var;
};

static void *putq_cp(void *dest, const void *src, size_t elemSize)
{
	struct putq_cp_arg *arg = (struct putq_cp_arg *)src;
	CHAN *ch = arg->ch;

	return memcpy(pv_value_ptr(dest, ch->type->getType), /*BUG? should that be putType?*/
		arg->var, ch->type->size * ch->count);
}

static void anonymous_put(SS_ID ss, CHAN *ch)
{
	char *var = valPtr(ch,ss);

	if (ch->queue)
	{
		QUEUE queue = ch->queue;
		pvType type = ch->type->getType; /*BUG? should that be putType?*/
		size_t size = ch->type->size;
		boolean full;
		struct putq_cp_arg arg = {ch, var};

		DEBUG("anonymous_put: type=%d, size=%d, count=%d, buf_size=%d, q=%p\n",
			type, size, ch->count, pv_size_n(type, ch->count), queue);
		print_channel_value(DEBUG, ch, var);

		/* Note: Must lock here because multiple state sets can issue
		   pvPut calls concurrently. OTOH, no need to lock against CA
		   callbacks, because anonymous and named PVs are disjoint. */
		epicsMutexMustLock(ch->varLock);

		full = seqQueuePutF(queue, putq_cp, &arg);
		if (full)
		{
			errlogSevPrintf(errlogMinor,
			  "pvPut on queued variable '%s' (anonymous): "
			  "last queue element overwritten (queue is full)\n",
			  ch->varName
			);
		}

		epicsMutexUnlock(ch->varLock);
	}
	else
	{
		/* Set dirty flag only if monitored */
		ss_write_buffer(ch, var, 0, ch->monitored);
	}
	/* If there's an event flag associated with this channel, set it */
	if (ch->syncedTo)
		seq_efSet(ss, ch->syncedTo);
	/* Wake up each state set that uses this channel in an event */
	ss_wakeup(ss->prog, ch->eventNum);
}

/*
 * Put a variable's value to a PV.
 */
epicsShareFunc pvStat seq_pvPut(SS_ID ss, VAR_ID varId, enum compType compType, double tmo)
{
	PROG	*sp = ss->prog;
	CHAN	*ch = sp->chan + varId;
	pvStat	status;
	unsigned count;
	char	*var = valPtr(ch,ss);	/* ptr to value */
	PVREQ	*req;
	DBCHAN	*dbch = ch->dbch;
	PVMETA	*meta = metaPtr(ch,ss);
	epicsEventId	putSem = ss->putSem[varId];

	DEBUG("pvPut: pv name=%s, var=%p\n", dbch ? dbch->dbName : "<anonymous>", var);

	/* First handle anonymous PV (safe mode only) */
	if (optTest(sp, OPT_SAFE) && !dbch)
	{
		anonymous_put(ss, ch);
		return pvStatOK;
	}
	if (!dbch)
	{
		errlogSevPrintf(errlogMajor,
			"pvPut(%s): user error (variable not assigned)\n",
			ch->varName
		);
		return pvStatERROR;
	}

	/* Check for channel connected */
	status = check_connected(dbch, meta);
	if (status != pvStatOK) return status;

	/* Determine whether to perform synchronous, asynchronous, or
	   plain put ((+a) option was never honored for put, so DEFAULT
	   means non-blocking and therefore implicitly asynchronous) */
	if (compType == SYNC)
	{
		double before, after;
		pvTimeGetCurrentDouble(&before);
		if (tmo <= 0.0)
		{
			errlogSevPrintf(errlogMajor,
				"pvPut(%s,SYNC,%f): user error (timeout must be positive)\n",
				ch->varName, tmo);
			return pvStatERROR;
		}
		switch (epicsEventWaitWithTimeout(putSem, tmo))
		{
		case epicsEventWaitOK:
			pvTimeGetCurrentDouble(&after);
			tmo -= (after - before);
			if (tmo <= 0.0)
				tmo = 0.001;
			break;
		case epicsEventWaitTimeout:
			errlogSevPrintf(errlogMajor,
				"pvPut(ss %s, var %s, pv %s): failed (timeout "
				"waiting for other put requests to finish)\n",
				ss->ssName, ch->varName, dbch->dbName
			);
			return pvStatERROR;
		case epicsEventWaitError:
			/* try to recover */
			ss->putReq[varId] = NULL;
			epicsEventSignal(putSem);
			errlogSevPrintf(errlogFatal,
				"pvPut: epicsEventWaitWithTimeout() failure\n");
			return pvStatERROR;
		}
	}
	else if (compType == ASYNC)
	{
		switch (epicsEventTryWait(putSem))
		{
		case epicsEventWaitOK:
			if (ss->putReq[varId] != NULL)
			{
				/* previous request timed out but user
				   did not call pvPutComplete */
				ss->putReq[varId] = NULL;
			}
			break;
		case epicsEventWaitTimeout:
			meta->status = pvStatERROR;
			meta->severity = pvSevrMAJOR;
			meta->message = "already one put pending";
			status = meta->status;
			errlogSevPrintf(errlogMajor,
				"pvPut(ss %s, var %s, pv %s): user error "
				"(there is already a put pending for this variable/"
				"state set combination)\n",
				ss->ssName, ch->varName, dbch->dbName
			);
			return pvStatERROR;
		case epicsEventWaitError:
			/* try to recover */
			ss->putReq[varId] = NULL;
			epicsEventSignal(putSem);
			errlogSevPrintf(errlogFatal,
				"pvPut: epicsEventTryWait() failure\n");
			return pvStatERROR;
		}
	}

	/* Determine number of elements to put (don't try to put more
	   than db count) */
	count = dbch->dbCount;

	/* Perform the PV put operation (either non-blocking or with a
	   callback routine specified) */
	if (compType == DEFAULT)
	{
		status = pvVarPutNoBlock(
				&dbch->pvid,		/* PV id */
				ch->type->putType,	/* data type */
				count,			/* element count */
				(pvValue *)var);	/* data value */
		if (status != pvStatOK)
		{
			errlogSevPrintf(errlogFatal, "pvPut(var %s, pv %s): pvVarPutNoBlock() failure: %s\n",
				ch->varName, dbch->dbName, pvVarGetMess(dbch->pvid));
			return status;
		}
	}
	else
	{
		/* Allocate and initialize a pv request */
		req = (PVREQ *)freeListMalloc(sp->pvReqPool);
		req->ss = ss;
		req->ch = ch;

		assert(ss->putReq[varId] == NULL);
		ss->putReq[varId] = req;

		status = pvVarPutCallback(
				&dbch->pvid,		/* PV id */
				ch->type->putType,	/* data type */
				count,			/* element count */
				(pvValue *)var,		/* data value */
				req);			/* user arg */
		if (status != pvStatOK)
		{
			ss->putReq[varId] = NULL;
			errlogSevPrintf(errlogFatal, "pvPut(var %s, pv %s): pvVarPutCallback() failure: %s\n",
				ch->varName, dbch->dbName, pvVarGetMess(dbch->pvid));
			freeListFree(sp->pvReqPool, req);
			epicsEventSignal(putSem);
			check_connected(dbch, meta);
			return status;
		}
	}

	/* Synchronous: wait for completion (10s timeout) */
	if (compType == SYNC)
	{
		epicsEventWaitStatus event_status;

		pvSysFlush(sp->pvSys);
		event_status = epicsEventWaitWithTimeout(putSem, tmo);
		ss->putReq[varId] = NULL;
		epicsEventSignal(putSem);
		switch (event_status)
		{
		case epicsEventWaitOK:
			status = check_connected(dbch, meta);
			if (status != pvStatOK) return status;
			break;
		case epicsEventWaitTimeout:
			meta->status = pvStatTIMEOUT;
			meta->severity = pvSevrMAJOR;
			meta->message = "put completion timeout";
			return meta->status;
			break;
		case epicsEventWaitError:
			meta->status = pvStatERROR;
			meta->severity = pvSevrMAJOR;
			meta->message = "put completion failure";
			return meta->status;
			break;
		}
	}

	return pvStatOK;
}

/*
 * Return whether the last put completed.
 */
epicsShareFunc boolean seq_pvPutComplete(
	SS_ID		ss,
	VAR_ID		varId,
	unsigned	length,
	boolean		any,
	boolean		*complete)
{
	PROG		*sp = ss->prog;
	boolean		anyDone = FALSE, allDone = TRUE;
	unsigned	n;

	for (n = 0; n < length; n++)
	{
		epicsEventId	putSem = ss->putSem[varId+n];
		boolean		done = FALSE;
		CHAN		*ch = sp->chan + varId + n;

		if (!ch->dbch)
		{
			/* Anonymous PVs always complete immediately */
			if (!(sp->options & OPT_SAFE))
				errlogSevPrintf(errlogMajor,
					"pvPutComplete(%s): user error (variable not assigned)\n",
					ch->varName);
			done = TRUE;
		}
		else if (!ss->putReq[varId+n])
		{
		        errlogSevPrintf(errlogMinor,
			        "pvPutComplete(%s): no pending put request for this variable\n",
			        ch->varName);
			done = TRUE;
		}
		else
		{
			switch (epicsEventTryWait(putSem))
			{
			case epicsEventWaitOK:
				ss->putReq[varId+n] = NULL;
				epicsEventSignal(putSem);
				check_connected(ch->dbch, metaPtr(ch,ss));
				done = TRUE;
				break;
			case epicsEventWaitTimeout:
				break;
			case epicsEventWaitError:
				ss->putReq[varId+n] = NULL;
				epicsEventSignal(putSem);
				errlogSevPrintf(errlogFatal, "pvPutComplete(%s): "
				  "epicsEventTryWait(putSem[%d]) failure\n", ch->varName, varId);
				break;
			}
		}

		anyDone = anyDone || done;
		allDone = allDone && done;

		if (complete)
		{
			complete[n] = done;
		}
		else if (any && done)
		{
			break;
		}
	}

	DEBUG("pvPutComplete: varId=%u, length=%u, anyDone=%u, allDone=%u\n",
		varId, length, anyDone, allDone);

	return any?anyDone:allDone;
}

/*
 * Cancel the last asynchronous put request.
 */
epicsShareFunc void seq_pvPutCancel(
	SS_ID		ss,
	VAR_ID		varId,
	unsigned	length)
{
	PROG		*sp = ss->prog;
	unsigned	n;

	for (n = 0; n < length; n++)
	{
		epicsEventId	putSem = ss->putSem[varId+n];
		CHAN		*ch = ss->prog->chan + varId + n;

		if (!ch->dbch)
		{
			if (!optTest(sp, OPT_SAFE))
				errlogSevPrintf(errlogMinor,
					"pvPutCancel(%s): user error (variable not assigned)\n",
					ch->varName);
		}
		else
		{
			ss->putReq[varId+n] = NULL;
			epicsEventSignal(putSem);
		}
	}
}

/* -------------------------------------------------------------------------- */

/*
 * Assign/Connect to a channel.
 * Assign to a zero-length string ("") disconnects/de-assigns,
 * in safe mode, creates an anonymous PV.
 */
epicsShareFunc pvStat seq_pvAssign(SS_ID ss, VAR_ID varId, const char *pvName)
{
	PROG	*sp = ss->prog;
	CHAN	*ch = sp->chan + varId;
	pvStat	status = pvStatOK;
	DBCHAN	*dbch = ch->dbch;
	char	new_pv_name[100];

	seqMacEval(sp, pvName, new_pv_name, sizeof(new_pv_name));

	DEBUG("Assign %s to \"%s\"\n", ch->varName, new_pv_name);

	epicsMutexMustLock(sp->lock);

	if (dbch)	/* was assigned to a named PV */
	{
		status = pvVarDestroy(&dbch->pvid);

		sp->assignCount -= 1;

		if (dbch->connected)	/* see connection handler */
		{
			dbch->connected = FALSE;
			sp->connectCount--;

			if (ch->monitored)
			{
				seq_camonitor(ch, FALSE);
			}
		}

		if (status != pvStatOK)
		{
			errlogSevPrintf(errlogFatal, "pvAssign(var %s, pv %s): pvVarDestroy() failure: "
				"%s\n", ch->varName, dbch->dbName, pvVarGetMess(dbch->pvid));
		}
		free(dbch->dbName);
	}

	if (new_pv_name[0] == 0)	/* new name is empty -> free resources */
	{
		if (dbch) {
			free(ch->dbch);
		}
	}
	else		/* new name is non-empty -> create resources */
	{
		if (!dbch)
		{
			dbch = new(DBCHAN);
			if (!dbch)
			{
				errlogSevPrintf(errlogFatal, "pvAssign: calloc failed\n");
				return pvStatERROR;
			}
		}
		dbch->dbName = epicsStrDup(new_pv_name);
		if (!dbch->dbName)
		{
			errlogSevPrintf(errlogFatal, "pvAssign: epicsStrDup failed\n");
			free(dbch);
			return pvStatERROR;
		}
		ch->dbch = dbch;
		sp->assignCount++;
		status = pvVarCreate(
			sp->pvSys,		/* PV system context */
			dbch->dbName,		/* DB channel name */
			seq_conn_handler,	/* connection handler routine */
			seq_event_handler,	/* event handler routine */
			ch,			/* user ptr is CHAN structure */
			&dbch->pvid);		/* ptr to pvid */
		if (status != pvStatOK)
		{
			errlogSevPrintf(errlogFatal, "pvAssign(var %s, pv %s): pvVarCreate() failure: "
				"%s\n", ch->varName, dbch->dbName, pvVarGetMess(dbch->pvid));
			free(ch->dbch->dbName);
			free(ch->dbch);
		}
	}

	epicsMutexUnlock(sp->lock);

	return status;
}

/*
 * Initiate a monitor.
 */
epicsShareFunc pvStat seq_pvMonitor(SS_ID ss, VAR_ID varId)
{
	PROG	*sp = ss->prog;
	CHAN	*ch = sp->chan + varId;
	DBCHAN	*dbch = ch->dbch;

	if (!dbch && optTest(sp, OPT_SAFE))
	{
		ch->monitored = TRUE;
		return pvStatOK;
	}
	if (!dbch)
	{
		errlogSevPrintf(errlogMajor,
			"pvMonitor(%s): user error (variable not assigned)\n",
			ch->varName
		);
		return pvStatERROR;
	}
	ch->monitored = TRUE;
	return seq_camonitor(ch, TRUE);
}

/*
 * Cancel a monitor.
 */
epicsShareFunc pvStat seq_pvStopMonitor(SS_ID ss, VAR_ID varId)
{
	PROG	*sp = ss->prog;
	CHAN	*ch = sp->chan + varId;
	DBCHAN	*dbch = ch->dbch;

	if (!dbch && optTest(sp, OPT_SAFE))
	{
		ch->monitored = FALSE;
		return pvStatOK;
	}
	if (!dbch)
	{
		errlogSevPrintf(errlogMajor,
			"pvStopMonitor(%s): user error (variable not assigned)\n",
			ch->varName
		);
		return pvStatERROR;
	}
	ch->monitored = FALSE;
	return seq_camonitor(ch, FALSE);
}

/*
 * Synchronize pv with an event flag.
 * ev_flag == 0 means unSync.
 */
epicsShareFunc void seq_pvSync(SS_ID ss, VAR_ID varId, unsigned length, EV_ID new_ev_flag)
{
	PROG	*sp = ss->prog;
	unsigned i;

	assert(new_ev_flag >= 0 && new_ev_flag <= sp->numEvFlags);

	epicsMutexMustLock(sp->lock);
	for (i=0; i<length; i++)
	{
		CHAN	*this_ch = sp->chan + varId + i;
		EV_ID	old_ev_flag = this_ch->syncedTo;

		if (old_ev_flag != new_ev_flag)
		{
			if (old_ev_flag)
			{
				/* remove it from the old list */
				CHAN *ch = sp->syncedChans[old_ev_flag];
				assert(ch);			/* since old_ev_flag != 0 */
				if (ch == this_ch)		/* first in list */
				{
					sp->syncedChans[old_ev_flag] = this_ch->nextSynced;
					ch->nextSynced = 0;
				}
				else
				{
					while (ch->nextSynced != this_ch)
					{
						ch = ch->nextSynced;
						assert(ch);	/* since old_ev_flag != 0 */
					}
					assert (ch->nextSynced == this_ch);
					ch->nextSynced = this_ch->nextSynced;
				}
			}
			this_ch->syncedTo = new_ev_flag;
			if (new_ev_flag)
			{
				/* insert it into the new list */
				CHAN *ch = sp->syncedChans[new_ev_flag];
				sp->syncedChans[new_ev_flag] = this_ch;
				this_ch->nextSynced = ch;
			}
		}
	}
	epicsMutexUnlock(sp->lock);
}

/*
 * Return total number of database channels.
 */
epicsShareFunc unsigned seq_pvChannelCount(SS_ID ss)
{
	return ss->prog->numChans;
}

/*
 * Return number of database channels connected.
 */
epicsShareFunc unsigned seq_pvConnectCount(SS_ID ss)
{
	return ss->prog->connectCount;
}

/*
 * Return number of database channels assigned.
 */
epicsShareFunc unsigned seq_pvAssignCount(SS_ID ss)
{
	return ss->prog->assignCount;
}

/* Flush outstanding PV requests */
epicsShareFunc void seq_pvFlush(SS_ID ss)
{
	pvSysFlush(ss->prog->pvSys);
}

/*
 * Return whether database channel is connected.
 */
epicsShareFunc boolean seq_pvConnected(SS_ID ss, VAR_ID varId)
{
	CHAN *ch = ss->prog->chan + varId;
	return ch->dbch && ch->dbch->connected;
}

/*
 * Return whether database channel is assigned.
 */
epicsShareFunc boolean seq_pvAssigned(SS_ID ss, VAR_ID varId)
{
	return ss->prog->chan[varId].dbch != NULL;
}

/*
 * Return number elements in an array, which is the lesser of
 * the array size and the element count returned by the PV layer.
 */
epicsShareFunc unsigned seq_pvCount(SS_ID ss, VAR_ID varId)
{
	CHAN *ch = ss->prog->chan + varId;
	return ch->dbch ? ch->dbch->dbCount : ch->count;
}

/*
 * Return a channel name of an assigned variable.
 */
epicsShareFunc char *seq_pvName(SS_ID ss, VAR_ID varId)
{
	CHAN *ch = ss->prog->chan + varId;
	return ch->dbch ? ch->dbch->dbName : NULL;
}

/*
 * Return channel alarm status.
 */
epicsShareFunc pvStat seq_pvStatus(SS_ID ss, VAR_ID varId)
{
	CHAN	*ch = ss->prog->chan + varId;
	PVMETA	*meta = metaPtr(ch,ss);
	return ch->dbch ? meta->status : pvStatOK;
}

/*
 * Return channel alarm severity.
 */
epicsShareFunc pvSevr seq_pvSeverity(SS_ID ss, VAR_ID varId)
{
	CHAN	*ch = ss->prog->chan + varId;
	PVMETA	*meta = metaPtr(ch,ss);
	return ch->dbch ? meta->severity : pvSevrOK;
}

/*
 * Return channel error message.
 */
epicsShareFunc const char *seq_pvMessage(SS_ID ss, VAR_ID varId)
{
	CHAN	*ch = ss->prog->chan + varId;
	PVMETA	*meta = metaPtr(ch,ss);
	return ch->dbch ? meta->message : "";
}

/*
 * Return channel time stamp.
 */
epicsShareFunc epicsTimeStamp seq_pvTimeStamp(SS_ID ss, VAR_ID varId)
{
	CHAN	*ch = ss->prog->chan + varId;
	PVMETA	*meta = metaPtr(ch,ss);
	if (ch->dbch)
	{
		return meta->timeStamp;
	}
	else
	{
		epicsTimeStamp ts;
		epicsTimeGetCurrent(&ts);
		return ts;
	}
}

/*
 * Set an event flag, then wake up each state
 * set that might be waiting on that event flag.
 */
epicsShareFunc void seq_efSet(SS_ID ss, EV_ID ev_flag)
{
	PROG	*sp = ss->prog;

	DEBUG("efSet: sp=%p, ss=%p, ev_flag=%d\n", sp, ss,
		ev_flag);
	assert(ev_flag > 0 && ev_flag <= ss->prog->numEvFlags);

	epicsMutexMustLock(sp->lock);

	/* Set this bit */
	bitSet(sp->evFlags, ev_flag);

	/* Wake up state sets that are waiting for this event flag */
	ss_wakeup(sp, ev_flag);

	epicsMutexUnlock(sp->lock);
}

/*
 * Return whether event flag is set.
 */
epicsShareFunc boolean seq_efTest(SS_ID ss, EV_ID ev_flag)
/* event flag */
{
	PROG	*sp = ss->prog;
	boolean	isSet;

	assert(ev_flag > 0 && ev_flag <= ss->prog->numEvFlags);
	epicsMutexMustLock(sp->lock);

	isSet = bitTest(sp->evFlags, ev_flag);

	DEBUG("efTest: ev_flag=%d, isSet=%d\n", ev_flag, isSet);

	if (optTest(sp, OPT_SAFE))
		ss_read_buffer_selective(sp, ss, ev_flag);

	epicsMutexUnlock(sp->lock);

	return isSet;
}

/*
 * Clear event flag.
 */
epicsShareFunc boolean seq_efClear(SS_ID ss, EV_ID ev_flag)
{
	PROG	*sp = ss->prog;
	boolean	isSet;

	assert(ev_flag > 0 && ev_flag <= ss->prog->numEvFlags);
	epicsMutexMustLock(sp->lock);

	isSet = bitTest(sp->evFlags, ev_flag);
	bitClear(sp->evFlags, ev_flag);

	/* Wake up state sets that are waiting for this event flag */
	ss_wakeup(sp, ev_flag);

	epicsMutexUnlock(sp->lock);

	return isSet;
}

/*
 * Atomically test event flag against outstanding events, then clear it
 * and return whether it was set.
 */
epicsShareFunc boolean seq_efTestAndClear(SS_ID ss, EV_ID ev_flag)
{
	PROG	*sp = ss->prog;
	boolean	isSet;

	assert(ev_flag > 0 && ev_flag <= ss->prog->numEvFlags);
	epicsMutexMustLock(sp->lock);

	isSet = bitTest(sp->evFlags, ev_flag);
	bitClear(sp->evFlags, ev_flag);

	DEBUG("efTestAndClear: ev_flag=%d, isSet=%d, ss=%d\n", ev_flag, isSet,
		(int)ssNum(ss));

	if (optTest(sp, OPT_SAFE))
		ss_read_buffer_selective(sp, ss, ev_flag);

	epicsMutexUnlock(sp->lock);

	return isSet;
}

struct getq_cp_arg {
	CHAN	*ch;
	void	*var;
	PVMETA	*meta;
};

static void *getq_cp(void *dest, const void *value, size_t elemSize)
{
	struct getq_cp_arg *arg = (struct getq_cp_arg *)dest;
	CHAN	*ch = arg->ch;
	PVMETA	*meta = arg->meta;
	void	*var = arg->var;
	pvType	type = ch->type->getType;
	size_t	count = ch->count;

	if (ch->dbch)
	{
		assert(pv_is_time_type(type));
		/* Copy status, severity and time stamp */
		meta->status = pv_status(value,type);
		meta->severity = pv_severity(value,type);
		meta->timeStamp = pv_stamp(value,type);
		count = ch->dbch->dbCount;
	}
	return memcpy(var, pv_value_ptr(value,type), ch->type->size * count);
}

/*
 * Get value from a queued PV.
 */
epicsShareFunc boolean seq_pvGetQ(SS_ID ss, VAR_ID varId)
{
	PROG	*sp = ss->prog;
	CHAN	*ch = sp->chan + varId;
	void	*var = valPtr(ch,ss);
	EV_ID	ev_flag = ch->syncedTo;
	PVMETA	*meta = metaPtr(ch,ss);
	boolean	was_empty;
	struct getq_cp_arg arg = {ch, var, meta};

	if (!ch->queue)
	{
		errlogSevPrintf(errlogMajor,
			"pvGetQ(%s): user error (variable not queued)\n",
			ch->varName
		);
		return FALSE;
	}

	was_empty = seqQueueGetF(ch->queue, getq_cp, &arg);

	if (ev_flag)
	{
		epicsMutexMustLock(sp->lock);
		/* If queue is now empty, clear the event flag */
		if (seqQueueIsEmpty(ch->queue))
		{
			bitClear(sp->evFlags, ev_flag);
		}
		epicsMutexUnlock(sp->lock);
	}

	return (!was_empty);
}

/*
 * Flush elements on syncQ queue and clear event flag.
 */
epicsShareFunc void seq_pvFlushQ(SS_ID ss, VAR_ID varId)
{
	PROG	*sp = ss->prog;
	CHAN	*ch = sp->chan + varId;
	EV_ID	ev_flag = ch->syncedTo;
	QUEUE	queue = ch->queue;

	DEBUG("pvFlushQ: pv name=%s, count=%d\n",
		ch->dbch ? ch->dbch->dbName : "<anomymous>", seqQueueUsed(queue));
	seqQueueFlush(queue);

	epicsMutexMustLock(sp->lock);
	/* Clear event flag */
	bitClear(sp->evFlags, ev_flag);
	epicsMutexUnlock(sp->lock);
}

/*
 * Test whether a given delay has expired.
 *
 * As a side-effect, adjust the state set's wakeupTime if our delay
 * is shorter than previously tested ones.
 */
epicsShareFunc boolean seq_delay(SS_ID ss, double delay)
{
	boolean	expired;
	double	now, timeExpired;

	pvTimeGetCurrentDouble(&now);
	timeExpired = ss->timeEntered + delay;
	expired = timeExpired <= now;
	if (!expired && timeExpired < ss->wakeupTime)
		ss->wakeupTime = timeExpired;

	DEBUG("delay(%s/%s,%.10f): entered=%.10f, diff=%.10f, %s\n", ss->ssName,
		ss->states[ss->currentState].stateName, delay, ss->timeEntered,
		timeExpired - now, expired ? "expired": "unexpired");
	return expired;
}

/*
 * Return the value of an option (e.g. "a").
 * FALSE means "-" and TRUE means "+".
 */
epicsShareFunc boolean seq_optGet(SS_ID ss, const char *opt)
{
	PROG	*sp = ss->prog;

	assert(opt);
	switch (opt[0])
	{
	case 'a': return optTest(sp, OPT_ASYNC);
	case 'c': return optTest(sp, OPT_CONN);
	case 'd': return optTest(sp, OPT_DEBUG);
	case 'e': return optTest(sp, OPT_NEWEF);
	case 'r': return optTest(sp, OPT_REENT);
	case 's': return optTest(sp, OPT_SAFE);
	default:  return FALSE;
	}
}

/* 
 * Given macro name, return pointer to its value.
 */
epicsShareFunc char *seq_macValueGet(SS_ID ss, const char *name)
{
	return seqMacValGet(ss->prog, name);
}

/* 
 * Immediately terminate all state sets and jump to global exit block.
 */
epicsShareFunc void seq_exit(SS_ID ss)
{
	PROG *sp = ss->prog;
	/* Ask all state set threads to exit */
	sp->die = TRUE;
	/* Take care that we die even if waiting for initial connect */
	epicsEventSignal(sp->ready);
	/* Wakeup all state sets unconditionally */
	ss_wakeup(sp, 0);
}
