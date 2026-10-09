//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $Workfile:     $
// $Date:         $
//
//-----------------------------------------------------------------------------
// $Log: $
//
// $NoKeywords: $
//=============================================================================//

#define	USED

#ifdef _WIN32
#include <windows.h>
#else
// tf2-skate: POSIX threads for the Linux build of the map tools.
#include <pthread.h>
#include <unistd.h>
#endif
#include "cmdlib.h"
#define NO_THREAD_NAMES
#include "threads.h"
#include "pacifier.h"

#define	MAX_THREADS	16


class CRunThreadsData
{
public:
	int m_iThread;
	void *m_pUserData;
	RunThreadsFn m_Fn;
};

CRunThreadsData g_RunThreadsData[MAX_THREADS];


int		dispatch;
int		workcount;
qboolean		pacifier;

qboolean	threaded;
bool g_bLowPriorityThreads = false;

#ifdef _WIN32
HANDLE g_ThreadHandles[MAX_THREADS];
#else
pthread_t g_ThreadHandles[MAX_THREADS];
#endif



/*
=============
GetThreadWork

=============
*/
int	GetThreadWork (void)
{
	int	r;

	ThreadLock ();

	if (dispatch == workcount)
	{
		ThreadUnlock ();
		return -1;
	}

	UpdatePacifier( (float)dispatch / workcount );

	r = dispatch;
	dispatch++;
	ThreadUnlock ();

	return r;
}


ThreadWorkerFn workfunction;

void ThreadWorkerFunction( int iThread, void *pUserData )
{
	int		work;

	while (1)
	{
		work = GetThreadWork ();
		if (work == -1)
			break;
		 
		workfunction( iThread, work );
	}
}

void RunThreadsOnIndividual (int workcnt, qboolean showpacifier, ThreadWorkerFn func)
{
	if (numthreads == -1)
		ThreadSetDefault ();
	
	workfunction = func;
	RunThreadsOn (workcnt, showpacifier, ThreadWorkerFunction);
}


/*
===================================================================

WIN32

===================================================================
*/

int		numthreads = -1;
static int enter;

#ifdef _WIN32
CRITICAL_SECTION		crit;
class CCritInit
{
public:
	CCritInit()
	{
		InitializeCriticalSection (&crit);
	}
} g_CritInit;
#define EnterCrit()	EnterCriticalSection( &crit )
#define LeaveCrit()	LeaveCriticalSection( &crit )
#else
static pthread_mutex_t crit = PTHREAD_MUTEX_INITIALIZER;
#define EnterCrit()	pthread_mutex_lock( &crit )
#define LeaveCrit()	pthread_mutex_unlock( &crit )
#endif



void SetLowPriority()
{
#ifdef _WIN32
	SetPriorityClass( GetCurrentProcess(), IDLE_PRIORITY_CLASS );
#else
	if ( nice( 19 ) == -1 ) {}
#endif
}


void ThreadSetDefault (void)
{
	if (numthreads == -1)	// not set manually
	{
#ifdef _WIN32
		SYSTEM_INFO info;
		GetSystemInfo (&info);
		numthreads = info.dwNumberOfProcessors;
#else
		numthreads = (int)sysconf( _SC_NPROCESSORS_ONLN );
#endif
		if (numthreads < 1 || numthreads > 32)
			numthreads = 1;
	}

	Msg ("%i threads\n", numthreads);
}


void ThreadLock (void)
{
	if (!threaded)
		return;
	EnterCrit();
	if (enter)
		Error ("Recursive ThreadLock\n");
	enter = 1;
}

void ThreadUnlock (void)
{
	if (!threaded)
		return;
	if (!enter)
		Error ("ThreadUnlock without lock\n");
	enter = 0;
	LeaveCrit();
}


// This runs in the thread and dispatches a RunThreadsFn call.
#ifdef _WIN32
DWORD WINAPI InternalRunThreadsFn( LPVOID pParameter )
#else
static void *InternalRunThreadsFn( void *pParameter )
#endif
{
	CRunThreadsData *pData = (CRunThreadsData*)pParameter;
	pData->m_Fn( pData->m_iThread, pData->m_pUserData );
	return 0;
}


void RunThreads_Start( RunThreadsFn fn, void *pUserData, ERunThreadsPriority ePriority )
{
	Assert( numthreads > 0 );
	threaded = true;

	if ( numthreads > MAX_TOOL_THREADS )
		numthreads = MAX_TOOL_THREADS;

	for ( int i=0; i < numthreads ;i++ )
	{
		g_RunThreadsData[i].m_iThread = i;
		g_RunThreadsData[i].m_pUserData = pUserData;
		g_RunThreadsData[i].m_Fn = fn;

#ifndef _WIN32
		pthread_create( &g_ThreadHandles[i], NULL, InternalRunThreadsFn, &g_RunThreadsData[i] );
		(void)ePriority;
#else
		DWORD dwDummy;
		g_ThreadHandles[i] = CreateThread(
		   NULL,	// LPSECURITY_ATTRIBUTES lpsa,
		   0,		// DWORD cbStack,
		   InternalRunThreadsFn,	// LPTHREAD_START_ROUTINE lpStartAddr,
		   &g_RunThreadsData[i],	// LPVOID lpvThreadParm,
		   0,			// DWORD fdwCreate,
		   &dwDummy );

		if ( ePriority == k_eRunThreadsPriority_UseGlobalState )
		{
			if( g_bLowPriorityThreads )
				SetThreadPriority( g_ThreadHandles[i], THREAD_PRIORITY_LOWEST );
		}
		else if ( ePriority == k_eRunThreadsPriority_Idle )
		{
			SetThreadPriority( g_ThreadHandles[i], THREAD_PRIORITY_IDLE );
		}
#endif
	}
}


void RunThreads_End()
{
#ifdef _WIN32
	WaitForMultipleObjects( numthreads, g_ThreadHandles, TRUE, INFINITE );
	for ( int i=0; i < numthreads; i++ )
		CloseHandle( g_ThreadHandles[i] );
#else
	for ( int i=0; i < numthreads; i++ )
		pthread_join( g_ThreadHandles[i], NULL );
#endif

	threaded = false;
}
	

/*
=============
RunThreadsOn
=============
*/
void RunThreadsOn( int workcnt, qboolean showpacifier, RunThreadsFn fn, void *pUserData )
{
	int		start, end;

	start = Plat_FloatTime();
	dispatch = 0;
	workcount = workcnt;
	StartPacifier("");
	pacifier = showpacifier;

#ifdef _PROFILE
	threaded = false;
	(*func)( 0 );
	return;
#endif

	
	RunThreads_Start( fn, pUserData );
	RunThreads_End();


	end = Plat_FloatTime();
	if (pacifier)
	{
		EndPacifier(false);
		printf (" (%i)\n", end-start);
	}
}


