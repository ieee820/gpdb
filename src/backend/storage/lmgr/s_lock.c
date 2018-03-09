/*-------------------------------------------------------------------------
 *
 * s_lock.c
 *	   Hardware-dependent implementation of spinlocks.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/lmgr/s_lock.c,v 1.51 2010/01/02 16:57:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>
#include <unistd.h>

#include "storage/s_lock.h"

slock_t		dummy_spinlock;

static int	spins_per_delay = DEFAULT_SPINS_PER_DELAY;


/*
 * s_lock_stuck() - complain about a stuck spinlock
 */
static void
s_lock_stuck(volatile slock_t *lock, const char *file, int line)
{
#if defined(S_LOCK_TEST)
	fprintf(stderr,
			"\nStuck spinlock (%p) detected at %s:%d.\n",
			lock, file, line);
	exit(1);
#else
	elog(PANIC, "stuck spinlock (%p) detected at %s:%d",
		 lock, file, line);
#endif
}


/*
 * s_lock(lock) - platform-independent portion of waiting for a spinlock.
 */
void
s_lock(volatile slock_t *lock, const char *file, int line)
{
	/*
	 * We loop tightly for awhile, then delay using pg_usleep() and try again.
	 * Preferably, "awhile" should be a small multiple of the maximum time we
	 * expect a spinlock to be held.  100 iterations seems about right as an
	 * initial guess.  However, on a uniprocessor the loop is a waste of
	 * cycles, while in a multi-CPU scenario it's usually better to spin a bit
	 * longer than to call the kernel, so we try to adapt the spin loop count
	 * depending on whether we seem to be in a uniprocessor or multiprocessor.
	 *
	 * Note: you might think MIN_SPINS_PER_DELAY should be just 1, but you'd
	 * be wrong; there are platforms where that can result in a "stuck
	 * spinlock" failure.  This has been seen particularly on Alphas; it seems
	 * that the first TAS after returning from kernel space will always fail
	 * on that hardware.
	 *
	 * Once we do decide to block, we use randomly increasing pg_usleep()
	 * delays. The first delay is 1 msec, then the delay randomly increases to
	 * about one second, after which we reset to 1 msec and start again.  The
	 * idea here is that in the presence of heavy contention we need to
	 * increase the delay, else the spinlock holder may never get to run and
	 * release the lock.  (Consider situation where spinlock holder has been
	 * nice'd down in priority by the scheduler --- it will not get scheduled
	 * until all would-be acquirers are sleeping, so if we always use a 1-msec
	 * sleep, there is a real possibility of starvation.)  But we can't just
	 * clamp the delay to an upper bound, else it would take a long time to
	 * make a reasonable number of tries.
	 *
	 * We time out and declare error after NUM_DELAYS delays (thus, exactly
	 * that many tries).  With the given settings, this will usually take 2 or
	 * so minutes.	It seems better to fix the total number of tries (and thus
	 * the probability of unintended failure) than to fix the total time
	 * spent.
	 *
	 * The pg_usleep() delays are measured in milliseconds because 1 msec is a
	 * common resolution limit at the OS level for newer platforms. On older
	 * platforms the resolution limit is usually 10 msec, in which case the
	 * total delay before timeout will be a bit more.
	 */
#define MIN_SPINS_PER_DELAY 10
#define MAX_SPINS_PER_DELAY 1000
#define NUM_DELAYS			1000
#define MIN_DELAY_MSEC		1
#define MAX_DELAY_MSEC		1000

	int			spins = 0;
	int			delays = 0;
	int			cur_delay = 0;

	while (TAS(lock))
	{
		/* CPU-specific delay each time through the loop */
		SPIN_DELAY();

		/* Block the process every spins_per_delay tries */
		if (++spins >= spins_per_delay)
		{
			if (++delays > NUM_DELAYS)
				s_lock_stuck(lock, file, line);

			if (cur_delay == 0) /* first time to delay? */
				cur_delay = MIN_DELAY_MSEC;

			pg_usleep(cur_delay * 1000L);

#if defined(S_LOCK_TEST)
			fprintf(stdout, "*");
			fflush(stdout);
#endif

			/* increase delay by a random fraction between 1X and 2X */
			cur_delay += (int) (cur_delay *
					  ((double) random() / (double) MAX_RANDOM_VALUE) + 0.5);
			/* wrap back to minimum delay when max is exceeded */
			if (cur_delay > MAX_DELAY_MSEC)
				cur_delay = MIN_DELAY_MSEC;

			spins = 0;
		}
	}

	/*
	 * If we were able to acquire the lock without delaying, it's a good
	 * indication we are in a multiprocessor.  If we had to delay, it's a sign
	 * (but not a sure thing) that we are in a uniprocessor. Hence, we
	 * decrement spins_per_delay slowly when we had to delay, and increase it
	 * rapidly when we didn't.  It's expected that spins_per_delay will
	 * converge to the minimum value on a uniprocessor and to the maximum
	 * value on a multiprocessor.
	 *
	 * Note: spins_per_delay is local within our current process. We want to
	 * average these observations across multiple backends, since it's
	 * relatively rare for this function to even get entered, and so a single
	 * backend might not live long enough to converge on a good value.	That
	 * is handled by the two routines below.
	 */
	if (cur_delay == 0)
	{
		/* we never had to delay */
		if (spins_per_delay < MAX_SPINS_PER_DELAY)
			spins_per_delay = Min(spins_per_delay + 100, MAX_SPINS_PER_DELAY);
	}
	else
	{
		if (spins_per_delay > MIN_SPINS_PER_DELAY)
			spins_per_delay = Max(spins_per_delay - 1, MIN_SPINS_PER_DELAY);
	}
}


/*
 * Set local copy of spins_per_delay during backend startup.
 *
 * NB: this has to be pretty fast as it is called while holding a spinlock
 */
void
set_spins_per_delay(int shared_spins_per_delay)
{
	spins_per_delay = shared_spins_per_delay;
}

/*
 * Recompute shared estimate of spins_per_delay during backend exit.
 *
 * NB: this has to be pretty fast as it is called while holding a spinlock
 */
int
recompute_spins_per_delay(int shared_spins_per_delay)
{
	/*
	 * We use an exponential moving average with a relatively slow adaption
	 * rate, so that noise in any one backend's result won't affect the shared
	 * value too much.	As long as both inputs are within the allowed range,
	 * the result must be too, so we need not worry about clamping the result.
	 *
	 * We deliberately truncate rather than rounding; this is so that single
	 * adjustments inside a backend can affect the shared estimate (see the
	 * asymmetric adjustment rules above).
	 */
	return (shared_spins_per_delay * 15 + spins_per_delay) / 16;
}


/*
 * Various TAS implementations that cannot live in s_lock.h as no inline
 * definition exists (yet).
 * In the future, get rid of tas.[cso] and fold it into this file.
 *
 * If you change something here, you will likely need to modify s_lock.h too,
 * because the definitions for these are split between this file and s_lock.h.
 */


#ifdef HAVE_SPINLOCKS			/* skip spinlocks if requested */


#if defined(__GNUC__)

/*
 * All the gcc flavors that are not inlined
 */


/*
 * Note: all the if-tests here probably ought to be testing gcc version
 * rather than platform, but I don't have adequate info to know what to
 * write.  Ideally we'd flush all this in favor of the inline version.
 */
#if defined(__m68k__) && !defined(__linux__)
/* really means: extern int tas(slock_t* **lock); */
static void
tas_dummy()
{
	__asm__		__volatile__(
#if defined(__NetBSD__) && defined(__ELF__)
/* no underscore for label and % for registers */
										 "\
.global		tas 				\n\
tas:							\n\
			movel	%sp@(0x4),%a0	\n\
			tas 	%a0@		\n\
			beq 	_success	\n\
			moveq	#-128,%d0	\n\
			rts 				\n\
_success:						\n\
			moveq	#0,%d0		\n\
			rts 				\n"
#else
										 "\
.global		_tas				\n\
_tas:							\n\
			movel	sp@(0x4),a0	\n\
			tas 	a0@			\n\
			beq 	_success	\n\
			moveq 	#-128,d0	\n\
			rts					\n\
_success:						\n\
			moveq 	#0,d0		\n\
			rts					\n"
#endif   /* __NetBSD__ && __ELF__ */
	);
}
#endif   /* __m68k__ && !__linux__ */
#else							/* not __GNUC__ */

/*
 * All non gcc
 */


#if defined(sun3)
static void
tas_dummy()						/* really means: extern int tas(slock_t
								 * *lock); */
{
	asm("LLA0:");
	asm("   .data");
	asm("   .text");
	asm("|#PROC# 04");
	asm("   .globl  _tas");
	asm("_tas:");
	asm("|#PROLOGUE# 1");
	asm("   movel   sp@(0x4),a0");
	asm("   tas a0@");
	asm("   beq LLA1");
	asm("   moveq   #-128,d0");
	asm("   rts");
	asm("LLA1:");
	asm("   moveq   #0,d0");
	asm("   rts");
	asm("   .data");
}
#endif   /* sun3 */
#endif   /* not __GNUC__ */
#endif   /* HAVE_SPINLOCKS */



/*****************************************************************************/
#if defined(S_LOCK_TEST)

/*
 * test program for verifying a port's spinlock support.
 */

struct test_lock_struct
{
	char		pad1;
	slock_t		lock;
	char		pad2;
};

volatile struct test_lock_struct test_lock;

int
main()
{
	srandom((unsigned int) time(NULL));

	test_lock.pad1 = test_lock.pad2 = 0x44;

	S_INIT_LOCK(&test_lock.lock);

	if (test_lock.pad1 != 0x44 || test_lock.pad2 != 0x44)
	{
		printf("S_LOCK_TEST: failed, declared datatype is wrong size\n");
		return 1;
	}

	if (!S_LOCK_FREE(&test_lock.lock))
	{
		printf("S_LOCK_TEST: failed, lock not initialized\n");
		return 1;
	}

	S_LOCK(&test_lock.lock);

	if (test_lock.pad1 != 0x44 || test_lock.pad2 != 0x44)
	{
		printf("S_LOCK_TEST: failed, declared datatype is wrong size\n");
		return 1;
	}

	if (S_LOCK_FREE(&test_lock.lock))
	{
		printf("S_LOCK_TEST: failed, lock not locked\n");
		return 1;
	}

	S_UNLOCK(&test_lock.lock);

	if (test_lock.pad1 != 0x44 || test_lock.pad2 != 0x44)
	{
		printf("S_LOCK_TEST: failed, declared datatype is wrong size\n");
		return 1;
	}

	if (!S_LOCK_FREE(&test_lock.lock))
	{
		printf("S_LOCK_TEST: failed, lock not unlocked\n");
		return 1;
	}

	S_LOCK(&test_lock.lock);

	if (test_lock.pad1 != 0x44 || test_lock.pad2 != 0x44)
	{
		printf("S_LOCK_TEST: failed, declared datatype is wrong size\n");
		return 1;
	}

	if (S_LOCK_FREE(&test_lock.lock))
	{
		printf("S_LOCK_TEST: failed, lock not re-locked\n");
		return 1;
	}

	printf("S_LOCK_TEST: this will print %d stars and then\n", NUM_DELAYS);
	printf("             exit with a 'stuck spinlock' message\n");
	printf("             if S_LOCK() and TAS() are working.\n");
	fflush(stdout);

	s_lock(&test_lock.lock, __FILE__, __LINE__);

	printf("S_LOCK_TEST: failed, lock not locked\n");
	return 1;
}

#endif   /* S_LOCK_TEST */
