/*************************************************************************\
Copyright (c) 1993      The Regents of the University of California
                        and the University of Chicago.
                        Los Alamos National Laboratory
Copyright (c) 2010-2012 Helmholtz-Zentrum Berlin f. Materialien
                        und Energie GmbH, Germany (HZB)
This file is distributed subject to a Software License Agreement found
in the file LICENSE that is included with this distribution.
\*************************************************************************/
/*	External interface to the sequencer run-time library
 *
 *	Author:		Andy Kozubal
 *	Date:		01mar94
 *
 *	Experimental Physics and Industrial Control System (EPICS)
 *
 *	This software was produced under  U.S. Government contracts:
 *	(W-7405-ENG-36) at the Los Alamos National Laboratory,
 *	and (W-31-109-ENG-38) at Argonne National Laboratory.
 *
 *	Initial development by:
 *		The Controls and Automation Group (AT-8)
 *		Ground Test Accelerator
 *		Accelerator Technology Division
 *		Los Alamos National Laboratory
 */
#ifndef INCLseqComh
#define INCLseqComh

#include "shareLib.h"
#include "pvAlarm.h"
#include "pvType.h"
#include "epicsThread.h"
#include "epicsTime.h"

#include "seq_release.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bit encoding for run-time options */
#define OPT_DEBUG		(1<<0)	/* turn on debugging */
#define OPT_ASYNC		(1<<1)	/* use async. gets */
#define OPT_CONN		(1<<2)	/* wait for all connections */
#define OPT_REENT		(1<<3)	/* generate reentrant code */
#define OPT_NEWEF		(1<<4)	/* new event flag mode */
#define OPT_MAIN		(1<<5)	/* generate main program */
#define OPT_SAFE		(1<<6)	/* safe mode */

/* Bit encoding for State Specific Options */
#define OPT_NORESETTIMERS	(1<<0)	/* Don't reset timers on */
					/* entry to state from same state */
#define OPT_DOENTRYFROMSELF	(1<<1)	/* Do entry{} even if from same state */
#define OPT_DOEXITTOSELF	(1<<2)	/* Do exit{} even if to same state */

/* seqMask macros */
#define NBITS			(8*sizeof(seqMask))	/* # bits in seqMask word */
#define NWORDS(maxBitNum)	(1+(maxBitNum)/NBITS)	/* # words in seqMask */

#define bitSet(words, bitnum)	( words[(bitnum)/NBITS] |=  (1u<<((bitnum)%NBITS)))
#define bitClear(words, bitnum)	( words[(bitnum)/NBITS] &= ~(1u<<((bitnum)%NBITS)))
#define bitTest(words, bitnum)	((words[(bitnum)/NBITS] &  (1u<<((bitnum)%NBITS))) != 0)

#define NOEVFLAG		0	/* argument to pvSync to remove sync */

#define DEFAULT_QUEUE_SIZE	100	/* default queue size (elements) */

/* I/O completion type (extra argument passed to seq_pvGet() and seq_pvPut()) */
enum compType {
	DEFAULT,
        ASYNC,
        SYNC
};

#ifndef TRUE
#define TRUE	1
#endif
#ifndef FALSE
#define FALSE	0
#endif

typedef	struct state_set *SS_ID;	/* state set id, opaque */
typedef struct UserVar USER_VAR;	/* defined by program, opaque */
typedef char string[MAX_STRING_SIZE];	/* the string typedef */

/* these typedefs make the code more self documenting */
typedef epicsUInt32 seqMask;		/* for event masks and options */
typedef unsigned EV_ID;			/* identifier for an event */
typedef unsigned VAR_ID;		/* identifier for a pv */
typedef unsigned DELAY_ID;		/* identifier for a delay */
typedef int seqBool;

/* Prototypes for functions generated by snc */
typedef void ACTION_FUNC(SS_ID ssId, USER_VAR *var, int transNum, int *nextState);
typedef seqBool EVENT_FUNC(SS_ID ssId, USER_VAR *var, int *transNum, int *nextState);
typedef void DELAY_FUNC(SS_ID ssId, USER_VAR *var);
typedef void ENTRY_FUNC(SS_ID ssId, USER_VAR *var);
typedef void EXIT_FUNC(SS_ID ssId, USER_VAR *var);
typedef void INIT_FUNC(USER_VAR *var);

typedef const struct seqChan seqChan;
typedef const struct seqState seqState;
typedef const struct seqSS seqSS;
typedef struct seqProgram seqProgram;

/* Static information about a channel */
struct seqChan
{
	const char	*chName;	/* assigned channel name */
	size_t		offset;		/* offset to value */
	const char	*varName;	/* variable name, including subscripts*/
	const char	*varType;	/* variable type, e.g. "int" */
	unsigned	count;		/* element count for arrays */
	unsigned	eventNum;	/* event number for this channel */
	EV_ID		efId;		/* event flag id if synced */
	seqBool		monitored;	/* whether channel should be monitored */
	unsigned	queueSize;	/* syncQ queue size (0=not queued) */
	unsigned	queueIndex;	/* syncQ queue index */
};

/* Static information about a state */
struct seqState
{
	const char	*stateName;	/* state name */
	ACTION_FUNC	*actionFunc;	/* action routine for this state */
	EVENT_FUNC	*eventFunc;	/* event routine for this state */
	DELAY_FUNC	*delayFunc;	/* delay setup routine for this state */
	ENTRY_FUNC	*entryFunc;	/* statements performed on entry to state */
	EXIT_FUNC	*exitFunc;	/* statements performed on exit from state */
	const seqMask	*eventMask;	/* event mask for this state */
	seqMask		options;	/* state option mask */
};

/* Static information about a state set */
struct seqSS
{
	const char	*ssName;	/* state set name */
	seqState	*states;	/* array of state blocks */
	unsigned	numStates;	/* number of states in this state set */
	unsigned	numDelays;	/* number of delays in this state set */
};

/* Static information about a state program */
struct seqProgram
{
	unsigned	magic;		/* magic number */
	const char	*progName;	/* program name (for debugging) */
	seqChan		*chan;		/* table of channels */
	unsigned	numChans;	/* number of db channels */
	seqSS		*ss;		/* array of state set info structs */
	unsigned	numSS;		/* number of state sets */
	unsigned	varSize;	/* # bytes in user variable area */
	const char	*params;	/* program paramters */
	unsigned	numEvFlags;	/* number of event flags */
	seqMask		options;	/* program option mask */
	INIT_FUNC	*initFunc;	/* init function */
	ENTRY_FUNC	*entryFunc;	/* entry function */
	EXIT_FUNC	*exitFunc;	/* exit function */
	unsigned	numQueues;	/* number of syncQ queues */
};

/*
 * Function declarations for interface between state program & sequencer.
 * Prefix "seq_" is added by SNC to reduce probability of name clashes.
 * Implementations are in module seq_if.c.
 */

/* event flag operations */
epicsShareFunc void epicsShareAPI seq_efSet(SS_ID, EV_ID);
epicsShareFunc seqBool epicsShareAPI seq_efTest(SS_ID, EV_ID);
epicsShareFunc seqBool epicsShareAPI seq_efClear(SS_ID, EV_ID);
epicsShareFunc seqBool epicsShareAPI seq_efTestAndClear(SS_ID, EV_ID);
/* pv operations */
epicsShareFunc pvStat epicsShareAPI seq_pvGet(SS_ID, VAR_ID, enum compType);
epicsShareFunc pvStat epicsShareAPI seq_pvGetMultiple(SS_ID, VAR_ID,
	unsigned, enum compType);
epicsShareFunc seqBool epicsShareAPI seq_pvGetQ(SS_ID, VAR_ID);
epicsShareFunc void epicsShareAPI seq_pvFlushQ(SS_ID, VAR_ID);
/* retain seq_pvFreeQ for compatibility */
#define seq_pvFreeQ seq_pvFlushQ
epicsShareFunc pvStat epicsShareAPI seq_pvPut(SS_ID, VAR_ID, enum compType);
epicsShareFunc pvStat epicsShareAPI seq_pvPutMultiple(SS_ID, VAR_ID,
	unsigned, enum compType);
epicsShareFunc seqBool epicsShareAPI seq_pvGetComplete(SS_ID, VAR_ID);
epicsShareFunc seqBool epicsShareAPI seq_pvPutComplete(SS_ID, VAR_ID,
	unsigned, seqBool, seqBool*);
epicsShareFunc pvStat epicsShareAPI seq_pvAssign(SS_ID, VAR_ID, const char *);
epicsShareFunc pvStat epicsShareAPI seq_pvMonitor(SS_ID, VAR_ID);
epicsShareFunc void epicsShareAPI seq_pvSync(SS_ID, VAR_ID, EV_ID);
epicsShareFunc pvStat epicsShareAPI seq_pvStopMonitor(SS_ID, VAR_ID);
/* pv info */
epicsShareFunc char *epicsShareAPI seq_pvName(SS_ID, VAR_ID);
epicsShareFunc unsigned epicsShareAPI seq_pvCount(SS_ID, VAR_ID);
epicsShareFunc pvStat epicsShareAPI seq_pvStatus(SS_ID, VAR_ID);
epicsShareFunc pvSevr epicsShareAPI seq_pvSeverity(SS_ID, VAR_ID);
epicsShareFunc epicsTimeStamp epicsShareAPI seq_pvTimeStamp(SS_ID, VAR_ID);
epicsShareFunc const char *epicsShareAPI seq_pvMessage(SS_ID, VAR_ID);
epicsShareFunc seqBool epicsShareAPI seq_pvAssigned(SS_ID, VAR_ID);
epicsShareFunc seqBool epicsShareAPI seq_pvConnected(SS_ID, VAR_ID);
epicsShareFunc VAR_ID epicsShareAPI seq_pvIndex(SS_ID, VAR_ID);
/* global operations */
epicsShareFunc void epicsShareAPI seq_pvFlush(SS_ID);
epicsShareFunc void epicsShareAPI seq_delayInit(SS_ID, DELAY_ID, double);
epicsShareFunc seqBool epicsShareAPI seq_delay(SS_ID, DELAY_ID);
epicsShareFunc char *epicsShareAPI seq_macValueGet(SS_ID, const char *);
epicsShareFunc void epicsShareAPI seq_exit(SS_ID);
/* global info */
epicsShareFunc unsigned epicsShareAPI seq_pvChannelCount(SS_ID);
epicsShareFunc unsigned epicsShareAPI seq_pvConnectCount(SS_ID);
epicsShareFunc unsigned epicsShareAPI seq_pvAssignCount(SS_ID);
epicsShareFunc seqBool epicsShareAPI seq_optGet(SS_ID, const char *);
/* shell commands */
epicsShareFunc void epicsShareAPI seqShow(epicsThreadId);
epicsShareFunc void epicsShareAPI seqChanShow(epicsThreadId, const char *);
epicsShareFunc void epicsShareAPI seqcar(int level);
epicsShareFunc void epicsShareAPI seqQueueShow(epicsThreadId);
epicsShareFunc void epicsShareAPI seqStop(epicsThreadId);
epicsShareFunc void epicsShareAPI seqRegisterSequencerProgram(seqProgram *p);
epicsShareFunc void epicsShareAPI seqRegisterSequencerCommands(void);
epicsShareFunc void epicsShareAPI seq(seqProgram *, const char *, unsigned);
/* exported for devSequencer */
epicsShareFunc struct program_instance* epicsShareAPI seqFindProgByName(const char *, int);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif	/*INCLseqComh*/
